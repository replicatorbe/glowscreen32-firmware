#include "jeedom_client.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "../ui/icons.h"

// Chemin du point d'entrée, figé par le contrat d'API depuis la v1.1.
// (la v1 visait core/api/ ; la convention réelle des plugins est core/php/)
static const char *JEEDOM_API_PATH = "/plugins/glowscreen32/core/php/api.php";

// En-tête d'authentification (contrat v1.1). Le repli `?apikey=<clé>` en query
// string reste toléré côté serveur, mais on ne l'utilise pas : la clé finirait
// en clair dans les journaux d'Apache, à raison d'un appel toutes les 30 s.
static const char *JEEDOM_APIKEY_HEADER = "X-GLOWSCREEN32-APIKEY";

// En-tête de négociation (contrat v2.0). Un plugin v1.4 l'ignore et répond en
// schéma 1, que ce client sait toujours lire.
static const char *JEEDOM_SCHEMA_HEADER = "X-GLOWSCREEN32-SCHEMA";

// Garde-fou anti-réponse aberrante. Le plus gros layout légitime (4 pages,
// 32 boutons) pèse ~4,8 ko ; 8 ko laissent de la marge sans menacer les
// 320 ko de RAM interne (aucune PSRAM sur cette carte).
static const size_t JEEDOM_JSON_MAX = 8192;

// Couleur de repli d'un bouton dont `color` est absent ou illisible.
static const uint16_t JEEDOM_COLOR_FALLBACK = 0x3A5F;

// --- Flux borné -------------------------------------------------------------
//
// ⚠️ LE GARDE-FOU DE LA v1.4 NE GARDAIT RIEN. Il testait
// `http.getSize() > JEEDOM_JSON_MAX`, or `getSize()` vaut **-1** quand le
// serveur répond en `Transfer-Encoding: chunked` (pas de `Content-Length`).
// `-1 > 8192` est faux : la désérialisation devenait NON BORNÉE, précisément
// dans le cas où le plafond aurait servi.
//
// On compte donc les octets RÉELLEMENT LUS. Au-delà de la limite, le flux se
// comporte comme une fin de fichier : ArduinoJson s'arrête et signale
// `IncompleteInput`, qu'on traduit en ErrParse.
class BoundedStream : public Stream {
 public:
  BoundedStream(WiFiClient &src, size_t limit) : _src(src), _left(limit) {}

  int available() override {
    const int a = _src.available();
    return (a > (int)_left) ? (int)_left : a;
  }
  int peek() override { return _left ? _src.peek() : -1; }
  int read() override {
    if (_left == 0) return -1;
    const int c = _src.read();
    if (c >= 0) {
      _left--;
      _consumed++;
    }
    return c;
  }
  // ArduinoJson lit TOUT par readBytes(), jusqu'au caractère isolé.
  //
  // ⚠️ L'ATTENTE DOIT CÉDER LA MAIN. WiFiClient ne redéfinit pas readBytes() :
  // déléguer au flux retombait sur Stream::timedRead(), qui attend chaque
  // octet en BOUCLE ACTIVE, sans jamais bloquer, jusqu'au timeout de la
  // requête — `wait` + 5 s, soit 30 s en attente longue. Sur le cœur 0, où
  // tournent les tâches réseau depuis la 2.2.0, cela affame la tâche IDLE0,
  // surveillée par le chien de garde AVEC panique au bout de 5 s : un corps de
  // réponse retardé par des retransmissions Wi-Fi suffisait à faire redémarrer
  // la carte. Constaté sur la 2.2.1 pendant sa période d'essai (rst=wdt), sur
  // un code identique à la 2.2.0 : intermittent, selon la qualité radio.
  // Jusqu'en 2.1, la même boucle tournait sur le cœur 1, non surveillé : elle
  // ne faisait pas redémarrer, elle FIGEAIT l'écran.
  size_t readBytes(char *buffer, size_t length) override {
    if (length > _left) length = _left;
    size_t got = 0;
    uint32_t lastDataMs = millis();
    while (got < length) {
      const int avail = _src.available();
      if (avail > 0) {
        size_t want = length - got;
        if (want > (size_t)avail) want = (size_t)avail;
        const int n = _src.read((uint8_t *)buffer + got, want);
        if (n > 0) {
          got += (size_t)n;
          lastDataMs = millis();
          continue;
        }
      }
      // Connexion fermée et plus rien en tampon : rien ne viendra, inutile
      // d'attendre le timeout complet.
      if (avail <= 0 && !_src.connected()) break;
      if (millis() - lastDataMs >= _src.getTimeout()) break;
      vTaskDelay(1);  // bloque : IDLE0 tourne, le chien de garde est nourri
    }
    _left -= got;
    _consumed += got;
    return got;
  }
  size_t write(uint8_t) override { return 0; }
  void flush() override {}

  bool exhausted() const { return _left == 0; }
  size_t consumed() const { return _consumed; }

 private:
  WiFiClient &_src;
  size_t _left;
  size_t _consumed = 0;
};

// Libellés volontairement courts : le bandeau n'offre qu'une petite centaine
// de pixels. Le détail complet part sur le port série.
const char *jeedomResultLabel(JeedomResult r) {
  switch (r) {
    case JeedomResult::Ok:                 return "API OK";
    case JeedomResult::ErrNoApikey:        return "Sans clé";
    case JeedomResult::ErrBadApikey:       return "Clé API";
    case JeedomResult::ErrUnknownDevice:   return "Écran ?";
    case JeedomResult::ErrUnknownButton:   return "Bouton ?";
    case JeedomResult::ErrFirmwareMissing: return "Firmware ?";
    case JeedomResult::ErrBadRequest:      return "Requête";
    case JeedomResult::ErrReadOnly:        return "Lecture seule";
    case JeedomResult::ErrServer:          return "Serveur";
    case JeedomResult::ErrTransport:       return "Injoignable";
    case JeedomResult::ErrParse:           return "Réponse";
    case JeedomResult::ErrNoWifi:          return "Hors ligne";
  }
  return "?";
}

bool jeedomResultIsPermanent(JeedomResult r) {
  // `firmware_unavailable` : le plugin annonce une mise à jour mais le fichier
  // a disparu. Réessayer en boucle serrée n'y changera rien.
  return r == JeedomResult::ErrNoApikey || r == JeedomResult::ErrBadApikey ||
         r == JeedomResult::ErrBadRequest || r == JeedomResult::ErrFirmwareMissing ||
         r == JeedomResult::ErrReadOnly;
}

// Traduit le champ `error` du contrat en code interne.
static JeedomResult mapError(const char *code) {
  if (!code) return JeedomResult::ErrServer;
  if (!strcmp(code, "bad_apikey"))     return JeedomResult::ErrBadApikey;
  if (!strcmp(code, "unknown_device")) return JeedomResult::ErrUnknownDevice;
  if (!strcmp(code, "unknown_button")) return JeedomResult::ErrUnknownButton;
  if (!strcmp(code, "bad_request"))    return JeedomResult::ErrBadRequest;
  if (!strcmp(code, "firmware_unavailable")) return JeedomResult::ErrFirmwareMissing;
  if (!strcmp(code, "read_only"))      return JeedomResult::ErrReadOnly;
  return JeedomResult::ErrServer;
}

// « #2D7FF9 » -> RGB565. Tolère l'absence de « # » et les couleurs vides.
static uint16_t hexToRgb565(const char *hex, uint16_t fallback) {
  if (!hex) return fallback;
  if (*hex == '#') hex++;
  if (strlen(hex) < 6) return fallback;
  char *end = nullptr;
  const long v = strtol(hex, &end, 16);
  if (end == hex) return fallback;
  const uint8_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Copie sûre d'une chaîne JSON ASCII (MAC, URL, empreinte, version) dans un
// champ à taille fixe. ⚠️ Pour du texte affiché, utiliser layoutCopyUtf8 :
// strncpy couperait un caractère accentué en deux.
static void copyField(char *dst, size_t len, const char *src) {
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, len - 1);
  dst[len - 1] = '\0';
}

// `state` du contrat : 0, 1, ou null (commande sans état).
static int8_t readState(JsonVariantConst v) {
  if (v.isNull()) return BTN_STATE_UNKNOWN;
  return (int8_t)(v.as<int>() ? BTN_STATE_ON : BTN_STATE_OFF);
}

// La clé du gabarit ne doit jamais partir sur le réseau : on la reconnaît
// pour afficher « Sans clé » plutôt que de boucler sur des 401 silencieux.
bool jeedomApikeyConfigured(const char *apikey) {
  if (!apikey || apikey[0] == '\0') return false;
  if (strstr(apikey, "A_REMPLIR") != nullptr) return false;
  if (strcmp(apikey, "clef-api-du-plugin-glowscreen32") == 0) return false;
  return strlen(apikey) >= 8;  // les clés Jeedom font une trentaine de caractères
}

void JeedomClient::begin(const char *host, const char *apikey, const char *device) {
  copyField(_host, sizeof(_host), host);
  copyField(_apikey, sizeof(_apikey), apikey);
  copyField(_device, sizeof(_device), device);
  _apikeyOk = jeedomApikeyConfigured(_apikey);
}

// Encode une valeur de query string (RFC 3986 : seuls A-Z a-z 0-9 - _ . ~
// passent tels quels). Sert au SSID — texte libre, espaces et accents
// compris — et à `rev`, opaque : rien ne garantit qu'elle reste alphanumérique.
static void appendEncoded(String &u, const char *v) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  for (const uint8_t *c = (const uint8_t *)v; c && *c; c++) {
    if (isalnum(*c) || *c == '-' || *c == '_' || *c == '.' || *c == '~') {
      u += (char)*c;
    } else {
      u += '%';
      u += HEX_DIGITS[*c >> 4];
      u += HEX_DIGITS[*c & 0x0F];
    }
  }
}

// Texte borné en caractères (contrat v3.0), troncature journalisée.
static void copyBounded(char *dst, size_t dstLen, const char *src, size_t maxChars,
                        const char *what) {
  if (layoutCopyUtf8Chars(dst, dstLen, src, maxChars)) {
    Serial.printf("[api] ⚠️ %s tronqué à %u caractères : \"%s\"\n", what, (unsigned)maxChars,
                  dst);
  }
}

String JeedomClient::url(const char *action) const {
  String u;
  u.reserve(160);
  u += "http://";
  u += _host;
  u += JEEDOM_API_PATH;
  u += "?action=";
  u += action;
  u += "&device=";
  u += _device;
  return u;
}

JeedomResult JeedomClient::request(const String &u, uint8_t attempts, JsonDocument &doc,
                                   uint16_t timeoutMs, uint8_t schema) {
  if (!_apikeyOk) {
    // Rien à tenter : la clé n'est pas configurée (voir src/secrets.h).
    _last = JeedomResult::ErrNoApikey;
    return _last;
  }
  if (WiFi.status() != WL_CONNECTED) {
    _last = JeedomResult::ErrNoWifi;
    return _last;
  }
  if (attempts == 0) attempts = 1;
  _lastJsonOk = false;

  JeedomResult result = JeedomResult::ErrTransport;

  for (uint8_t attempt = 1; attempt <= attempts; attempt++) {
    HTTPClient http;
    // L'établissement de la connexion n'a pas à attendre comme la réponse :
    // en attente longue, le délai de lecture vaut `wait` + 5 s, mais un hôte
    // injoignable doit se déclarer en 5 s comme avant.
    http.setConnectTimeout(timeoutMs < JEEDOM_TIMEOUT_MS ? timeoutMs : JEEDOM_TIMEOUT_MS);
    http.setTimeout(timeoutMs);
    http.setReuse(false);
    // HTTP/1.0 : pas de `Transfer-Encoding: chunked`. Le corps arrive brut,
    // borné par Content-Length ou par la fermeture, et BoundedStream compte
    // exactement ce qu'ArduinoJson lit — sans décodage de blocs intercalé.
    http.useHTTP10(true);

    if (!http.begin(u)) {
      result = JeedomResult::ErrTransport;
      http.end();
    } else {
      http.addHeader(JEEDOM_APIKEY_HEADER, _apikey);
      // Contrat v2.0 : on annonce ce qu'on sait lire. Un plugin v1.4 ignore
      // cet en-tête et répond en schéma 1 — parfaitement lisible ici.
      // Un `press` annonce le schéma de SON `id`, pas le plus haut connu.
      http.addHeader(JEEDOM_SCHEMA_HEADER, String(schema ? schema : 1));

      const int code = http.GET();
      if (code <= 0) {
        // Pas de réponse : timeout, DNS, hôte injoignable -> on retente.
        Serial.printf("[api] transport KO (%s), tentative %u/%u\n",
                      http.errorToString(code).c_str(), attempt, attempts);
        result = JeedomResult::ErrTransport;
      } else if (http.getSize() > (int)JEEDOM_JSON_MAX) {
        // Chemin rapide quand `Content-Length` est présent : on n'ouvre même
        // pas le flux. Ce test ne SUFFIT pas (voir BoundedStream).
        Serial.printf("[api] réponse annoncée à %d octets : refusée\n", http.getSize());
        result = JeedomResult::ErrParse;
      } else {
        // Le corps est toujours du JSON, y compris pour les erreurs.
        doc.clear();
        BoundedStream bounded(http.getStream(), JEEDOM_JSON_MAX);
        // NestingLimit 8 : `{pages:[{buttons:[{…}]}]}` occupe 5 niveaux, il en
        // reste donc trois de marge. La v1.4 était à 6, soit un seul cran.
        DeserializationError err =
            deserializeJson(doc, bounded, DeserializationOption::NestingLimit(8));
        // Une réponse lisible, quel que soit son sens : c'est la preuve 5 de
        // validation pour `action=firmware` (contrat v3.0).
        _lastJsonOk = !err;
        if (err) {
          if (bounded.exhausted()) {
            Serial.printf("[api] réponse tronquée au plafond de %u octets\n",
                          (unsigned)JEEDOM_JSON_MAX);
          } else {
            Serial.printf("[api] JSON illisible (%s), HTTP %d, %u octets lus\n",
                          err.c_str(), code, (unsigned)bounded.consumed());
          }
          result = JeedomResult::ErrParse;
        } else if (code == 200 && doc["ok"].as<bool>()) {
          // Succès : HTTP 200 ET ok:true, comme l'impose le contrat.
          http.end();
          _last = JeedomResult::Ok;
          return _last;
        } else {
          result = mapError(doc["error"].as<const char *>());
          const char *code_str = doc["error"].as<const char *>();
          if (result == JeedomResult::ErrBadRequest) {
            // 400 = requête malformée : défaut de programmation d'un des deux
            // côtés, pas un incident réseau. ⚠️ Le plugin rabat AUSSI toute
            // exception PHP sur bad_request : un défaut serveur est donc
            // indiscernable ici, c'est le journal Jeedom qui tranche.
            Serial.printf("[api] BAD_REQUEST sur %s -- défaut de programmation\n",
                          u.c_str());
          } else {
            Serial.printf("[api] erreur %s (HTTP %d)\n", code_str ? code_str : "?", code);
          }
          // Une erreur applicative est définitive : inutile de réessayer.
          http.end();
          _last = result;
          return _last;
        }
      }
      http.end();
    }

    if (attempt < attempts) delay(JEEDOM_RETRY_DELAY_MS);
  }

  _last = result;
  return _last;
}

// --- Lecture d'un bouton ----------------------------------------------------

// `value`/`tone` d'une tuile `view` (contrat v3.0), en entrée de `layout`
// (champs du bouton) comme de `ping` (objet {"v", "t"} de `values`).
// `value` null -> inconnue, la carte affiche « — ». `tone` absent ou inconnu
// -> neutral.
static void readTileValue(JsonVariantConst value, JsonVariantConst tone, TileValue &out) {
  memset(&out, 0, sizeof(out));
  const char *v = value.as<const char *>();
  if (v) {
    // Bornes réappliquées : 16 caractères, coupés sur une frontière UTF-8.
    copyBounded(out.text, sizeof(out.text), v, TILE_VALUE_CHARS, "value");
    out.known = 1;
  }
  const char *t = tone.as<const char *>();
  out.tone = TONE_NEUTRAL;
  if (t) {
    if (!strcmp(t, "ok")) out.tone = TONE_OK;
    else if (!strcmp(t, "warn")) out.tone = TONE_WARN;
    else if (!strcmp(t, "alert")) out.tone = TONE_ALERT;
  }
}

// Champs communs aux deux schémas. `page` et `slot` sont posés par l'appelant.
static void parseButtonCommon(JsonVariantConst v, LayoutButton &b, uint8_t rank,
                              TileValue &value) {
  // `id` est opaque (contrat v1.3) : on le stocke et on le renverra tel quel.
  // S'il manque, on retombe sur le rang — sans jamais lui prêter de sens.
  b.id = v["id"] | (int32_t)rank;
  copyBounded(b.label, sizeof(b.label), v["label"].as<const char *>(), LAYOUT_TEXT_CHARS,
              "label");
  b.color = hexToRgb565(v["color"].as<const char *>(), JEEDOM_COLOR_FALLBACK);
  // Vocabulaire d'icônes FERMÉ : un nom inconnu vaut ICON_NONE et la tuile se
  // rabat sur son seul libellé. La conversion se fait ici, une fois : le champ
  // coûte 1 octet dans le modèle au lieu des 12 de la chaîne en v1.4.
  b.icon = (uint8_t)iconFromName(v["icon"].as<const char *>());

  const char *mode = v["mode"].as<const char *>();
  b.mode = BTN_MODE_ACTION;  // absent -> action, le comportement le plus prudent
  if (mode) {
    if (!strcmp(mode, "toggle")) b.mode = BTN_MODE_TOGGLE;
    else if (!strcmp(mode, "nav")) b.mode = BTN_MODE_NAV;
    // Une tuile `view` ne déclenche RIEN. Reconnue dans tout schéma : un
    // serveur qui en enverrait par erreur en schéma 2 ne doit pas la voir
    // traitée comme un bouton — le défaut que le schéma 3 existe pour éviter.
    else if (!strcmp(mode, "view")) b.mode = BTN_MODE_VIEW;
  }
  b.state = readState(v["state"]);
  b.target = 0;
  if (b.mode == BTN_MODE_VIEW) {
    b.state = BTN_STATE_UNKNOWN;  // toujours null (contrat)
    readTileValue(v["value"], v["tone"], value);
  } else {
    memset(&value, 0, sizeof(value));
  }
}

// Schéma 1 : un tableau plat, tout sur la page 0, slot = rang.
void JeedomClient::parseFlatButtons(JsonArrayConst arr, Layout &out, TileValues &values) {
  uint8_t n = 0;
  bool truncated = false;
  for (JsonVariantConst v : arr) {
    if (n >= LAYOUT_MAX_BUTTONS) { truncated = true; break; }
    LayoutButton &b = out.buttons[n];
    parseButtonCommon(v, b, n, values.v[n]);
    if (b.mode == BTN_MODE_NAV) b.mode = BTN_MODE_ACTION;  // pas de pages en v1
    b.page = 0;
    b.slot = n;
    n++;
  }
  out.count = n;
  out.pageCount = 1;
  if (truncated) {
    Serial.printf("[api] ⚠️ plus de %u boutons reçus : les suivants sont IGNORÉS\n",
                  LAYOUT_MAX_BUTTONS);
  }
}

// Schéma 2 : des pages, chacune avec ses boutons. L'index de page est la
// POSITION dans le tableau, pas le champ `id` : une numérotation creuse
// casserait `parent` et les pastilles de navigation.
void JeedomClient::parsePages(JsonArrayConst arr, Layout &out, TileValues &values) {
  uint8_t n = 0;         // index de stockage, global à l'écran
  uint8_t pageIndex = 0;
  const uint8_t slots = layoutSlots(out);
  bool truncated = false;

  for (JsonVariantConst pv : arr) {
    if (pageIndex >= LAYOUT_MAX_PAGES) { truncated = true; break; }
    LayoutPage &p = out.pages[pageIndex];
    copyBounded(p.title, sizeof(p.title), pv["title"].as<const char *>(), LAYOUT_TEXT_CHARS,
                "title");

    JsonVariantConst parent = pv["parent"];
    p.hasParent = 0;
    p.parent = 0;
    if (!parent.isNull()) {
      const int par = parent.as<int>();
      // Une page qui se déclare sa propre parente piégerait l'utilisateur
      // dans un retour qui ne remonte nulle part.
      if (par >= 0 && par < LAYOUT_MAX_PAGES && par != (int)pageIndex) {
        p.parent = (uint8_t)par;
        p.hasParent = 1;
      }
    }

    uint8_t rankInPage = 0;
    for (JsonVariantConst v : pv["buttons"].as<JsonArrayConst>()) {
      if (n >= LAYOUT_MAX_BUTTONS) { truncated = true; break; }
      LayoutButton &b = out.buttons[n];
      parseButtonCommon(v, b, n, values.v[n]);
      b.page = pageIndex;

      const int slot = v["slot"] | (int)rankInPage;
      if (slot < 0 || slot >= slots) {
        Serial.printf("[api] ⚠️ bouton \"%s\" en case %d hors grille %ux%u : invisible\n",
                      b.label, slot, out.cols, out.rows);
        b.slot = (uint8_t)LAYOUT_MAX_SLOTS;  // hors grille : jamais dessiné
      } else {
        b.slot = (uint8_t)slot;
      }

      if (b.mode == BTN_MODE_NAV) {
        const int target = v["page"] | -1;
        if (target < 0 || target >= LAYOUT_MAX_PAGES) {
          Serial.printf("[api] ⚠️ bouton \"%s\" : cible de navigation %d invalide\n",
                        b.label, target);
          b.target = pageIndex;  // sans effet, plutôt qu'un saut imprévisible
        } else {
          b.target = (uint8_t)target;
        }
        // Un bouton de navigation n'a pas d'état : il ne pilote rien.
        b.state = BTN_STATE_UNKNOWN;
      }

      n++;
      rankInPage++;
    }
    pageIndex++;
  }

  out.count = n;
  out.pageCount = pageIndex ? pageIndex : 1;
  if (truncated) {
    // Contrat v2.0 : une troncature est journalisée, JAMAIS silencieuse. En
    // v1.4 elle était muette et un écran pouvait afficher 6 boutons sur 8
    // sans que personne ne puisse s'en apercevoir.
    Serial.printf("[api] ⚠️ mise en page tronquée (max %u pages, %u boutons)\n",
                  LAYOUT_MAX_PAGES, LAYOUT_MAX_BUTTONS);
  }
}

// `features` et `rev` (contrat v2.2), communs à `layout` et `ping`.
static void parseExtras(JsonDocument &doc, ServerExtras &x) {
  x = ServerExtras();
  JsonVariantConst f = doc["features"];
  if (f.is<JsonObjectConst>()) {
    x.hasFeatures = true;
    long w = f["wait"].is<long>() ? f["wait"].as<long>() : 0;
    if (w < 0) w = 0;
    if (w > JEEDOM_WAIT_MAX_S) w = JEEDOM_WAIT_MAX_S;
    x.features.wait = (uint16_t)w;
    x.features.cmd = f["cmd"] | false;
  }
  const char *rev = doc["rev"].as<const char *>();
  if (rev && strlen(rev) < sizeof(x.rev)) {
    copyField(x.rev, sizeof(x.rev), rev);
  } else if (rev) {
    // Tronquer une valeur opaque la rendrait fausse : le serveur la verrait
    // toujours « différente » et répondrait aussitôt. On l'ignore — sans
    // `rev`, la carte reste à la cadence `poll` (voir net_tasks).
    Serial.printf("[api] rev de %u caractères (> 16) ignorée\n", (unsigned)strlen(rev));
  }
}

JeedomResult JeedomClient::fetchLayout(Layout &out, TileValues &values, BannerInfo &banner,
                                       ServerExtras &extras, uint8_t attempts) {
  JsonDocument doc;
  const JeedomResult r = request(url("layout"), attempts, doc);
  if (r != JeedomResult::Ok) return r;

  // ⚠️ `out` est rempli en place : `Layout` fait ~1,6 ko. C'est un tampon
  // appartenant à la tâche réseau (double tampon, voir net/net_tasks), jamais
  // la mise en page affichée, et jamais une variable locale.
  layoutClear(out);
  memset(&values, 0, sizeof(values));

  copyField(out.device, sizeof(out.device), doc["device"].as<const char *>());
  // Le champ `device` doit être l'écho de notre MAC ; s'il manque ou diffère,
  // c'est notre propre identité qui fait foi (cache NVS lié à cette carte).
  if (strcmp(out.device, _device) != 0) {
    if (out.device[0] != '\0') {
      Serial.printf("[api] device renvoyé \"%s\" != \"%s\"\n", out.device, _device);
    }
    copyField(out.device, sizeof(out.device), _device);
  }
  copyBounded(out.name, sizeof(out.name), doc["name"].as<const char *>(), LAYOUT_TEXT_CHARS,
              "name");
  if (out.name[0] == '\0') layoutCopyUtf8(out.name, sizeof(out.name), "GlowScreen");

  out.version = doc["version"] | 0;
  out.poll = doc["poll"] | 30;
  // Bornes du contrat [5, 3600]. Hors bornes : 30, PAS la borne — la règle
  // du serveur, appliquée à l'identique (contrat). Jusqu'en 2.3.0 la carte
  // ramenait à la borne, et un `poll` aberrant de 4 000 donnait 3 600 s.
  if (out.poll < 5 || out.poll > 3600) {
    Serial.printf("[api] ⚠️ poll %ld hors [5, 3600] : 30 s\n", (long)out.poll);
    out.poll = 30;
  }

  // Schéma réellement servi. Absent -> 1, conformément au contrat.
  out.schema = (uint8_t)(doc["schema"] | 1);
  if (out.schema > LAYOUT_SCHEMA) out.schema = LAYOUT_SCHEMA;
  _serverSchema = out.schema;

  // Grille : défaut 3x3 (contrat v2.0) ; absente -> 3x2, la disposition v1.
  JsonVariantConst grid = doc["grid"];
  if (grid.is<JsonObjectConst>()) {
    const int c = grid["cols"] | 3;
    const int rw = grid["rows"] | 3;
    out.cols = (uint8_t)((c >= 3 && c <= LAYOUT_MAX_COLS) ? c : 3);
    out.rows = (uint8_t)((rw >= 2 && rw <= LAYOUT_MAX_ROWS) ? rw : 3);
    if (out.cols != c || out.rows != rw) {
      // Contrat : un dépassement est journalisé, jamais absorbé en silence.
      Serial.printf("[api] ⚠️ grille %dx%d hors bornes : %ux%u\n", c, rw, out.cols, out.rows);
    }
  } else if (out.schema < 2) {
    out.cols = 3;
    out.rows = 2;  // disposition historique du schéma 1
  }

  JsonVariantConst ui = doc["ui"];
  out.swipe = ui.is<JsonObjectConst>() ? (ui["swipe"] | false) : false;
  out.clock = ui.is<JsonObjectConst>() ? (ui["clock"] | false) : false;
  // Schéma 3 : écran en lecture seule. Défaut false.
  out.readonly = ui.is<JsonObjectConst>() ? (ui["readonly"] | false) : false;

  // Champs VOLATILS : ils repartent dans `banner`, jamais dans `Layout`.
  // `info` est du texte (« 21.4 °C ») : copie UTF-8, pas strncpy.
  copyBounded(banner.info, sizeof(banner.info), doc["info"].as<const char *>(),
              JEEDOM_INFO_CHARS, "info");
  banner.time = doc["time"] | 0UL;
  banner.tzoffset = doc["tzoffset"] | 0;
  banner.hasTime = banner.time != 0;

  JsonArrayConst pages = doc["pages"].as<JsonArrayConst>();
  if (!pages.isNull() && pages.size() > 0) {
    parsePages(pages, out, values);
  } else {
    parseFlatButtons(doc["buttons"].as<JsonArrayConst>(), out, values);
  }

  // `states` de niveau layout (schéma 2) : indexé par l'`id` GLOBAL.
  JsonArrayConst states = doc["states"].as<JsonArrayConst>();
  if (!states.isNull()) {
    for (uint8_t i = 0; i < out.count; i++) {
      LayoutButton &b = out.buttons[i];
      if (b.mode == BTN_MODE_NAV || b.mode == BTN_MODE_VIEW) continue;
      if (b.id >= 0 && (size_t)b.id < states.size()) {
        b.state = readState(states[(size_t)b.id]);
      }
    }
  }

  parseExtras(doc, extras);

  Serial.printf("[api] layout \"%s\" v%ld schéma %u : %u boutons, %u page(s), "
                "grille %ux%u, poll %lds%s%s%s, attente longue %us\n",
                out.name, (long)out.version, out.schema, out.count, out.pageCount,
                out.cols, out.rows, (long)out.poll, out.swipe ? ", balayage" : "",
                out.clock ? ", horloge" : "", out.readonly ? ", LECTURE SEULE" : "",
                extras.features.wait);
  return JeedomResult::Ok;
}

JeedomResult JeedomClient::press(int32_t id, uint8_t schema, PressInfo &out, uint8_t attempts) {
  JsonDocument doc;
  String u = url("press");
  u += "&id=";
  u += id;  // valeur opaque, renvoyée telle quelle
  const JeedomResult r = request(u, attempts, doc, JEEDOM_PRESS_TIMEOUT_MS, schema);
  if (r != JeedomResult::Ok) return r;

  out.state = readState(doc["state"]);
  // `pending` absent (plugin antérieur à la v1.3) -> on suppose « en attente »,
  // l'hypothèse prudente : le ping suivant tranchera de toute façon.
  out.pending = doc["pending"] | true;
  Serial.printf("[api] press #%ld -> état attendu %d%s\n", (long)id, (int)out.state,
                out.pending ? " (en attente de confirmation)" : "");
  return JeedomResult::Ok;
}

JeedomResult JeedomClient::checkFirmware(const char *fw, FirmwareInfo &out,
                                         uint8_t attempts) {
  JsonDocument doc;
  String u = url("firmware");
  u += "&fw=";
  u += fw;
  const JeedomResult r = request(u, attempts, doc);
  if (r != JeedomResult::Ok) return r;

  out = FirmwareInfo();
  out.update = doc["update"] | false;
  if (!out.update) return JeedomResult::Ok;  // à jour, ou OTA désactivé côté serveur

  copyField(out.version, sizeof(out.version), doc["version"].as<const char *>());
  copyField(out.url, sizeof(out.url), doc["url"].as<const char *>());
  copyField(out.sha256, sizeof(out.sha256), doc["sha256"].as<const char *>());
  out.size = doc["size"] | 0UL;

  // Une annonce incomplète n'est pas exploitable : on la refuse plutôt que de
  // télécharger un binaire qu'on ne saura pas vérifier.
  if (out.url[0] == '\0' || strlen(out.sha256) != 64) {
    Serial.println("[ota] annonce incomplète (url ou sha256 manquant) : ignorée");
    out.update = false;
    return JeedomResult::ErrParse;
  }
  Serial.printf("[ota] mise à jour annoncée : %s (%lu octets)\n", out.version,
                (unsigned long)out.size);
  return JeedomResult::Ok;
}

// Cause du dernier redémarrage, dans le vocabulaire fermé du contrat v2.2.
// `panic` ou `wdt` signalent un firmware qui plante sans que personne ne le
// voie : l'écran redémarre en deux secondes et paraît fonctionner.
static const char *resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_SW:       return "sw";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT:      return "ext";
    default:               return "other";
  }
}

// Lecture d'une commande à distance (contrat v2.2). Les bornes sont
// RÉAPPLIQUÉES ici : le serveur les applique déjà avant la mise en file, mais
// la carte ne dépend pas de sa discipline.
static void parseCmd(JsonVariantConst c, RemoteCmd &out) {
  out = RemoteCmd();
  const long seq = c["seq"] | -1L;
  const char *verb = c["do"].as<const char *>();
  if (seq < 0 || !verb) {
    Serial.println("[cmd] commande sans seq ou sans verbe : ignorée");
    return;
  }
  out.seq = (uint32_t)seq;
  copyField(out.name, sizeof(out.name), verb);

  auto bounded = [&](const char *key, long def, long lo, long hi) -> long {
    long v = c[key].is<long>() ? c[key].as<long>() : def;
    return v < lo ? lo : (v > hi ? hi : v);
  };

  if (!strcmp(verb, "reboot")) {
    out.verb = CmdVerb::Reboot;
  } else if (!strcmp(verb, "identify")) {
    out.verb = CmdVerb::Identify;
    out.duration = (uint16_t)bounded("duration", 10, 1, 120);
  } else if (!strcmp(verb, "message")) {
    out.verb = CmdVerb::Message;
    copyBounded(out.text, sizeof(out.text), c["text"].as<const char *>(),
                JEEDOM_CMD_TEXT_CHARS, "message");
    out.duration = (uint16_t)bounded("duration", 30, 1, 600);
  } else if (!strcmp(verb, "page")) {
    out.verb = CmdVerb::Page;
    out.page = (uint8_t)bounded("page", 0, 0, LAYOUT_MAX_PAGES - 1);
  } else if (!strcmp(verb, "calibrate")) {
    out.verb = CmdVerb::Calibrate;
  } else if (!strcmp(verb, "ota")) {
    out.verb = CmdVerb::Ota;
  } else if (!strcmp(verb, "wifi")) {
    out.verb = CmdVerb::Wifi;
    layoutCopyUtf8(out.ssid, sizeof(out.ssid), c["ssid"].as<const char *>());
    layoutCopyUtf8(out.pass, sizeof(out.pass), c["pass"].as<const char *>());
  } else {
    out.verb = CmdVerb::Unknown;  // journalisé par l'appelant
  }
}

JeedomResult JeedomClient::ping(PingInfo &out, const PingParams &p) {
  String u = url("ping");
  u.reserve(320);
  // Contrat v2.1 : le niveau Wi-Fi, en dBm, joint au seul appel périodique —
  // donc le seul qui en donne une mesure suivie sans rien coûter de plus.
  //
  // Les bornes sont vérifiées ICI aussi, et pas seulement côté serveur : une
  // carte qui n'a pas encore associé son Wi-Fi rend des valeurs fantaisistes,
  // et il n'y a aucune raison de les faire voyager pour se les faire refuser.
  if (p.rssi >= -120 && p.rssi <= 0) {
    u += "&rssi=";
    u += p.rssi;
  }

  // Diagnostics v2.2. Le serveur ignore sans erreur tout ce qui est absent,
  // mal formé ou hors bornes : on borne quand même ici, par principe.
  u += "&up=";
  u += (uint32_t)(esp_timer_get_time() / 1000000LL);
  u += "&rst=";
  u += resetReasonName();
  uint32_t heap = ESP.getFreeHeap();
  uint32_t blk = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  u += "&heap=";
  u += heap > 400000 ? 400000 : heap;
  u += "&blk=";
  u += blk > 400000 ? 400000 : blk;
  u += "&ip=";
  u += WiFi.localIP().toString();
  u += "&ssid=";
  appendEncoded(u, WiFi.SSID().c_str());

  // `rev` part sur CHAQUE ping dès qu'on en connaît une, attente longue ou
  // non : le plugin ne livre une commande à distance qu'à une requête qui la
  // porte (contrat v2.2, « Commandes à distance »). Sans `wait`, elle ne fait
  // retenir rien : la réponse reste immédiate, comme en v2.1.
  if (p.rev && p.rev[0]) {
    u += "&rev=";
    appendEncoded(u, p.rev);
  }
  // Attente longue : `wait` n'a de sens qu'accompagné de `rev`, sinon le
  // serveur répond aussitôt.
  uint16_t timeoutMs = JEEDOM_TIMEOUT_MS;
  if (p.wait > 0 && p.rev && p.rev[0]) {
    const uint16_t w = p.wait > JEEDOM_WAIT_MAX_S ? JEEDOM_WAIT_MAX_S : p.wait;
    u += "&wait=";
    u += w;
    timeoutMs = (uint16_t)(w * 1000U + JEEDOM_WAIT_MARGIN_MS);
  }

  JsonDocument doc;
  const JeedomResult r = request(u, 1, doc, timeoutMs);
  if (r != JeedomResult::Ok) return r;

  out.version = doc["version"] | 0;
  out.hasStates = false;
  out.stateCount = 0;

  out.banner.time = doc["time"] | 0UL;
  out.banner.tzoffset = doc["tzoffset"] | 0;
  out.banner.hasTime = out.banner.time != 0;
  copyBounded(out.banner.info, sizeof(out.banner.info), doc["info"].as<const char *>(),
              JEEDOM_INFO_CHARS, "info");

  // `states` est arrivé en v1.2 : absent si le plugin n'est pas à jour.
  JsonVariantConst raw = doc["states"];
  if (raw.is<JsonArrayConst>()) {
    out.hasStates = true;
    for (JsonVariantConst v : raw.as<JsonArrayConst>()) {
      if (out.stateCount >= LAYOUT_MAX_BUTTONS) {
        // Plus d'états que le contrat n'autorise de boutons : tableau suspect.
        // L'appelant le rejettera par comparaison des tailles et forcera un
        // `layout` complet — la règle qui a rattrapé la v1.2, conservée telle
        // quelle en v2.0.
        Serial.printf("[api] ⚠️ states dépasse %u entrées : tableau rejeté\n",
                      LAYOUT_MAX_BUTTONS);
        out.stateCount = LAYOUT_MAX_BUTTONS + 1;
        break;
      }
      out.states[out.stateCount++] = readState(v);
    }
  }

  // `values` (schéma 3) : même règle que `states` — la taille est jugée par
  // l'appelant, un tableau trop long est rejeté ici.
  out.hasValues = false;
  out.valueCount = 0;
  JsonVariantConst vals = doc["values"];
  if (vals.is<JsonArrayConst>()) {
    out.hasValues = true;
    for (JsonVariantConst v : vals.as<JsonArrayConst>()) {
      if (out.valueCount >= LAYOUT_MAX_BUTTONS) {
        Serial.printf("[api] ⚠️ values dépasse %u entrées : tableau rejeté\n",
                      LAYOUT_MAX_BUTTONS);
        out.valueCount = LAYOUT_MAX_BUTTONS + 1;
        break;
      }
      TileValue &tv = out.values[out.valueCount++];
      if (v.is<JsonObjectConst>()) {
        readTileValue(v["v"], v["t"], tv);
      } else {
        memset(&tv, 0, sizeof(tv));  // null : tuile qui n'est pas `view`
      }
    }
  }

  parseExtras(doc, out.extras);

  // Une commande n'est lue que si la capacité est ANNONCÉE (même règle que
  // pour l'attente longue) : une clé `cmd` venue d'ailleurs est ignorée.
  out.cmd = RemoteCmd();
  JsonVariantConst c = doc["cmd"];
  if (c.is<JsonObjectConst>()) {
    if (out.extras.features.cmd) {
      parseCmd(c, out.cmd);
    } else {
      Serial.println("[cmd] commande reçue sans features.cmd annoncé : ignorée");
    }
  }
  return JeedomResult::Ok;
}
