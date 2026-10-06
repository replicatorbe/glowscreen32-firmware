// Client HTTP du plugin Jeedom « glowscreen32 ».
// Implémente à la lettre les quatre actions du contrat d'API v2.2 :
//   action=layout  -> mise en page complète (pages, grille, bandeau)
//   action=press   -> déclenchement d'une commande
//   action=ping    -> liaison + version + états + info + heure, attente
//                     longue, commandes à distance, diagnostics (v2.2)
//   action=firmware-> mise à jour par le réseau (OTA)
//
// Les requêtes sont synchrones (HTTPClient) mais bornées en temps ET en
// taille. ⚠️ Depuis la v2.2 elles ne s'exécutent PLUS JAMAIS dans la tâche
// d'affichage : chaque tâche réseau (net/net_tasks) possède SA propre
// instance de JeedomClient, sans état partagé.
//
// NÉGOCIATION DE SCHÉMA (v2.0) : la carte annonce LAYOUT_SCHEMA en en-tête.
// Un plugin v1.4 ignore cet en-tête et répond en schéma 1 — ce qui reste
// parfaitement lisible ici. C'est ce qui permet de déployer les deux moitiés
// dans n'importe quel ordre sans immobiliser le parc.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "../model/layout.h"

// Timeout de référence du contrat, pour `layout` et `ping`.
static const uint16_t JEEDOM_TIMEOUT_MS = 5000;
// `layout` est rare et doit aboutir : on lui laisse les 3 tentatives pleines.
static const uint8_t JEEDOM_ATTEMPTS = 3;

// `press` est une action que l'utilisateur vient de demander : il veut savoir
// vite si elle a marché.
//
// UNE SEULE TENTATIVE (contrat v2.2 : 2,5 s x 1). Jusqu'en v2.1 l'appel
// figeait l'écran : en 2 x 3 s + 250 ms il restait inerte 6,3 s après un
// simple geste. Depuis la v2.2 il ne fige plus rien (tâche réseau), mais il
// retient les appuis suivants dans la file : s'acharner reste un mauvais
// calcul, d'autant que c'est le `ping` suivant qui fait autorité sur l'état.
static const uint16_t JEEDOM_PRESS_TIMEOUT_MS = 2500;
static const uint8_t JEEDOM_PRESS_ATTEMPTS = 1;

// Pause entre deux tentatives (courte : on ne veut pas figer l'UI).
static const uint16_t JEEDOM_RETRY_DELAY_MS = 250;

// Longueur du champ `info` du bandeau : 16 CARACTÈRES (contrat), soit jusqu'à
// 32 octets accentués + terminateur. Il en faisait 24 jusqu'en 2.3.0.
#define JEEDOM_INFO_CHARS LAYOUT_INFO_CHARS
#define JEEDOM_INFO_LEN   LAYOUT_INFO_LEN

// Sentinelle « aucun niveau à annoncer », pour le paramètre `rssi` optionnel du
// `ping` (contrat v2.1).
//
// ⚠️ Pourquoi une sentinelle et pas 0 : le contrat accepte l'intervalle
// -120..0, et 0 dBm en fait partie. S'en servir comme « pas de valeur »
// marcherait tant qu'on ne mesure jamais 0 — c'est-à-dire jusqu'au jour où on
// le mesure. +1 est hors bornes par construction, donc impossible à confondre.
static const int16_t JEEDOM_RSSI_NONE = 1;

// Capacités annoncées par le plugin (contrat v2.2, `features`). ⚠️ La carte
// n'utilise une capacité QUE si elle est annoncée : une carte qui enverrait
// `wait` à un plugin v2.1 recevrait une réponse immédiate, relancerait
// aussitôt, et martèlerait Jeedom en boucle serrée.
struct ServerFeatures {
  uint16_t wait = 0;   // s pendant lesquels le serveur accepte de retenir un ping
  bool     cmd = false;
};

// `rev` est opaque, au plus 16 caractères (contrat v2.2).
#define JEEDOM_REV_LEN 17

// Ce que `layout` et `ping` rapportent en plus depuis la v2.2.
struct ServerExtras {
  ServerFeatures features;
  bool hasFeatures = false;        // `features` présent (plugin >= v2.2)
  char rev[JEEDOM_REV_LEN] = {0};  // vide = absente
};

// Plafond technique de l'attente longue côté carte. HTTPClient ne connaît
// qu'un délai de lecture sur 16 bits (setTimeout(uint16_t), en ms) : `wait`
// + 5 s doit donc tenir sous 65,5 s. Le plugin annonce 25 s ; ce plafond ne
// joue que face à un serveur qui annoncerait davantage.
static const uint16_t JEEDOM_WAIT_MAX_S = 60;
// Marge ajoutée à `wait` pour le délai HTTP de l'attente longue (contrat).
static const uint16_t JEEDOM_WAIT_MARGIN_MS = 5000;

// --- Commandes à distance (contrat v2.2, `cmd`) ------------------------------

enum class CmdVerb : uint8_t {
  None,       // pas de commande dans la réponse
  Reboot,
  Identify,
  Message,
  Page,
  Calibrate,
  Ota,
  Wifi,
  Unknown,    // verbe hors vocabulaire : ignoré, mais journalisé
};

// 64 CARACTÈRES UTF-8 au plus : jusqu'à 4 octets chacun en théorie, 2 en
// pratique pour du français. La copie tronque sur le nombre de caractères ET
// sur la taille du tampon, toujours sur une frontière de caractère.
#define JEEDOM_CMD_TEXT_CHARS 64
#define JEEDOM_CMD_TEXT_LEN   160

struct RemoteCmd {
  CmdVerb  verb = CmdVerb::None;
  uint32_t seq = 0;
  uint16_t duration = 0;  // s, déjà borné (identify 1-120, message 1-600)
  uint8_t  page = 0;      // 0-3
  char     name[16] = {0};                  // verbe reçu, pour les traces
  char     text[JEEDOM_CMD_TEXT_LEN] = {0};  // message
  char     ssid[33] = {0};                  // wifi : <= 32 octets
  char     pass[65] = {0};                  // wifi : <= 64 octets
};

// Paramètres d'un `ping`.
struct PingParams {
  int16_t     rssi = 1;          // JEEDOM_RSSI_NONE (+1) : pas de mesure
  uint16_t    wait = 0;          // attente longue, s ; 0 = ping v2.1
  const char *rev = nullptr;     // dernière `rev` reçue : envoyée à CHAQUE ping
                                 // (condition de livraison de `cmd`), requise si wait > 0
};

// Résultat d'un appel. Les valeurs `Err*` du milieu correspondent exactement
// aux codes `error` du contrat.
enum class JeedomResult : uint8_t {
  Ok,
  ErrNoApikey,        // clé absente de secrets.h : on n'appelle même pas
  ErrBadApikey,       // 401 bad_apikey
  ErrUnknownDevice,   // 404 unknown_device
  ErrUnknownButton,   // 404 unknown_button
  ErrFirmwareMissing, // 404 firmware_unavailable
  ErrBadRequest,      // 400 bad_request
  ErrReadOnly,        // 403 read_only (contrat v3.0) : `press` sur un écran en lecture seule
  ErrServer,          // autre code HTTP / `ok:false` inconnu
  ErrTransport,       // pas de réponse (timeout, hôte injoignable)
  ErrParse,           // réponse illisible, JSON invalide, ou trop volumineuse
  ErrNoWifi,          // appel tenté sans liaison Wi-Fi
};

// Vrai si la clé API n'a manifestement pas été renseignée dans secrets.h.
bool jeedomApikeyConfigured(const char *apikey);

// Libellé court en français, pour le bandeau et les traces.
const char *jeedomResultLabel(JeedomResult r);

// Vrai pour les erreurs qui ne se corrigeront pas toutes seules : clé absente
// ou invalide, requête malformée. On les journalise et on espace franchement
// les tentatives au lieu d'enchaîner un backoff qui ne mènera nulle part.
bool jeedomResultIsPermanent(JeedomResult r);

// Réponse de `action=firmware` (contrat v1.4). Les deux verrous (`ota_enabled`
// global et `ota_allowed` par écran) sont entièrement côté serveur : la carte
// ne peut ni les forcer ni les contourner, elle reçoit juste `update:false`.
struct FirmwareInfo {
  bool update = false;
  char version[16] = {0};
  char url[192] = {0};
  char sha256[65] = {0};  // 64 caractères hexadécimaux + terminateur
  uint32_t size = 0;
};

// Réponse de `action=press` (contrat v1.3). `state` est l'état ATTENDU, pas
// constaté : sur matériel réel l'information remonte après un aller-retour.
// Il ne sert qu'à un retour visuel optimiste ; la vérité arrive avec le
// tableau `states` du `ping` suivant.
struct PressInfo {
  int8_t state = BTN_STATE_UNKNOWN;
  bool pending = true;  // true tant que l'équipement n'a pas confirmé
};

// Ce que `layout` et `ping` rapportent en plus du modèle persisté.
//
// ⚠️ Ces champs sont VOLATILS et n'ont RIEN à faire dans `Layout` : `info`
// (la température) et `time` changent à chaque ping. Les y mettre ferait
// réécrire la NVS toutes les 30 secondes par LayoutStore::save(), qui compare
// le contenu avant d'écrire.
struct BannerInfo {
  char     info[JEEDOM_INFO_LEN] = {0};  // chaîne déjà formatée par le plugin
  uint32_t time = 0;                     // epoch UTC
  int32_t  tzoffset = 0;                 // secondes, DST comprise
  bool     hasTime = false;
};

// Réponse de `action=ping`. Le tableau `states` est arrivé en v1.2 ; il peut
// manquer si le plugin n'est pas encore à jour, d'où `hasStates`.
struct PingInfo {
  int32_t  version = 0;
  bool     hasStates = false;                 // le champ `states` était présent
  uint8_t  stateCount = 0;                    // sa taille réelle
  int8_t   states[LAYOUT_MAX_BUTTONS] = {0};  // 0 / 1 / BTN_STATE_UNKNOWN
  BannerInfo banner;
  // `values` (schéma 3) : indexé par l'`id` GLOBAL, comme `states`, et de même
  // longueur. Une entrée sans sens (tuile non `view`) a `known` == 0.
  bool     hasValues = false;
  uint8_t  valueCount = 0;                    // LAYOUT_MAX_BUTTONS + 1 = tableau rejeté
  TileValue values[LAYOUT_MAX_BUTTONS] = {};
  ServerExtras extras;                        // v2.2 : features + rev
  RemoteCmd cmd;                              // v2.2 : verb None si absente
};

class JeedomClient {
 public:
  // `host` sans schéma (« 192.168.1.10 »), `device` = MAC normalisée.
  // Les trois chaînes sont COPIÉES : la configuration vit en NVS et peut être
  // réécrite par le portail pendant que les tâches réseau tournent.
  void begin(const char *host, const char *apikey, const char *device);

  // action=layout — remplit `out`, `banner` et `extras` uniquement en cas de
  // succès. Une seule tentative par défaut : c'est la tâche réseau qui
  // enchaîne les essais, pour pouvoir servir un `press` entre deux.
  // `values` reçoit `value`/`tone` des tuiles `view` (schéma 3), volatils.
  JeedomResult fetchLayout(Layout &out, TileValues &values, BannerInfo &banner,
                           ServerExtras &extras, uint8_t attempts = 1);

  // action=press — `id` est la valeur opaque reçue dans `layout` (le rang
  // global du bouton), renvoyée telle quelle : c'est le plugin qui résout la
  // commande. ⚠️ Ne JAMAIS appeler pour un bouton de mode `nav` : sa
  // navigation est locale et le plugin répondrait `unknown_button`.
  //
  // ⚠️ `schema` : celui dans lequel cet `id` a été REÇU (Layout::schema), et
  // annoncé tel quel dans l'en-tête. Le plugin résout le rang dans
  // l'aplatissement du schéma négocié, et les `id` d'un même bouton diffèrent
  // d'un schéma à l'autre (contrat v3.0 : le schéma 2 retire les tuiles
  // `view`). Un `id` reçu en schéma 2 et envoyé en annonçant 3 désignerait un
  // autre bouton.
  JeedomResult press(int32_t id, uint8_t schema, PressInfo &out,
                     uint8_t attempts = JEEDOM_PRESS_ATTEMPTS);

  // action=firmware — `fw` est la version compilée dans ce binaire.
  // Une seule tentative : l'appel est horaire, il est sa propre reprise.
  JeedomResult checkFirmware(const char *fw, FirmwareInfo &out, uint8_t attempts = 1);

  // action=ping — version, états, depuis la v2.0 info + heure, depuis la
  // v2.2 features + rev + cmd. Une seule tentative : le ping est lui-même
  // périodique, il est sa propre reprise.
  //
  // `p.rssi` est le niveau Wi-Fi à annoncer (contrat v2.1, OPTIONNEL) : hors
  // bornes ou laissé à JEEDOM_RSSI_NONE, il n'est pas envoyé. Les diagnostics
  // v2.2 (`up`, `rst`, `heap`, `blk`, `ip`, `ssid`) partent à chaque ping.
  // `p.wait` > 0 demande l'ATTENTE LONGUE : le délai HTTP passe alors à
  // `wait` + 5 s. À n'utiliser que si `features.wait` a été annoncé.
  JeedomResult ping(PingInfo &out, const PingParams &p);

  // Dernier résultat observé par CETTE instance (traces).
  JeedomResult lastResult() const { return _last; }

  // La dernière réponse était du JSON bien formé (quel qu'en soit le sens).
  bool lastJsonOk() const { return _lastJsonOk; }

  // Faux si `JEEDOM_APIKEY` est resté à sa valeur de gabarit.
  bool apikeyConfigured() const { return _apikeyOk; }

  // Schéma réellement servi par le plugin lors du dernier `layout` (1 ou 2).
  uint8_t serverSchema() const { return _serverSchema; }

 private:
  // Exécute `attempts` tentatives d'un GET et désérialise la réponse dans `doc`.
  // Renvoie Ok uniquement si HTTP 200 ET `ok:true` ; traduit sinon le champ
  // `error` du contrat en JeedomResult.
  JeedomResult request(const String &url, uint8_t attempts, JsonDocument &doc,
                       uint16_t timeoutMs = JEEDOM_TIMEOUT_MS,
                       uint8_t schema = LAYOUT_SCHEMA);

  // Construit « http://<host>/plugins/glowscreen32/core/php/api.php?action=..&device=.. »
  // La clé API voyage en en-tête, pas dans l'URL (contrat v1.1).
  String url(const char *action) const;

  // Les deux lectures de `buttons` : plate (schéma 1) ou paginée (schéma 2).
  void parseFlatButtons(JsonArrayConst arr, Layout &out, TileValues &values);
  void parsePages(JsonArrayConst arr, Layout &out, TileValues &values);

  char _host[64] = {0};
  char _apikey[100] = {0};
  char _device[LAYOUT_DEVICE_LEN] = {0};
  bool _apikeyOk = false;  // faux tant que la clé n'est pas renseignée
  uint8_t _serverSchema = 1;
  JeedomResult _last = JeedomResult::ErrNoWifi;
  bool _lastJsonOk = false;
};
