#include "portal.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "../fw_version.h"
#include "../net/jeedom_client.h"
#include "../store/device_config.h"

// Pile de la tâche portail : WebServer analyse la requête et la page est
// construite dans une String sur le tas — 6 ko laissent la marge, la valeur
// réelle est journalisée par netLogStacks().
static const uint32_t PORTAL_STACK = 6144;

// Liste des réseaux proposés : les plus forts d'abord, sans doublon.
static const uint8_t PORTAL_MAX_NETS = 12;
// Un balayage coûte ~2,5 s et coupe la radio du point d'accès pendant ce
// temps : on n'en refait un qu'à la demande, ou si le précédent a échoué.
static const uint32_t PORTAL_RESCAN_MS = 60000;

enum class PortalState : uint8_t { Off, Running, Stopping };

struct PortalNet {
  char   ssid[CFG_SSID_LEN];
  int8_t rssi;
  bool   open;
};

// Tout ce qui ne sert qu'au portail est alloué à l'ouverture et rendu à la
// fermeture : ~1 ko de tas plutôt qu'en BSS permanent, pour une fonction qui
// ne tourne presque jamais.
struct PortalCtx {
  WebServer server{80};
  DNSServer dns;
  PortalNet nets[PORTAL_MAX_NETS];
  uint8_t   netCount = 0;
  uint32_t  scannedAtMs = 0;
  bool      scanned = false;
  char      device[13] = {0};
};

static volatile PortalState s_state = PortalState::Off;
static volatile bool s_stopRequested = false;
static volatile bool s_taskDone = false;
// Soumission en attente et état affiché : échangés avec l'UI sous verrou.
static portMUX_TYPE s_subMux = portMUX_INITIALIZER_UNLOCKED;
static PortalSubmission s_sub;
static bool s_subReady = false;
static char s_status[96] = {0};
static TaskHandle_t s_task = nullptr;
static PortalCtx *s_ctx = nullptr;
static volatile uint32_t s_stackMargin = 0;

// --- Balayage -----------------------------------------------------------------

static void scanNetworks(PortalCtx &c) {
  // Échoue tant que la station est en pleine tentative d'association : on le
  // saura au prochain affichage de la page, qui retentera.
  const int16_t n = WiFi.scanNetworks(false, false, false, 300);
  c.scannedAtMs = millis();
  if (n < 0) {
    Serial.printf("[portail] balayage impossible (%d)\n", n);
    c.scanned = false;
    return;
  }
  c.netCount = 0;
  // Les résultats ne sont pas triés : sélection des plus forts, sans doublon
  // (un même SSID diffusé par la box et par un répéteur).
  for (uint8_t slot = 0; slot < PORTAL_MAX_NETS; slot++) {
    int16_t best = -1;
    for (int16_t i = 0; i < n; i++) {
      const String s = WiFi.SSID(i);
      if (s.length() == 0 || s.length() >= CFG_SSID_LEN) continue;
      bool seen = false;
      for (uint8_t k = 0; k < c.netCount && !seen; k++) seen = (s == c.nets[k].ssid);
      if (seen) continue;
      if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    }
    if (best < 0) break;
    PortalNet &net = c.nets[c.netCount++];
    strlcpy(net.ssid, WiFi.SSID(best).c_str(), sizeof(net.ssid));
    net.rssi = (int8_t)WiFi.RSSI(best);
    net.open = WiFi.encryptionType(best) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  c.scanned = true;
  Serial.printf("[portail] %u réseau(x) trouvé(s)\n", c.netCount);
}

// --- Page ---------------------------------------------------------------------

static void appendEscaped(String &out, const char *s) {
  for (; s && *s; s++) {
    switch (*s) {
      case '&':  out += "&amp;"; break;
      case '<':  out += "&lt;"; break;
      case '>':  out += "&gt;"; break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:   out += *s;
    }
  }
}

static const char PAGE_HEAD[] PROGMEM =
    "<!doctype html><html lang=fr><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>GlowScreen</title><style>"
    "body{font-family:sans-serif;max-width:26em;margin:1em auto;padding:0 1em;"
    "background:#0b0e14;color:#e8ecf2}"
    "label{display:block;margin-top:1em;font-weight:bold}"
    "input,select,button{width:100%;box-sizing:border-box;padding:.5em;margin-top:.3em;"
    "font-size:1em}button{margin-top:1.5em;background:#4c9aff;color:#fff;border:0}"
    "small{color:#7c8798}a{color:#4c9aff}</style></head><body>";

static void handleRoot() {
  PortalCtx &c = *s_ctx;
  if (c.server.hasArg("scan") || !c.scanned ||
      millis() - c.scannedAtMs > PORTAL_RESCAN_MS) {
    scanNetworks(c);
  }

  // La configuration courante sert à présélectionner le réseau et à
  // préremplir l'hôte. ⚠️ Ni le mot de passe ni la clé API ne sont jamais
  // renvoyés dans la page : quiconque est associé au point d'accès les lirait.
  DeviceConfig cfg;
  deviceConfig.copy(cfg);

  String page;
  page.reserve(3200);
  page += FPSTR(PAGE_HEAD);
  page += "<h2>GlowScreen</h2><p><small>Écran ";
  page += c.device;
  page += " · firmware " GLOWSCREEN_FW_VERSION "</small></p>";
  char status[sizeof(s_status)];
  portENTER_CRITICAL(&s_subMux);
  memcpy(status, s_status, sizeof(status));
  portEXIT_CRITICAL(&s_subMux);
  if (status[0]) {
    page += "<p><b>";
    page += status;
    page += "</b></p>";
  }
  page += "<form method=post action=/save>";
  page += "<label>Réseau Wi-Fi</label><select name=ssid>";
  bool currentListed = false;
  for (uint8_t i = 0; i < c.netCount; i++) {
    const PortalNet &n = c.nets[i];
    const bool cur = !strcmp(n.ssid, cfg.ssid);
    currentListed |= cur;
    page += "<option value=\"";
    appendEscaped(page, n.ssid);
    page += cur ? "\" selected>" : "\">";
    appendEscaped(page, n.ssid);
    page += " (";
    page += n.rssi;
    page += n.open ? " dBm, ouvert)" : " dBm)";
    page += "</option>";
  }
  if (!currentListed && cfg.ssid[0]) {
    page += "<option value=\"";
    appendEscaped(page, cfg.ssid);
    page += "\" selected>";
    appendEscaped(page, cfg.ssid);
    page += " (actuel, non trouvé)</option>";
  }
  page += "</select><input name=ssid2 maxlength=32 placeholder='ou saisir un autre nom'>"
          "<small><a href='/?scan=1'>Actualiser la liste</a></small>"
          "<label>Mot de passe Wi-Fi</label>"
          "<input type=password name=pass maxlength=64 autocomplete=off>"
          "<small>Vide : inchangé pour le même réseau, aucun pour un autre réseau.</small>"
          "<label>Hôte Jeedom</label><input name=host maxlength=63 value=\"";
  appendEscaped(page, cfg.host);
  page += "\"><label>Clé API du plugin</label>"
          "<input type=password name=apikey maxlength=99 autocomplete=off>"
          "<small>Vide : inchangée — obligatoire si l'hôte change.</small>"
          "<button>Essayer et enregistrer</button></form></body></html>";
  c.server.send(200, "text/html; charset=utf-8", page);
}

static bool validHost(const String &h) {
  if (h.length() == 0 || h.length() >= CFG_HOST_LEN) return false;
  for (size_t i = 0; i < h.length(); i++) {
    const char ch = h[i];
    if (!isalnum((unsigned char)ch) && ch != '.' && ch != '-' && ch != ':') return false;
  }
  return true;
}

static bool validApikey(const String &k) {
  if (k.length() < 8 || k.length() >= CFG_APIKEY_LEN) return false;
  for (size_t i = 0; i < k.length(); i++) {
    if (k[i] <= ' ' || k[i] > '~') return false;  // ASCII imprimable, sans espace
  }
  return true;
}

static void sendMessage(int code, const char *msg) {
  String page;
  page.reserve(900);
  page += FPSTR(PAGE_HEAD);
  page += "<h2>GlowScreen</h2><p>";
  page += msg;  // texte interne, jamais une saisie : pas d'échappement requis
  page += "</p><p><a href='/'>Retour</a></p></body></html>";
  s_ctx->server.send(code, "text/html; charset=utf-8", page);
}

static void handleSave() {
  WebServer &srv = s_ctx->server;
  DeviceConfig cfg;
  deviceConfig.copy(cfg);

  // ⚠️ Le SSID n'est PAS « nettoyé » : une espace en tête ou en fin en fait
  // partie (802.11), la retirer viserait un autre réseau. Toutes les longueurs
  // sont des OCTETS (String::length), ce que mesurent 802.11 et WPA2.
  String ssid = srv.arg("ssid2");
  if (ssid.length() == 0) ssid = srv.arg("ssid");
  const String pass = srv.arg("pass");
  String host = srv.arg("host");
  host.trim();
  String apikey = srv.arg("apikey");
  apikey.trim();

  if (ssid.length() == 0 || ssid.length() > 32) {
    sendMessage(400, "Nom de réseau absent ou trop long (32 octets au plus).");
    return;
  }
  // WPA2 : phrase de 8 à 63 octets, ou clé brute de 64 chiffres hexadécimaux.
  if (pass.length() > 0) {
    bool ok = pass.length() >= 8 && pass.length() <= 64;
    if (ok && pass.length() == 64) {
      for (size_t i = 0; i < 64 && ok; i++) ok = isxdigit((unsigned char)pass[i]);
    }
    if (!ok) {
      sendMessage(400, "Mot de passe Wi-Fi invalide : 8 à 63 octets, ou 64 chiffres "
                       "hexadécimaux.");
      return;
    }
  }
  if (!validHost(host)) {
    sendMessage(400, "Hôte Jeedom invalide : une adresse IP ou un nom, sans http://.");
    return;
  }
  if (apikey.length() > 0 && !validApikey(apikey)) {
    sendMessage(400, "Clé API invalide (8 à 99 caractères, sans espace).");
    return;
  }
  // ⚠️ Hôte CHANGÉ : la clé doit être ressaisie. Sinon, quiconque a rejoint
  // le point d'accès (il faut être devant l'écran, mais c'est tout) pourrait
  // saisir l'adresse de SON serveur et y recevoir la clé en service à la
  // première sonde. Et sans clé valide en service, rien à « garder ».
  if (apikey.length() == 0 && host != cfg.host) {
    sendMessage(400, "L'hôte change : ressaisir la clé API du plugin.");
    return;
  }
  if (apikey.length() == 0 && !jeedomApikeyConfigured(cfg.apikey)) {
    sendMessage(400, "Aucune clé API valide en service : la saisir.");
    return;
  }

  // Mot de passe vide : on garde l'actuel s'il s'agit du même réseau ; pour
  // un autre réseau, c'est un réseau ouvert. Le texte de la page le dit.
  const char *newPass = pass.length() ? pass.c_str() : (ssid == cfg.ssid ? cfg.pass : "");
  const char *newKey = apikey.length() ? apikey.c_str() : cfg.apikey;

  // ⚠️ RIEN n'est écrit ici. Des identifiants jamais éprouvés en NVS, c'est
  // une carte murale qui redémarre sur un réseau qu'elle ne rejoindra pas.
  // L'UI essaie d'abord (60 s au plus) et n'écrit qu'en cas de succès.
  portENTER_CRITICAL(&s_subMux);
  strlcpy(s_sub.ssid, ssid.c_str(), sizeof(s_sub.ssid));
  strlcpy(s_sub.pass, newPass, sizeof(s_sub.pass));
  strlcpy(s_sub.host, host.c_str(), sizeof(s_sub.host));
  strlcpy(s_sub.apikey, newKey, sizeof(s_sub.apikey));
  s_subReady = true;
  strlcpy(s_status, "Essai en cours…", sizeof(s_status));
  portEXIT_CRITICAL(&s_subMux);
  memset(&cfg, 0, sizeof(cfg));
  sendMessage(200, "Essai en cours (60 s au plus). Le point d'accès peut décrocher "
                   "pendant l'essai : reconnectez-vous puis rechargez la page. "
                   "Succès : l'écran enregistre et redémarre. Échec : rien n'est modifié.");
}

static void handleNotFound() {
  // Portail captif : toute URL ramène au formulaire. C'est ce qui fait
  // s'ouvrir la page toute seule sur un téléphone qui rejoint le réseau.
  WebServer &srv = s_ctx->server;
  srv.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
  srv.send(302, "text/plain", "");
}

// --- Tâche --------------------------------------------------------------------

static void portalTask(void *) {
  PortalCtx &c = *s_ctx;
  c.dns.start(53, "*", WiFi.softAPIP());
  c.server.on("/", HTTP_GET, handleRoot);
  c.server.on("/save", HTTP_POST, handleSave);
  c.server.onNotFound(handleNotFound);
  c.server.begin();
  scanNetworks(c);

  while (!s_stopRequested) {
    c.dns.processNextRequest();
    c.server.handleClient();
    s_stackMargin = uxTaskGetStackHighWaterMark(nullptr);
    vTaskDelay(pdMS_TO_TICKS(5));  // bloque : la tâche IDLE du cœur 0 respire
  }

  c.server.stop();
  c.dns.stop();
  s_task = nullptr;
  s_taskDone = true;
  vTaskDelete(nullptr);
}

// --- Interface ------------------------------------------------------------------

bool portalStart(const char *apName, const char *apPass, const char *device) {
  if (s_state != PortalState::Off) return false;

  s_ctx = new (std::nothrow) PortalCtx();
  if (!s_ctx) {
    Serial.println("[portail] mémoire insuffisante");
    return false;
  }
  strlcpy(s_ctx->device, device, sizeof(s_ctx->device));

  // Station + point d'accès : la station continue d'essayer son réseau.
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(apName, apPass)) {
    Serial.println("[portail] point d'accès refusé");
    WiFi.mode(WIFI_STA);
    delete s_ctx;
    s_ctx = nullptr;
    return false;
  }

  s_stopRequested = false;
  s_taskDone = false;
  portENTER_CRITICAL(&s_subMux);
  s_subReady = false;
  s_status[0] = '\0';
  portEXIT_CRITICAL(&s_subMux);
  // CŒUR 1, pas 0 : WebServer lit chaque requête par readStringUntil(), donc
  // par Stream::timedRead(), qui attend en BOUCLE ACTIVE jusqu'à 5 s qu'un
  // client lent (un téléphone qui vient de s'associer) envoie la suite. Sur le
  // cœur 0, cela affamerait IDLE0 et le chien de garde ferait redémarrer la
  // carte en pleine configuration (même défaut que BoundedStream, voir
  // jeedom_client.cpp). Le cœur 1 n'est pas surveillé : au pire, l'écran fige
  // quelques secondes pendant qu'on le configure.
  if (xTaskCreatePinnedToCore(portalTask, "portal", PORTAL_STACK, nullptr, 1, &s_task, 1) !=
      pdPASS) {
    Serial.println("[portail] tâche non créée");
    WiFi.softAPdisconnect(true);
    delete s_ctx;
    s_ctx = nullptr;
    return false;
  }
  s_state = PortalState::Running;
  // Le mot de passe n'est PAS journalisé : il n'existe qu'à l'écran.
  Serial.printf("[portail] ouvert : %s sur %s\n", apName, WiFi.softAPIP().toString().c_str());
  return true;
}

void portalStop() {
  if (s_state != PortalState::Running) return;
  s_stopRequested = true;
  s_state = PortalState::Stopping;
}

void portalService() {
  if (s_state != PortalState::Stopping || !s_taskDone) return;
  WiFi.softAPdisconnect(true);  // abaisse l'AP, la station reste
  delete s_ctx;
  s_ctx = nullptr;
  s_state = PortalState::Off;
  Serial.println("[portail] fermé");
}

bool portalActive() { return s_state != PortalState::Off; }

bool portalRunning() { return s_state == PortalState::Running; }

uint8_t portalClients() {
  return s_state == PortalState::Running ? WiFi.softAPgetStationNum() : 0;
}

bool portalTakeSubmission(PortalSubmission &out) {
  bool ready;
  portENTER_CRITICAL(&s_subMux);
  ready = s_subReady;
  if (ready) {
    memcpy(&out, &s_sub, sizeof(out));
    memset(&s_sub, 0, sizeof(s_sub));
    s_subReady = false;
  }
  portEXIT_CRITICAL(&s_subMux);
  return ready;
}

void portalSetStatus(const char *text) {
  portENTER_CRITICAL(&s_subMux);
  strlcpy(s_status, text ? text : "", sizeof(s_status));
  portEXIT_CRITICAL(&s_subMux);
}

uint32_t portalStackMargin() { return s_stackMargin; }
