// GlowScreen32 — firmware de la carte ESP32-2432S028 (Cheap Yellow Display).
//
// Affiche une grille tactile PAGINÉE décrite par le plugin Jeedom
// « glowscreen32 » (voir docs/api-contract.md, contrat v2.2) et notifie les
// appuis.
//
// Ce fichier est la TÂCHE D'AFFICHAGE (loopTask Arduino, cœur 1) : tactile,
// dessin, machine à états, application des résultats. ⚠️ Depuis la v2.2 elle
// n'émet AUCUN appel HTTP : elle dépose des demandes dans les files de
// net/net_tasks et applique ce qui en revient. Un réseau lent ne fige donc
// plus l'écran, et un appui fait pendant un appel n'est plus perdu.
//
// Machine à états (réseau) : BOOT -> WIFI -> LAYOUT -> RUN, plus ENROLL quand
// la carte n'est pas encore déclarée dans Jeedom. Par-dessus, un MODE
// d'affichage : accueil (grille ou enrôlement), réglages, portail Wi-Fi,
// calibration, identification, OTA, attente de redémarrage.
//
// Le binaire est IDENTIQUE sur toutes les cartes du parc : l'identité de
// l'écran est sa propre MAC (WifiMgr::deviceId), et sa configuration locale
// (Wi-Fi, Jeedom, calibration) vit en NVS (store/device_config).

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "cyd_pins.h"
#include "fw_version.h"
#include "model/layout.h"
#include "net/jeedom_client.h"
#include "net/net_tasks.h"
#include "net/portal.h"
#include "net/wifi_mgr.h"
#include "ota/ota_updater.h"
#include "store/device_config.h"
#include "store/layout_store.h"
#include "ui/button_grid.h"
#include "ui/theme.h"
#include "ui/touch_calib.h"

// --- Matériel --------------------------------------------------------------
TFT_eSPI tft = TFT_eSPI();
SPIClass touchSpi = SPIClass(VSPI);
XPT2046_Touchscreen touch(CYD_TOUCH_CS, CYD_TOUCH_IRQ);

// --- Modules ---------------------------------------------------------------
WifiMgr wifi;
LayoutStore store;
ButtonGrid grid;

// ⚠️ DOUBLE TAMPON. Une `LayoutBundle` (mise en page ~1,6 ko + valeurs des
// tuiles `view` ~1,3 ko) ne va jamais sur une pile. Il en existe exactement
// deux, et chacune a UN propriétaire à un instant donné :
//   - `curBundle` : celle affichée, à la tâche d'affichage ;
//   - l'autre : à la tâche réseau, qui y parse le prochain `layout`.
// Un `layout` réussi est CÉDÉ à l'UI, qui rend aussitôt l'ancien
// (netReturnLayout). Rien n'est copié, rien n'est partagé en écriture.
static LayoutBundle s_bundleA;
static LayoutBundle s_bundleB;
static LayoutBundle *curBundle = &s_bundleA;
static Layout *cur = &s_bundleA.layout;          // raccourcis vers curBundle
static TileValues *curValues = &s_bundleA.values;

// Tampon de réception des résultats réseau (~120 octets), statique plutôt que
// sur la pile de la tâche d'affichage. Le ping, lui, arrive par pointeur.
static NetResult s_net;

// Hôte Jeedom, pour l'écran Réglages (ni le mot de passe ni la clé ne sont
// gardés ici : ils ne s'affichent jamais).
static char s_host[CFG_HOST_LEN] = {0};

// --- Machine à états -------------------------------------------------------
enum class AppState : uint8_t {
  BOOT,    // initialisation matérielle + relecture du cache NVS
  WIFI,    // attente d'une liaison Wi-Fi
  LAYOUT,  // récupération de la mise en page auprès de Jeedom
  ENROLL,  // écran non déclaré dans Jeedom : on affiche la MAC et on attend
  RUN,     // fonctionnement nominal : tactile + ping
};

// Ce qui occupe l'écran. Indépendant de l'état réseau : on peut ouvrir les
// réglages pendant un enrôlement, ou recevoir un `layout` sous la calibration.
enum class UiMode : uint8_t {
  Home,      // grille (ou écran d'enrôlement en ENROLL), message éventuel par-dessus
  Settings,  // menu local
  Portal,    // portail Wi-Fi de secours
  Calib,     // calibration tactile
  Identify,  // commande `identify`
  Ota,       // mise à jour en cours, ou son échec affiché
  Notice,    // « Redémarrage… »
};

static AppState state = AppState::BOOT;
static UiMode mode = UiMode::Home;
static bool layoutKnown = false;      // une mise en page est affichable
static bool layoutInFlight = false;   // un `layout` est demandé à la tâche réseau
static uint32_t nextLayoutTryMs = 0;  // prochaine tentative `layout` après échec
static uint8_t layoutFailures = 0;    // pour le backoff
// La carte SAIT que sa mise en page est périmée : un ping a annoncé une autre
// `version`, ou des `states`/`values` incohérents. Jusqu'au `layout` suivant,
// les `id` affichés peuvent désigner d'autres boutons (contrat v3.0) : aucun
// `press` n'est émis (voir handleButton).
static bool layoutStale = false;
// Instant d'émission (millis) du dernier ping APPLIQUÉ : un `press` émis avant
// lui n'a plus à poser d'état « en attente » (voir onPressDone).
static uint32_t lastPingAppliedStartMs = 0;
static uint8_t pingsSansStates = 0;   // voir LAYOUT_RESYNC_PINGS
static uint8_t pressesInFlight = 0;   // `press` déposés, résultat pas encore reçu
static JeedomResult lastApiResult = JeedomResult::ErrNoWifi;
static bool apikeyOk = false;

// Repli pour un plugin resté en v1.1 (pas de champ `states` dans `ping`) :
// `version` ne bougeant qu'aux changements de CONFIGURATION, un état modifié
// ailleurs (appli Jeedom, interrupteur mural) passerait inaperçu. On force
// alors un `layout` complet tous les N pings. Avec un plugin v1.2 ou plus ce
// compteur ne sert jamais : `states` suffit, sans redessin de la grille.
static const uint8_t LAYOUT_RESYNC_PINGS = 10;

// Backoff sur `layout` : 2 s, doublé par échec, plafonné à 60 s.
static const uint32_t LAYOUT_RETRY_BASE_MS = 2000;
static const uint32_t LAYOUT_RETRY_MAX_MS = 60000;

// Période de scrutation en attente d'enrôlement (contrat : 10 s).
static const uint32_t ENROLL_RETRY_MS = 10000;

// Après un `press` marqué `pending`, SANS attente longue, on rapproche le ping
// suivant : c'est lui qui confirmera ou infirmera l'état optimiste affiché.
// Avec l'attente longue, inutile : le ping retenu revient de lui-même dès que
// l'état change (contrat v2.2).
static const uint32_t PRESS_CONFIRM_DELAY_MS = 2000;

// --- Configuration locale (contrat v2.2) -----------------------------------

// Appui maintenu sur le bandeau qui ouvre le menu Réglages.
static const uint32_t MENU_HOLD_MS = 5000;
// Le menu se referme seul : un écran mural ne doit pas rester sur ses
// réglages parce que quelqu'un l'a ouvert puis oublié.
static const uint32_t SETTINGS_IDLE_MS = 60000;
static const uint32_t SCREEN_REFRESH_MS = 2000;
// Sans Wi-Fi depuis ce délai, le portail de secours s'ouvre (contrat).
static const uint32_t PORTAL_AUTO_MS = 5UL * 60UL * 1000UL;
// Calibration : abandon sans toucher pendant ce délai (contrat).
static const uint32_t CALIB_IDLE_MS = 60000;
// Commande `wifi` : délai pour s'associer avant retour aux anciens
// identifiants (contrat).
static const uint32_t WIFI_TRIAL_MS = 60000;

// --- Mise à jour par le réseau (OTA, contrat v1.4) -------------------------

static const uint32_t OTA_CHECK_PERIOD_MS = 3600000UL;            // 1 h
static const uint32_t OTA_FIRST_CHECK_MS = 60000UL;               // 1 min
static const uint32_t OTA_RETRY_AFTER_FAIL_MS = 6UL * 3600000UL;  // 6 h
static const uint32_t OTA_DEFER_MS = 60000UL;  // mise à jour annoncée mais écran occupé
static uint32_t nextOtaCheckMs = 0;
static bool fwCheckInFlight = false;
static bool otaRunning = false;
static bool otaFailed = false;
static uint32_t otaFailUntilMs = 0;

// --- Filet de sécurité du firmware -----------------------------------------
//
// Un firmware installé par OTA démarre en état « à confirmer ». Tant qu'il ne
// s'est pas déclaré sain, le bootloader rebascule sur la partition précédente
// au prochain redémarrage. On ne le déclare sain QU'APRÈS avoir vérifié que
// l'écran, le Wi-Fi et l'API répondent — jamais depuis setup().
static bool fwPendingVerify = false;
static bool screenReady = false;
static bool apiAnswered = false;
static uint32_t verifyDeadlineMs = 0;

// ⚠️ Preuves EXIGÉES depuis la revue de sûreté de la 2.2.0. Un seul `layout`
// réussi ne prouvait que la tâche réseau : la tâche ping, l'attente longue et
// la lecture des commandes n'avaient pas encore tourné. Un défaut dans l'une
// d'elles, découvert APRÈS validation, donnait une carte murale qui redémarre
// en boucle sans plus aucun retour arrière possible — et dont le contrôle OTA
// (prévu 60 s après validation) n'était jamais atteint. On exige donc que
// chaque chemin ait tourné au moins une fois :
static bool pingOkSeen = false;        // un ping réussi rendu par la TÂCHE ping
static bool longPollPingSeen = false;  // un ping en attente longue terminé normalement
static bool fwCheckOkSeen = false;     // une réponse OK à `action=firmware` (contrôle seul)
// ... et un minimum de fonctionnement continu : une fuite ou un plantage
// différé de quelques dizaines de secondes doit avoir le temps de se montrer.
static const uint32_t OTA_VERIFY_MIN_UPTIME_MS = 120000UL;  // 2 min

// Délai laissé au firmware pour faire ses preuves. Généreux à dessein : sur la
// liaison faible constatée sur site (-88 dBm), l'association Wi-Fi puis le
// premier `layout` peuvent demander plusieurs tentatives, et les preuves
// ci-dessus demandent au moins un contrôle firmware (60 s après le démarrage)
// et un ping retenu (jusqu'à 30 s). Trop court, on reviendrait en arrière sur
// un firmware parfaitement sain.
static const uint32_t OTA_VERIFY_TIMEOUT_MS = 600000UL;  // 10 min
// Contrôle firmware en échec AVANT validation : on réessaie vite, c'est une
// preuve exigée — l'attendre une heure ferait revenir en arrière.
static const uint32_t OTA_VERIFY_CHECK_RETRY_MS = 30000UL;

// Écran non déclaré (ENROLL) : ni ping ni contrôle firmware n'aboutissent pour
// une MAC inconnue. Les preuves 4 et 5 sont alors REMPLACÉES par deux réponses
// `unknown_device` espacées d'au moins une minute (contrat v3.0). Sans cette
// exception, un écran pas encore déclaré reviendrait en arrière après chaque
// OTA, et une version saine serait mémorisée comme rejetée.
static bool enrollFirstSeen = false;
static uint32_t enrollFirstMs = 0;
static bool enrollProof = false;
static const uint32_t ENROLL_PROOF_SPACING_MS = 60000UL;

// Une réponse de l'API compte comme preuve de bon fonctionnement si elle a
// traversé TOUTE la chaîne : réseau, authentification, parsing JSON.
// `unknown_device` en fait partie — le plugin a validé la clé avant de chercher
// l'équipement. Sans cela, un écran neuf pas encore déclaré dans Jeedom
// reviendrait en arrière indéfiniment, ce qui serait absurde. `bad_apikey`,
// lui, ne compte PAS : un firmware qui casserait l'authentification doit être
// rejeté. Depuis la v2.2 les réponses arrivent des tâches réseau, mais c'est
// toujours ICI, dans la tâche d'affichage, qu'elles sont comptées.
static void noteApiAnswer(JeedomResult r) {
  if (r == JeedomResult::Ok || r == JeedomResult::ErrUnknownDevice) apiAnswered = true;
  if (r == JeedomResult::ErrUnknownDevice && fwPendingVerify) {
    if (!enrollFirstSeen) {
      enrollFirstSeen = true;
      enrollFirstMs = millis();
    } else if (millis() - enrollFirstMs >= ENROLL_PROOF_SPACING_MS) {
      enrollProof = true;
    }
  }
}

// --- Échéances ---------------------------------------------------------------
//
// Échéance atteinte ? `(int32_t)(millis() - deadline) >= 0` est juste au
// passage de millis() à zéro, mais FAUX pour une échéance oubliée depuis plus
// de 24,8 jours : la différence change de signe et l'échéance redevient
// « future » pour 24,8 jours de plus. C'était le cas de `nextLayoutTryMs`,
// jamais rafraîchie après un `layout` réussi : au bout de 24,8 jours sans
// échec, une nouvelle `version` laissait l'écran bloqué en LAYOUT — plus de
// ping, plus d'OTA, et des appuis envoyés avec des `id` périmés.
//
// `maxAheadMs` est le plus long délai qu'on pose jamais sur cette échéance :
// une échéance plus lointaine que cela ne peut être qu'une échéance PÉRIMÉE,
// donc atteinte.
static bool due(uint32_t deadline, uint32_t maxAheadMs) {
  const uint32_t now = millis();
  return (int32_t)(now - deadline) >= 0 || (deadline - now) > maxAheadMs;
}

// --- Écrans et superpositions ----------------------------------------------
static bool messageShown = false;
static uint32_t messageUntilMs = 0;
static uint32_t identifyUntilMs = 0, identifyNextMs = 0;
static uint8_t identifyPhase = 0;
static uint32_t settingsUntilMs = 0, screenRefreshMs = 0;
static bool rebootConfirm = false;
static uint32_t rebootConfirmUntilMs = 0;
static bool rebootPending = false;
static uint32_t rebootAtMs = 0;
static SettingsInfo s_settings;

// Portail : ouvert automatiquement (se referme au retour du Wi-Fi) ou depuis
// le menu (reste ouvert jusqu'à ce qu'on le ferme).
static bool portalAuto = false;
// Portail ouvert DEPUIS LE MENU : refermé seul après 20 min sans aucun
// appareil associé. Oublié ouvert, il resterait un point d'accès permanent.
static uint32_t portalLastClientMs = 0;
static const uint32_t PORTAL_MENU_IDLE_MS = 20UL * 60UL * 1000UL;
static char portalName[24] = {0};
static char portalPass[12] = {0};
static bool wifiDown = true;
static uint32_t wifiDownSinceMs = 0;

// Essai d'identifiants (commande `wifi` ou portail). Rien n'est écrit en NVS
// avant que les DEUX preuves soient réunies : association nouvelle sur le SSID
// visé (WifiMgr::trialAssociated) PUIS réponse de l'API, avec l'hôte et la clé
// essayés, à une requête émise après cette association (netRequestProbe).
struct CredTrial {
  bool     active = false;
  bool     fromPortal = false;
  bool     probeInFlight = false;
  uint32_t nextProbeMs = 0;  // posé à millis() au lancement (jamais laissé à 0)
  uint32_t gen = 0;          // génération : un résultat d'un essai précédent est ignoré
  bool     associatedEver = false;   // pour le message d'échec
  JeedomResult lastProbe = JeedomResult::ErrNoWifi;
  char     host[CFG_HOST_LEN] = {0};
  char     apikey[CFG_APIKEY_LEN] = {0};
};
static CredTrial s_trial;
static uint32_t s_trialGen = 0;
// Issue du dernier essai lancé depuis le portail, affichée sur son écran.
static char s_portalTrialStatus[64] = {0};
static const uint32_t TRIAL_PROBE_RETRY_MS = 3000;

// Commandes à distance : dédoublonnage par `seq` (contrat v2.2).
static bool haveCmdSeq = false;
static uint32_t lastCmdSeq = 0;

static uint32_t nextStackLogMs = 0;
static const uint32_t STACK_LOG_FIRST_MS = 15000;
static const uint32_t STACK_LOG_PERIOD_MS = 10UL * 60UL * 1000UL;

// --- Rétroéclairage --------------------------------------------------------
//
// IO21 est piloté par logiciel. Un panneau mural à pleine luminosité toute la
// nuit est une nuisance : on l'atténue après une minute d'inactivité.
// ⚠️ L'attachement LEDC doit venir APRÈS tft.init() : TFT_eSPI, configuré avec
// -DTFT_BL=21, fait lui-même un digitalWrite sur cette broche à l'init.
static uint32_t lastActivityMs = 0;
static bool backlightDim = false;

static void backlightInit() {
  ledcSetup(UI_BL_CHANNEL, UI_BL_FREQ_HZ, UI_BL_RES_BITS);
  ledcAttachPin(CYD_TFT_BL, UI_BL_CHANNEL);
  ledcWrite(UI_BL_CHANNEL, UI_BL_FULL);
  lastActivityMs = millis();
}

static bool backlightAsleep() { return backlightDim; }

static void backlightWake() {
  lastActivityMs = millis();
  if (!backlightDim) return;

  // ⚠️ Contrat v2.1 : un `ping` IMMÉDIAT au réveil, avant même le premier
  // appui. C'est la contrepartie indispensable de la cadence espacée en veille
  // (voir pingConfigure) : sans elle, quelqu'un qui s'approche verrait les
  // pastilles du dernier rafraîchissement nocturne, c'est-à-dire de
  // l'information périmée au seul instant où quelqu'un la regarde.
  //
  // Depuis la v2.2 ce n'est plus qu'une DEMANDE à la tâche ping : jusqu'en
  // v2.1 ce ping partait dans le même tour de boucle que le toucher qui
  // réveillait, et l'écran restait figé pendant toute sa durée. Placé APRÈS le
  // retour anticipé : seul un vrai réveil compte.
  pingPoke(0);

  for (int v = UI_BL_DIM; v < UI_BL_FULL; v += 16) {
    ledcWrite(UI_BL_CHANNEL, v);
    delay(3);
  }
  ledcWrite(UI_BL_CHANNEL, UI_BL_FULL);
  backlightDim = false;
}

static void backlightLoop() {
  if (backlightDim) return;
  // Durée écoulée en non signé : juste quel que soit l'âge de l'activité.
  if (millis() - lastActivityMs < UI_BL_IDLE_MS) return;
  for (int v = UI_BL_FULL; v > UI_BL_DIM; v -= 8) {
    ledcWrite(UI_BL_CHANNEL, v);
    delay(5);
  }
  ledcWrite(UI_BL_CHANNEL, UI_BL_DIM);
  backlightDim = true;
}

// LED RVB, logique inversée (LOW = allumé). Éteinte hors `identify`.
static void setLed(bool r, bool g, bool b) {
  digitalWrite(CYD_LED_R, r ? LOW : HIGH);
  digitalWrite(CYD_LED_G, g ? LOW : HIGH);
  digitalWrite(CYD_LED_B, b ? LOW : HIGH);
}

// --- Suivi du geste tactile ------------------------------------------------
//
// Contrairement à la v1.4, qui déclenchait dès le FRONT DESCENDANT, l'action
// part au RELÂCHEMENT. Le retour visuel, lui, reste immédiat — la règle du
// contrat est respectée. Ce découplage apporte deux choses : un doigt qui
// glisse hors de la tuile annule l'appui au lieu de l'exécuter, et le
// balayage devient possible (il faut connaître le point d'arrivée).
static bool touchDown = false;
static int16_t touchStartX = 0, touchStartY = 0;
static uint32_t touchStartMs = 0;
static uint32_t lastReleaseMs = 0;
static int8_t touchButton = -1;    // bouton sous le point de départ
static bool touchTargetResolved = false;
static bool touchCancelled = false;
static bool touchWokeScreen = false;
static bool touchInBanner = false;  // candidat à l'appui maintenu du menu
// Geste ANNULÉ parce que l'écran a changé sous le doigt (message, page ou
// mise en page reçus à distance, écran plein ouvert). Contrairement à
// `touchCancelled` (doigt sorti de la tuile, balayage encore possible), rien
// ne part au relâchement : ni appui, ni balayage — la tuile visée n'est peut-
// être plus la même.
static bool touchAborted = false;

static void scheduleLayoutSoon();

static void cancelGesture() {
  if (touchDown) touchAborted = true;
  touchButton = -1;
  if (grid.pressed() >= 0) grid.setPressed(-1);
}

// Fenêtre glissante pour la médiane, et dernière position jugée FIABLE.
static int16_t tsX[TOUCH_MEDIAN_N], tsY[TOUCH_MEDIAN_N];
static uint8_t tsCount = 0, tsHead = 0;
static int16_t touchGoodX = 0, touchGoodY = 0;
static bool touchHaveGood = false;
static uint8_t touchOffTarget = 0;
static uint32_t touchLostMs = 0;  // instant de perte du contact, 0 = présent

static void touchPush(int16_t x, int16_t y) {
  tsX[tsHead] = x;
  tsY[tsHead] = y;
  tsHead = (uint8_t)((tsHead + 1) % TOUCH_MEDIAN_N);
  if (tsCount < TOUCH_MEDIAN_N) tsCount++;
}

static const char *stateName(AppState s) {
  switch (s) {
    case AppState::BOOT:   return "BOOT";
    case AppState::WIFI:   return "WIFI";
    case AppState::LAYOUT: return "LAYOUT";
    case AppState::ENROLL: return "ENROLL";
    case AppState::RUN:    return "RUN";
  }
  return "?";
}

static void goTo(AppState s) {
  if (s == state) return;
  Serial.printf("[etat] %s -> %s\n", stateName(state), stateName(s));
  state = s;
}

// Met le bandeau à jour à partir de l'état courant des deux liaisons.
//
// ⚠️ Contrat v2.0 : quand tout va bien, on ne dit RIEN. La place sert alors à
// `info` et à l'heure. Un voyant « API OK » affiché en permanence n'informe
// personne et occupe la seule zone utile de l'écran.
//
// ⚠️ Ne consomme PAS l'événement Wi-Fi (jusqu'en v2.1 elle appelait
// wifi.consumeChange(), qui pouvait avaler une transition que loop() attendait).
static void refreshBanner(JeedomResult apiResult) {
  grid.setWifi(wifi.connected(), wifi.connected() ? wifi.rssi() : 0);
  if (!apikeyOk) {
    // Cause la plus probable au premier démarrage : clé non renseignée.
    grid.setApi(UiLink::Error, jeedomResultLabel(JeedomResult::ErrNoApikey));
  } else if (!wifi.connected()) {
    grid.setApi(UiLink::Offline, portalActive() ? "Portail Wi-Fi" : "Hors ligne");
  } else if (apiResult == JeedomResult::Ok) {
    grid.setApi(UiLink::Ok, "");
  } else {
    grid.setApi(UiLink::Error, jeedomResultLabel(apiResult));
  }
  // Seule la moitié droite est repeinte : repeindre tout le bandeau ferait
  // clignoter le titre à chaque ping. Sans effet sous un écran plein.
  grid.drawStatus();
}

// Recale l'horloge et la valeur du bandeau depuis ce que le plugin a renvoyé.
static void applyBanner(const BannerInfo &b) {
  if (b.hasTime) grid.setClock(b.time, b.tzoffset);
  grid.setInfo(b.info);
}

// --- Écrans : transitions ----------------------------------------------------

static void simpleTouchDisarm();

static const char *currentSsid() {
  static char ssid[CFG_SSID_LEN];
  strlcpy(ssid, wifi.connected() ? WiFi.SSID().c_str() : wifi.targetSsid(), sizeof(ssid));
  return ssid;
}

// Retour à l'écran d'accueil : la grille, ou l'écran d'enrôlement.
static void showHome() {
  mode = UiMode::Home;
  messageShown = false;
  touchDown = false;
  simpleTouchDisarm();
  grid.setPressed(-1);  // sous un écran plein : ne repeint rien, efface l'appui
  if (state == AppState::ENROLL) {
    grid.drawEnroll(wifi.macPretty(), wifi.deviceId(), wifi.connected() ? currentSsid() : nullptr);
  } else {
    grid.uncover();
  }
}

static void closeMessage() {
  if (!messageShown) return;
  messageShown = false;
  if (mode == UiMode::Home && state != AppState::ENROLL) grid.uncover();
}

// Écrans qui ne doivent pas être interrompus par une commande à distance :
// une calibration à moitié faite, un OTA, un redémarrage annoncé.
static bool uiBusy() {
  return mode == UiMode::Calib || mode == UiMode::Ota || mode == UiMode::Notice;
}

static void scheduleReboot(const char *reason, uint32_t delayMs) {
  if (fwPendingVerify) {
    // Un redémarrage AVANT validation fait revenir à la version précédente :
    // c'est le filet de sécurité, il n'a pas d'exception.
    Serial.println("[boot] ⚠️ firmware pas encore validé : ce redémarrage ramènera la "
                   "version précédente");
  }
  Serial.printf("[boot] redémarrage dans %lu ms : %s\n", (unsigned long)delayMs, reason);
  cancelGesture();
  mode = UiMode::Notice;
  messageShown = false;
  grid.drawNotice("Redémarrage", reason);
  rebootPending = true;
  rebootAtMs = millis() + delayMs;
}

static void fillSettings(SettingsInfo &s) {
  s.mac = wifi.macPretty();
  strlcpy(s.ip, WiFi.localIP().toString().c_str(), sizeof(s.ip));
  strlcpy(s.ssid, currentSsid(), sizeof(s.ssid));
  s.wifiUp = wifi.connected();
  s.rssi = s.wifiUp ? wifi.rssi() : 0;
  s.fw = GLOWSCREEN_FW_VERSION;
  s.partition = otaRunningPartition();
  s.uptimeS = (uint32_t)(esp_timer_get_time() / 1000000LL);
  s.heap = ESP.getFreeHeap();
  s.block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  s.host = s_host;
  s.api = !apikeyOk ? "sans clé"
                    : (lastApiResult == JeedomResult::Ok ? "API OK" : jeedomResultLabel(lastApiResult));
  s.portalOpen = portalRunning();
  s.confirmReboot = rebootConfirm;
}

static void openSettings() {
  if (uiBusy()) return;
  Serial.println("[ui] menu Réglages");
  messageShown = false;
  mode = UiMode::Settings;
  rebootConfirm = false;
  settingsUntilMs = millis() + SETTINGS_IDLE_MS;
  screenRefreshMs = millis() + SCREEN_REFRESH_MS;
  fillSettings(s_settings);
  grid.drawSettings(s_settings);
  simpleTouchDisarm();
}

static void fillPortal(PortalInfo &p) {
  static char ip[16];
  strlcpy(ip, WiFi.softAPIP().toString().c_str(), sizeof(ip));
  p.apName = portalName;
  p.apPass = portalPass;
  p.apIp = ip;
  p.targetSsid = wifi.targetSsid();
  p.staUp = wifi.connected();
  p.clients = portalClients();
  p.trial = s_portalTrialStatus;  // déclaré plus haut, avant les essais
}

static void showPortalScreen() {
  cancelGesture();
  mode = UiMode::Portal;
  messageShown = false;
  PortalInfo p;
  fillPortal(p);
  grid.drawPortal(p);
  screenRefreshMs = millis() + SCREEN_REFRESH_MS;
  simpleTouchDisarm();
}

static void openPortal(bool automatic) {
  if (portalRunning()) {
    showPortalScreen();
    return;
  }
  // En cours de fermeture : la tâche peut encore servir une requête. On ne
  // rouvre qu'une fois tout abaissé (portalService), à la demande suivante.
  if (portalActive()) return;
  s_portalTrialStatus[0] = '\0';
  portalLastClientMs = millis();
  // Mot de passe tiré au hasard à CHAQUE ouverture, alphabet sans caractères
  // ambigus (0/O, 1/l/I) : il se recopie depuis l'écran, à deux mètres.
  // esp_random() est un vrai aléa tant que la radio tourne.
  static const char PW_CHARS[] = "abcdefghjkmnpqrstuvwxyz23456789";
  for (uint8_t i = 0; i < 8; i++) portalPass[i] = PW_CHARS[esp_random() % (sizeof(PW_CHARS) - 1)];
  portalPass[8] = '\0';
  snprintf(portalName, sizeof(portalName), "GlowScreen-%s", wifi.macSuffix());
  if (!portalStart(portalName, portalPass, wifi.deviceId())) return;
  portalAuto = automatic;
  Serial.printf("[portail] ouverture %s\n", automatic ? "automatique (Wi-Fi absent)" : "depuis le menu");
  showPortalScreen();
  refreshBanner(lastApiResult);
}

static void closePortal() {
  portalStop();
  s_portalTrialStatus[0] = '\0';
  portalAuto = false;
  // Le compte à rebours de 5 min repart : s'il manque toujours, le Wi-Fi
  // rouvrira le portail, mais pas dans la seconde qui suit sa fermeture.
  wifiDownSinceMs = millis();
}

// --- Essai d'identifiants ---------------------------------------------------------

static void showMessage(const char *text, uint16_t durationS);

static bool startCredTrial(const char *ssid, const char *pass, const char *host,
                           const char *apikey, bool fromPortal) {
  if (otaRunning || wifi.trialRunning() || s_trial.active) {
    Serial.println("[wifi] essai refusé : OTA ou autre essai en cours");
    if (fromPortal) portalSetStatus("Refusé : un autre essai ou une mise à jour est en cours.");
    return false;
  }
  s_trial = CredTrial();
  s_trial.active = true;
  s_trial.fromPortal = fromPortal;
  s_trial.gen = ++s_trialGen;
  // ⚠️ Jamais 0 : comparée par due(), une échéance laissée à 0 serait vue
  // « future » pendant 24,8 jours une fois millis() passé 2^31 — la sonde
  // ne partirait jamais et l'essai échouerait à tort.
  s_trial.nextProbeMs = millis();
  strlcpy(s_trial.host, host, sizeof(s_trial.host));
  strlcpy(s_trial.apikey, apikey, sizeof(s_trial.apikey));
  if (fromPortal) strlcpy(s_portalTrialStatus, "Essai en cours…", sizeof(s_portalTrialStatus));
  wifi.tryCredentials(ssid, pass, WIFI_TRIAL_MS);
  return true;
}

// Cause précise d'un essai raté, pour l'écran du portail et sa page web.
static const char *trialFailureText() {
  if (!s_trial.associatedEver) return "Échec : Wi-Fi non associé (réseau ou mot de passe)";
  switch (s_trial.lastProbe) {
    case JeedomResult::ErrBadApikey: return "Échec : clé API refusée par Jeedom";
    case JeedomResult::ErrNoApikey:  return "Échec : clé API absente ou invalide";
    case JeedomResult::ErrBadRequest:
    case JeedomResult::ErrServer:    return "Échec : Jeedom répond en erreur";
    default:                         return "Échec : Jeedom injoignable à cet hôte";
  }
}

static void stepCredTrial() {
  if (!s_trial.active) return;
  if (wifi.trialAssociated()) s_trial.associatedEver = true;
  // Association nouvelle obtenue : la preuve suivante est une réponse de
  // l'API. Émise MAINTENANT, elle part forcément après l'association.
  if (wifi.trialAssociated() && !s_trial.probeInFlight &&
      due(s_trial.nextProbeMs, TRIAL_PROBE_RETRY_MS)) {
    if (netRequestProbe(s_trial.host, s_trial.apikey, wifi.deviceId(), s_trial.gen)) {
      s_trial.probeInFlight = true;
    } else {
      s_trial.nextProbeMs = millis() + TRIAL_PROBE_RETRY_MS;
    }
  }

  switch (wifi.consumeTrialOutcome()) {
    case WifiTrial::Succeeded: {
      // Jeedom d'abord, Wi-Fi ensuite : chacun est un blob NVS atomique. Si le
      // premier échoue, rien n'a changé ; si seul le second échoue, le message
      // le dit exactement.
      bool jeedomOk = true;
      if (s_trial.fromPortal) jeedomOk = deviceConfig.saveJeedom(s_trial.host, s_trial.apikey);
      const bool wifiOk = jeedomOk && deviceConfig.saveWifi(wifi.targetSsid(), wifi.trialPass());
      const bool unknownScreen = s_trial.lastProbe == JeedomResult::ErrUnknownDevice;
      Serial.printf("[wifi] essai réussi, enregistrement Jeedom %s, Wi-Fi %s\n",
                    jeedomOk ? "fait" : "EN ÉCHEC", wifiOk ? "fait" : "EN ÉCHEC");
      if (s_trial.fromPortal) {
        const char *msg;
        if (!jeedomOk) msg = "Réussi, mais mémoire refusée : rien n'est modifié";
        else if (!wifiOk) msg = "Hôte et clé enregistrés, Wi-Fi NON enregistré";
        else if (unknownScreen) msg = "Enregistré ; écran pas encore déclaré dans Jeedom";
        else msg = "Réussi et enregistré : redémarrage";
        strlcpy(s_portalTrialStatus, msg, sizeof(s_portalTrialStatus));
        portalSetStatus(msg);
        // Hôte et clé ne sont pris en compte par les tâches qu'au démarrage.
        if (jeedomOk) scheduleReboot(msg, 2500);
      }
      memset(&s_trial, 0, sizeof(s_trial));
      break;
    }
    case WifiTrial::Reverted: {
      const char *why = trialFailureText();
      Serial.printf("[wifi] essai raté en 60 s (%s) : anciens réglages rétablis, rien écrit\n",
                    why);
      if (s_trial.fromPortal) {
        strlcpy(s_portalTrialStatus, why, sizeof(s_portalTrialStatus));
        portalSetStatus(why);
        // L'échec reste SUR L'ÉCRAN DU PORTAIL : le mot de passe du point
        // d'accès doit rester lisible pour corriger la saisie.
        if (portalRunning() && !uiBusy()) showPortalScreen();
      }
      memset(&s_trial, 0, sizeof(s_trial));
      break;
    }
    default:
      break;
  }
}

static void onProbeDone(const NetResult &res) {
  // Résultat d'une sonde d'un essai PRÉCÉDENT (abandonné, remplacé) : il ne
  // dit rien des identifiants en cours d'essai.
  if (!s_trial.active || res.gen != s_trial.gen) return;
  s_trial.probeInFlight = false;
  s_trial.lastProbe = res.r;
  // `unknown_device` prouve autant qu'`ok` que l'hôte et la clé sont bons
  // (même règle que pour la validation d'un firmware) ; `bad_apikey` non.
  if (res.r == JeedomResult::Ok || res.r == JeedomResult::ErrUnknownDevice) {
    wifi.confirmTrial();
  } else {
    Serial.printf("[wifi] essai : l'API ne répond pas (%s), nouvel essai\n",
                  jeedomResultLabel(res.r));
    s_trial.nextProbeMs = millis() + TRIAL_PROBE_RETRY_MS;
  }
}

// --- Calibration tactile à l'écran ------------------------------------------
//
// Reprend la logique de l'outil `calib` (src/tools/touch_calib.cpp) : quatre
// cibles, moyenne des lectures brutes sous chaque doigt, extrapolation aux
// bords (touchCalFromSamples, partagée). Le résultat va en NVS ; touchRawToScreen
// l'utilise aussitôt.
static uint8_t calStep = 0;  // 0..3 : coins ; TOUCH_CAL_TARGETS : contrôle au centre
static int32_t calX[TOUCH_CAL_TARGETS], calY[TOUCH_CAL_TARGETS];
static int32_t calSumX = 0, calSumY = 0;
static uint16_t calN = 0;
static bool calDown = false, calArmed = false;
static uint32_t calStartMs = 0, calLostMs = 0, calFreeSinceMs = 0, calLastTouchMs = 0;
static uint32_t calOutcomeUntilMs = 0;
static bool calOutcomeShown = false;
static TouchCal calCandidate;  // bornes calculées, PAS encore écrites
// Sans toucher la cible de contrôle dans ce délai : abandon, rien n'est écrit.
static const uint32_t CALIB_CHECK_IDLE_MS = 15000;

static void startCalibration() {
  if (otaRunning || mode == UiMode::Ota || mode == UiMode::Notice) return;
  Serial.println("[calib] début (anciennes bornes conservées en cas d'abandon)");
  cancelGesture();
  messageShown = false;
  mode = UiMode::Calib;
  calStep = 0;
  calDown = false;
  calArmed = false;  // un doigt encore posé (menu) ne compte pas pour la cible 1
  calFreeSinceMs = 0;
  calOutcomeShown = false;
  calLastTouchMs = millis();
  backlightWake();
  grid.drawCalibTarget(0, TOUCH_CAL_TARGETS);
}

static void calibOutcome(bool saved, const char *title, const char *detail) {
  Serial.printf("[calib] %s — %s\n", title, detail);
  grid.drawCalibOutcome(saved, title, detail);
  calOutcomeShown = true;
  calOutcomeUntilMs = millis() + 2500;
}

// Après les quatre coins : bornes calculées, puis CONTRÔLE au centre avant
// toute écriture. Des relevés cohérents entre eux peuvent rester faux (un coin
// touché à côté, deux fois pareil) : un écran mal calibré ne se recalibre pas
// lui-même, il faudrait un câble. Le contrôle le refuse avant qu'il ne soit
// trop tard.
static void cornersDone() {
  if (!touchCalFromSamples(calX, calY, UI_SCREEN_W, UI_SCREEN_H, calCandidate)) {
    calibOutcome(false, "Relevés incohérents", "Anciennes valeurs conservées");
    return;
  }
  calLastTouchMs = millis();
  grid.drawCalibTarget(TOUCH_CAL_TARGETS, TOUCH_CAL_TARGETS);
}

static void checkDone(int32_t rawX, int32_t rawY) {
  int16_t sx, sy, cx, cy;
  touchRawToScreen(calCandidate, (int16_t)rawX, (int16_t)rawY, UI_SCREEN_W, UI_SCREEN_H, sx, sy);
  touchCalTargetPos(TOUCH_CAL_TARGETS, UI_SCREEN_W, UI_SCREEN_H, cx, cy);
  const int16_t dx = (int16_t)abs(sx - cx), dy = (int16_t)abs(sy - cy);
  Serial.printf("[calib] contrôle : %d,%d pour %d,%d (écart %d/%d px)\n", sx, sy, cx, cy, dx, dy);
  if (dx > TOUCH_CAL_CHECK_PX || dy > TOUCH_CAL_CHECK_PX) {
    calibOutcome(false, "Contrôle raté", "Anciennes valeurs conservées");
    return;
  }
  if (!deviceConfig.saveCalib(calCandidate)) {
    calibOutcome(false, "Écriture impossible", "Anciennes valeurs conservées");
    return;
  }
  char detail[40];
  snprintf(detail, sizeof(detail), "X %d..%d   Y %d..%d", calCandidate.xMin, calCandidate.xMax,
           calCandidate.yMin, calCandidate.yMax);
  calibOutcome(true, "Calibration enregistrée", detail);
}

static void pollCalibration() {
  const uint32_t now = millis();
  if (calOutcomeShown) {
    if (due(calOutcomeUntilMs, 2500)) {
      calOutcomeShown = false;
      showHome();
    }
    return;
  }
  // Abandon : commande `calibrate` lancée sur un écran que personne ne
  // regarde, ou utilisateur parti. Rien n'est écrit.
  const bool checking = calStep >= TOUCH_CAL_TARGETS;
  if (now - calLastTouchMs >= (checking ? CALIB_CHECK_IDLE_MS : CALIB_IDLE_MS)) {
    calibOutcome(false, checking ? "Contrôle non fait" : "Calibration abandonnée",
                 "Anciennes valeurs conservées");
    return;
  }

  TS_Point p;
  const bool contact = touch.tirqTouched() && touch.touched() &&
                       (p = touch.getPoint()).z >= TOUCH_Z_MIN;
  if (contact) {
    calLastTouchMs = now;
    lastActivityMs = now;
    calFreeSinceMs = 0;
    if (!calArmed) return;
    if (!calDown) {
      calDown = true;
      calStartMs = now;
      calSumX = calSumY = 0;
      calN = 0;
    }
    calLostMs = 0;
    // Les premières lectures, prises pendant que la pression s'établit, sont
    // les plus bruitées : on les écarte.
    if (now - calStartMs >= 40 && calN < 200) {
      calSumX += p.x;
      calSumY += p.y;
      calN++;
    }
    return;
  }

  if (!calDown) {
    // Réarmement après un vrai relâchement : une même pression ne doit pas
    // valider deux cibles de suite.
    if (!calArmed) {
      if (!calFreeSinceMs) calFreeSinceMs = now;
      else if (now - calFreeSinceMs >= 150) calArmed = true;
    }
    return;
  }
  if (!calLostMs) {
    calLostMs = now;
    return;
  }
  if (now - calLostMs < TOUCH_RELEASE_GRACE_MS) return;

  calDown = false;
  calArmed = false;
  calFreeSinceMs = 0;
  if (calN < 4) return;  // effleurement : on attend un vrai appui sur la même cible
  const int32_t rx = calSumX / calN, ry = calSumY / calN;
  if (checking) {
    checkDone(rx, ry);
    return;
  }
  calX[calStep] = rx;
  calY[calStep] = ry;
  Serial.printf("[calib] cible %u : brut x=%ld y=%ld (%u lectures)\n", calStep + 1, (long)rx,
                (long)ry, calN);
  if (++calStep >= TOUCH_CAL_TARGETS) {
    cornersDone();
  } else {
    grid.drawCalibTarget(calStep, TOUCH_CAL_TARGETS);
  }
}

// --- Toucher simple (écrans pleins) ------------------------------------------
//
// Les écrans pleins n'ont ni tuiles ni balayage : un appui = une position au
// relâchement. Même filtrage que la grille (médiane, délai de grâce,
// contact minimal), sans le reste.
//
// ⚠️ « Armé » seulement après un vrai relâchement : on arrive souvent sur ces
// écrans le doigt encore posé (appui maintenu 5 s sur le bandeau). Sans
// cela, le relâchement de ce même doigt — sur le bandeau, donc sur « retour »
// — refermerait le menu à l'instant où il s'ouvre.
enum class TapKind : uint8_t { None, Tap, Hold };
struct TapEvt {
  TapKind kind = TapKind::None;
  int16_t x = 0, y = 0;
};
static bool stArmed = false, stDown = false, stHoldFired = false, stWoke = false;
static uint32_t stStartMs = 0, stLostMs = 0, stFreeSinceMs = 0;

static void simpleTouchDisarm() {
  stArmed = false;
  stDown = false;
  stFreeSinceMs = 0;
}

static TapEvt pollSimpleTouch(uint32_t holdMs) {
  TapEvt e;
  const uint32_t now = millis();
  TS_Point p;
  const bool contact = touch.tirqTouched() && touch.touched() &&
                       (p = touch.getPoint()).z >= TOUCH_Z_MIN;
  if (contact) {
    stFreeSinceMs = 0;
    lastActivityMs = now;
    if (!stArmed) return e;
    if (!stDown) {
      if (now - lastReleaseMs < TOUCH_DEBOUNCE_MS) return e;
      stDown = true;
      stStartMs = now;
      stHoldFired = false;
      tsCount = tsHead = 0;
      touchHaveGood = false;
      // Comme sur la grille : le premier toucher d'un écran atténué ne fait
      // que le réveiller.
      stWoke = backlightAsleep();
      backlightWake();
    }
    stLostMs = 0;
    int16_t sx, sy;
    touchRawToScreen(deviceConfig.cal(), p.x, p.y, UI_SCREEN_W, UI_SCREEN_H, sx, sy);
    touchPush(sx, sy);
    touchGoodX = touchMedian(tsX, tsCount);
    touchGoodY = touchMedian(tsY, tsCount);
    touchHaveGood = true;
    if (holdMs && !stHoldFired && !stWoke && now - stStartMs >= holdMs) {
      stHoldFired = true;
      e.kind = TapKind::Hold;
      e.x = touchGoodX;
      e.y = touchGoodY;
    }
    return e;
  }

  if (!stDown) {
    if (!stArmed) {
      if (!stFreeSinceMs) stFreeSinceMs = now;
      else if (now - stFreeSinceMs >= 2 * TOUCH_RELEASE_GRACE_MS) stArmed = true;
    }
    return e;
  }
  // Délai de grâce : une perte de contact n'est pas encore un relâchement.
  if (!stLostMs) {
    stLostMs = now;
    return e;
  }
  if (now - stLostMs < TOUCH_RELEASE_GRACE_MS) return e;

  stDown = false;
  lastReleaseMs = now;
  const uint32_t duration = stLostMs - stStartMs;
  if (stWoke) {
    stWoke = false;
    return e;
  }
  if (stHoldFired || !touchHaveGood || duration < TOUCH_MIN_CONTACT_MS) return e;
  e.kind = TapKind::Tap;
  e.x = touchGoodX;
  e.y = touchGoodY;
  return e;
}

// --- Appui sur un bouton ---------------------------------------------------

static void handleButton(uint8_t index) {
  if (index >= cur->count) return;
  LayoutButton &b = cur->buttons[index];

  // Bouton de navigation : TOUT EST LOCAL. Aucun appel réseau, donc réponse
  // instantanée — et la navigation continue de fonctionner sur le seul cache
  // NVS, écran coupé de Jeedom (contrat v2.0).
  if (b.mode == BTN_MODE_NAV) {
    grid.setPressed(-1);
    if (!grid.goToPage(b.target)) {
      Serial.printf("[ui] page %u introuvable (%u connues)\n", b.target, cur->pageCount);
    }
    return;
  }

  // Tuile `view` ou écran en lecture seule : JAMAIS de `press` (contrat
  // v3.0). hitTest() les écarte déjà ; ce garde-fou ne coûte rien et ne
  // dépend pas d'elle.
  if (b.mode == BTN_MODE_VIEW || cur->readonly) {
    grid.setPressed(-1);
    return;
  }
  // Mise en page SUE périmée (un ping a annoncé une autre `version`, ou des
  // `states`/`values` incohérents) : l'`id` de cette case peut désigner un
  // autre bouton côté serveur. Refusé jusqu'au `layout` suivant, qui arrive
  // dans la seconde — un appui à refaire vaut mieux que le portail ouvert.
  if (layoutStale) {
    Serial.printf("[press] #%ld refusé : mise en page périmée, layout en cours\n", (long)b.id);
    grid.setPressed(-1);
    scheduleLayoutSoon();
    return;
  }

  // 1) Retour visuel. Il a normalement déjà été posé au contact du doigt ;
  //    on ne le repeint que s'il manque (appui arrivé par un autre chemin).
  //    Il reste affiché jusqu'au résultat du `press`.
  if (grid.pressed() != (int8_t)index) grid.setPressed((int8_t)index);

  // 2) Demande `press` à la tâche réseau. `b.id` est une valeur OPAQUE (le
  //    rang global du bouton) : on la renvoie telle quelle, c'est le plugin qui
  //    résout la commande Jeedom. La tâche d'affichage, elle, est déjà libre :
  //    un second appui fait pendant l'appel prend sa place dans la file.
  //    Le `press` annonce le schéma dans lequel cet `id` a été reçu.
  if (netRequestPress(index, b.id, cur->schema)) {
    pressesInFlight++;
  } else {
    Serial.printf("[press] file pleine : appui #%ld abandonné\n", (long)b.id);
    grid.setPressed(-1);
  }
}

static void handleTap(const UiHit &hit) {
  switch (hit.kind) {
    case UiHit::Back:
      grid.goBack();
      break;
    case UiHit::Page:
      grid.goToPage((uint8_t)hit.index);
      break;
    case UiHit::Button:
      handleButton((uint8_t)hit.index);
      break;
    default:
      if (grid.pressed() >= 0) grid.setPressed(-1);
      break;
  }
}

// --- Lecture du tactile (grille) ---------------------------------------------

static void pollTouch() {
  const bool irq = touch.tirqTouched() && touch.touched();
  TS_Point p;
  uint16_t z = 0;
  if (irq) {
    p = touch.getPoint();
    z = (uint16_t)p.z;
  }

  // Le plancher de pression est celui de la bibliothèque (400) : `touched()`
  // a déjà tranché, et en dessous les coordonnées valent zéro. Voir
  // touch_calib.h — une hystérésis de pression serait inerte ici.
  const bool contact = irq && (z >= TOUCH_Z_MIN);

  if (contact) {
    int16_t sx, sy;
    // Bornes de la NVS (contrat v2.2), plus celles compilées.
    touchRawToScreen(deviceConfig.cal(), p.x, p.y, UI_SCREEN_W, UI_SCREEN_H, sx, sy);

    if (!touchDown) {
      if (millis() - lastReleaseMs < TOUCH_DEBOUNCE_MS) return;
      touchDown = true;
      touchStartMs = millis();
      tsCount = tsHead = 0;
      touchHaveGood = false;
      touchOffTarget = 0;
      touchLostMs = 0;
      touchCancelled = false;
      touchTargetResolved = false;
      touchButton = -1;
      touchInBanner = false;
      touchAborted = false;
      // Le premier toucher sur un écran atténué ne fait QUE le réveiller.
      touchWokeScreen = backlightAsleep();
      backlightWake();
      // Commande `message` : le toucher ferme le message SANS actionner le
      // bouton qui se trouve dessous (contrat v2.2). Même traitement qu'un
      // réveil : le geste est consommé jusqu'au relâchement.
      if (messageShown) {
        closeMessage();
        touchWokeScreen = true;
      }
    }
    lastActivityMs = millis();
    touchLostMs = 0;  // le contact est revenu (ou n'a jamais cessé)
    touchPush(sx, sy);

    // ⚠️ La position de référence n'est mise à jour que par des échantillons
    // FERMES (z >= TOUCH_Z_MIN). Quand le doigt se lève, la pression
    // s'effondre et les coordonnées partent en vrille : ce sont justement les
    // DERNIERS échantillons, ceux sur lesquels la version précédente décidait
    // de l'appui. On les ignore, et on garde la dernière position obtenue
    // sous une pression franche.
    if (z >= TOUCH_Z_MIN) {
      touchGoodX = touchMedian(tsX, tsCount);
      touchGoodY = touchMedian(tsY, tsCount);
      if (!touchHaveGood) {
        touchStartX = touchGoodX;
        touchStartY = touchGoodY;
        touchInBanner = touchStartY < UI_BANNER_H;
      }
      touchHaveGood = true;
    }
    if (touchWokeScreen || touchAborted || !touchHaveGood) return;

    // Appui MAINTENU 5 s sur le bandeau : menu Réglages (contrat v2.2). Un
    // appui bref garde son sens (retour à la page parente, au relâchement) ;
    // le doigt doit rester immobile, sinon c'est un autre geste.
    if (touchInBanner && millis() - touchStartMs >= MENU_HOLD_MS) {
      if (abs(touchGoodX - touchStartX) <= UI_TAP_SLOP &&
          abs(touchGoodY - touchStartY) <= UI_TAP_SLOP) {
        touchDown = false;
        lastReleaseMs = millis();
        openSettings();
        return;
      }
      touchInBanner = false;
    }

    // Désignation du bouton, une fois la position stabilisée.
    if (!touchTargetResolved && tsCount >= TOUCH_SETTLE_SAMPLES) {
      touchTargetResolved = true;
      const UiHit hit = grid.hitTest(touchGoodX, touchGoodY);
      if (hit.kind == UiHit::Button) {
        touchButton = hit.index;
        grid.setPressed(touchButton);  // retour visuel (règle du contrat)
      }
    }

    // Sortie de la tuile : il faut PLUSIEURS échantillons consécutifs hors
    // cible. Annuler dès le premier rendait un unique point aberrant fatal.
    if (touchButton >= 0 && !touchCancelled) {
      const UiHit h = grid.hitTest(touchGoodX, touchGoodY);
      if (h.kind != UiHit::Button || h.index != touchButton) {
        if (++touchOffTarget >= TOUCH_OFF_TARGET_N) {
          grid.setPressed(-1);
          touchCancelled = true;
        }
      } else {
        touchOffTarget = 0;
      }
    }
    return;
  }

  if (!touchDown) return;

  // ⚠️ DÉLAI DE GRÂCE. Une perte de contact n'est pas encore un relâchement :
  // la pression fluctue pendant un appui et passe régulièrement sous le
  // plancher de 400 imposé par la bibliothèque. Sans cette tolérance, un appui
  // maintenu se fragmente en plusieurs appuis — ou s'annule en cours de route.
  if (touchLostMs == 0) {
    touchLostMs = millis();
    return;
  }
  if (millis() - touchLostMs < TOUCH_RELEASE_GRACE_MS) return;

  // --- Relâchement confirmé : c'est ici que l'action part ---
  touchDown = false;
  lastReleaseMs = millis();
  lastActivityMs = lastReleaseMs;
  // La durée se compte jusqu'à la PERTE du contact, pas jusqu'à l'expiration
  // de la grâce : sinon tout appui paraîtrait 60 ms plus long qu'il ne l'est,
  // ce qui fausserait la détection du balayage.
  const uint32_t duration = touchLostMs - touchStartMs;

  if (touchWokeScreen) {
    touchWokeScreen = false;
    return;  // ce geste n'a servi qu'à rallumer (ou à fermer un message)
  }
  if (touchAborted) {
    touchAborted = false;
    return;  // l'écran a changé sous le doigt : rien ne part
  }

  // Contact trop bref ou jamais stabilisé : parasite électrique, pas un doigt.
  if (!touchHaveGood || duration < TOUCH_MIN_CONTACT_MS) {
    if (grid.pressed() >= 0) grid.setPressed(-1);
    return;
  }

  const int16_t dx = (int16_t)(touchGoodX - touchStartX);
  const int16_t dy = (int16_t)(touchGoodY - touchStartY);

  // Balayage horizontal : change de page et n'actionne RIEN.
  if (cur->swipe && cur->pageCount > 1 && duration <= UI_SWIPE_MAX_MS &&
      abs(dx) >= UI_SWIPE_MIN_DX && abs(dy) <= UI_SWIPE_MAX_DY && abs(dx) > abs(dy)) {
    if (grid.pressed() >= 0) grid.setPressed(-1);
    if (dx < 0) {
      grid.nextPage();
    } else {
      grid.prevPage();
    }
    Serial.printf("[ui] balayage %s -> page %u\n", dx < 0 ? "gauche" : "droite",
                  grid.page());
    return;
  }

  if (touchCancelled) return;

  // Dérive trop grande pour un appui : le doigt a glissé, on n'exécute rien.
  if (abs(dx) > UI_TAP_SLOP || abs(dy) > UI_TAP_SLOP) {
    if (grid.pressed() >= 0) grid.setPressed(-1);
    return;
  }

  const UiHit hit = grid.hitTest(touchGoodX, touchGoodY);
  Serial.printf("[touch] appui %d,%d (%lu ms, %u ech.) -> zone %u index %d\n", touchGoodX,
                touchGoodY, (unsigned long)duration, tsCount, (unsigned)hit.kind,
                hit.index);
  handleTap(hit);
}

// --- Résultats de la tâche réseau ---------------------------------------------

static void onLayoutDone(NetResult &res) {
  layoutInFlight = false;
  noteApiAnswer(res.r);
  lastApiResult = res.r;

  if (res.r == JeedomResult::Ok && res.layout) {
    LayoutBundle *freshBundle = res.layout;
    Layout *fresh = &freshBundle->layout;
    layoutFailures = 0;
    layoutStale = false;
    // ⚠️ Échéance RAFRAÎCHIE au succès. Laissée à sa dernière valeur, elle
    // vieillissait indéfiniment et, passé 24,8 jours, redevenait « future » :
    // la prochaine `version` bloquait l'écran en LAYOUT (voir due()).
    nextLayoutTryMs = millis();
    pingsSansStates = 0;
    pingAdoptExtras(res.extras);

    // Redessin complet seulement si la structure change ; sinon on repeint
    // uniquement les cases dont l'état a bougé (pas de clignotement).
    const bool wasEnroll = (state == AppState::ENROLL);
    const bool structureChanged =
        !layoutKnown || fresh->version != cur->version || fresh->count != cur->count ||
        fresh->pageCount != cur->pageCount || fresh->cols != cur->cols ||
        fresh->rows != cur->rows ||
        // Un changement de schéma RENUMÉROTE les `id` (contrat v3.0), même à
        // version égale : tout est redessiné, rien n'est comparé rang à rang.
        fresh->schema != cur->schema || fresh->readonly != cur->readonly;

    bool stateChanged[LAYOUT_MAX_BUTTONS] = {false};
    if (!structureChanged) {
      for (uint8_t i = 0; i < fresh->count; i++) {
        // `id` et `mode` sont comparés par simple égalité : `id` est opaque,
        // on ne lui prête aucun sens, on détecte juste qu'il a changé.
        stateChanged[i] = (fresh->buttons[i].state != cur->buttons[i].state) ||
                          (fresh->buttons[i].id != cur->buttons[i].id) ||
                          (fresh->buttons[i].mode != cur->buttons[i].mode) ||
                          (fresh->buttons[i].mode == BTN_MODE_VIEW &&
                           !tileValueEqual(freshBundle->values.v[i], curValues->v[i])) ||
                          grid.isPending(i);
      }
    }
    // Un `layout` frais fait autorité : il lève tous les états optimistes en
    // attente (filet de sécurité si le plugin ne renvoie pas encore `states`).
    grid.clearPending();

    // Échange du double tampon : la grille passe sur le tampon frais, l'ancien
    // retourne à la tâche réseau. Le cache NVS a déjà été écrit par elle.
    LayoutBundle *old = curBundle;
    curBundle = freshBundle;
    cur = &curBundle->layout;
    curValues = &curBundle->values;
    const bool hadLayout = layoutKnown;
    layoutKnown = true;
    applyBanner(res.banner);
    if (structureChanged || wasEnroll) {
      cancelGesture();  // la case sous le doigt n'est peut-être plus la même
      // Page courante conservée si elle existe encore (v2.2) — sauf en sortie
      // d'enrôlement, où l'on arrive sur l'accueil.
      grid.setLayout(cur, curValues, hadLayout && !wasEnroll);
    } else {
      grid.adoptLayout(cur, curValues);
      for (uint8_t i = 0; i < cur->count; i++) {
        if (stateChanged[i]) grid.refreshButton(i);
      }
    }
    netReturnLayout(old);
    goTo(AppState::RUN);
    // Sortie d'enrôlement : l'écran plein laisse place à la grille.
    if (wasEnroll && mode == UiMode::Home) showHome();
    refreshBanner(res.r);
    return;
  }

  // Écran pas (ou plus) déclaré dans Jeedom : on bascule sur l'écran
  // d'enrôlement, qui affiche la MAC et réinterroge toutes les 10 s.
  if (res.r == JeedomResult::ErrUnknownDevice) {
    nextLayoutTryMs = millis() + ENROLL_RETRY_MS;
    if (state != AppState::ENROLL) {
      goTo(AppState::ENROLL);
      cancelGesture();
      // Un écran atténué au moment de basculer doit pouvoir être lu : la MAC
      // est là pour être recopiée (jusqu'en v2.1, il restait dans la pénombre).
      backlightWake();
      if (mode == UiMode::Home) showHome();
    }
    return;
  }

  // Échec : on reste utilisable si un cache existe, le bandeau signale la panne.
  if (layoutFailures < 8) layoutFailures++;
  uint32_t wait = LAYOUT_RETRY_BASE_MS << (layoutFailures - 1);
  if (wait > LAYOUT_RETRY_MAX_MS) wait = LAYOUT_RETRY_MAX_MS;
  nextLayoutTryMs = millis() + wait;
  refreshBanner(res.r);

  // Clé absente ou invalide, requête malformée : rien de tout cela ne se
  // corrige avec un backoff. On journalise et on espace franchement les
  // tentatives (dans le cas ErrNoApikey, aucune requête n'est même émise).
  if (jeedomResultIsPermanent(res.r)) {
    if (res.r == JeedomResult::ErrBadRequest) {
      Serial.println("[layout] bad_request : défaut de programmation, pas de reprise rapide");
    }
    nextLayoutTryMs = millis() + LAYOUT_RETRY_MAX_MS;
  }

  if (layoutKnown && state != AppState::ENROLL) {
    // Le cache NVS suffit à rester opérationnel : on passe en RUN et le ping
    // se chargera de détecter un changement de configuration plus tard. Le
    // premier ping part à l'échéance qu'aurait eue le `layout` suivant.
    goTo(AppState::RUN);
    pingPoke(wait);
  }
}

static void onPressDone(const NetResult &res) {
  if (pressesInFlight) pressesInFlight--;
  noteApiAnswer(res.r);
  lastApiResult = res.r;

  // La mise en page a pu changer pendant l'appel : on ne touche au bouton que
  // s'il porte toujours le même `id` au même rang.
  const uint8_t i = res.index;
  const bool same = i < cur->count && cur->buttons[i].id == res.id &&
                    cur->buttons[i].mode != BTN_MODE_NAV;

  if (res.r == JeedomResult::Ok) {
    // Un ping émis APRÈS ce `press` a déjà été appliqué : son `states` reflète
    // (au moins) l'exécution, et il fait autorité. Poser par-dessus l'état
    // optimiste « en attente » le laisserait affiché jusqu'au ping suivant —
    // 25 s en attente longue — pour une information déjà dépassée.
    const bool superseded = lastPingAppliedStartMs &&
                            (int32_t)(lastPingAppliedStartMs - res.startedMs) > 0;
    if (same && superseded) {
      Serial.printf("[press] #%ld : un ping plus récent fait déjà foi\n", (long)res.id);
    } else if (same) {
      // Retour visuel OPTIMISTE : `state` est l'état ATTENDU, pas constaté.
      // Le bouton est marqué « en attente » ; le `states` suivant tranchera.
      // Pas d'écriture NVS : un état optimiste n'a rien à faire dans le cache.
      cur->buttons[i].state = res.press.state;
      grid.setPending(i, res.press.pending);
      // Sans attente longue, on avance le ping pour confirmer vite. Avec, le
      // ping retenu revient de lui-même dès que l'état change.
      if (res.press.pending && !pingLongPollActive()) pingPoke(PRESS_CONFIRM_DELAY_MS);
    }
  } else if (res.r == JeedomResult::ErrReadOnly) {
    // Le plugin refuse : l'écran est en lecture seule côté Jeedom alors que
    // notre mise en page ne le dit pas encore. Le `layout` suivant le dira.
    Serial.printf("[press] #%ld refusé : écran en lecture seule\n", (long)res.id);
  } else if (res.r == JeedomResult::ErrBadRequest) {
    // Défaut de programmation, pas un incident réseau : on journalise et on
    // passe à autre chose, sans backoff ni nouvelle tentative.
    Serial.printf("[press] bad_request sur le bouton #%ld : requête malformée\n", (long)res.id);
  } else {
    Serial.printf("[press] échec bouton #%ld : %s\n", (long)res.id, jeedomResultLabel(res.r));
  }

  if (grid.pressed() == (int8_t)i) {
    grid.setPressed(-1);  // repeint la case, avec l'état optimiste s'il y en a un
  } else if (same) {
    grid.refreshButton(i);
  }
  refreshBanner(res.r);
}

// Conditions réunies pour lancer un OTA sans risquer d'interrompre
// l'utilisateur ni d'empiler deux mises à jour.
static bool otaMayStart() {
  if (state != AppState::RUN) return false;  // pas en plein enrôlement
  if (!wifi.connected() || wifi.trialRunning()) return false;
  if (!apikeyOk) return false;
  if (touchDown) return false;               // un doigt est posé
  if (pressesInFlight) return false;         // un appui est en cours de traitement
  if (mode != UiMode::Home || messageShown) return false;  // menu, calibration…
  if (portalActive()) return false;
  // ⚠️ Jamais d'OTA sur un firmware qui n'a pas encore fait ses preuves :
  // on écraserait la partition de repli qui nous sauve.
  if (fwPendingVerify) return false;
  return true;
}

// Le CONTRÔLE (sans installation) est permis bien plus largement que
// l'installation : y compris avant validation, dont il est l'une des preuves.
static bool otaMayCheck() {
  return state == AppState::RUN && wifi.connected() && !wifi.trialRunning() && apikeyOk &&
         !otaRunning;
}

static void onFirmwareDone(const NetResult &res) {
  fwCheckInFlight = false;
  noteApiAnswer(res.r);
  // Preuve 5 (contrat v3.0) : toute réponse JSON bien formée prouve le
  // chemin — `firmware_unavailable` ou annonce incomplète compris. Pas
  // `bad_apikey` : la chaîne d'authentification, elle, est en échec.
  if (res.jsonOk && res.r != JeedomResult::ErrBadApikey) fwCheckOkSeen = true;
  if (res.r != JeedomResult::Ok) {
    if (fwPendingVerify) {
      nextOtaCheckMs = millis() + OTA_VERIFY_CHECK_RETRY_MS;
    } else if (jeedomResultIsPermanent(res.r)) {
      // `firmware_unavailable` notamment : le plugin annonce une mise à jour
      // dont le fichier a disparu. Rien à gagner à s'acharner.
      Serial.printf("[ota] %s : pas de reprise rapide\n", jeedomResultLabel(res.r));
      nextOtaCheckMs = millis() + OTA_RETRY_AFTER_FAIL_MS;
    }
    return;
  }
  if (!res.update) return;  // à jour, ou OTA verrouillé côté serveur

  // ⚠️ Jamais d'installation depuis un firmware pas encore validé : on
  // écraserait la partition de repli. Le contrôle reprendra après validation.
  if (fwPendingVerify) {
    Serial.printf("[ota] %s proposée, mais firmware courant pas encore validé : pas "
                  "d'installation\n", res.version);
    return;
  }
  // Version déjà rejetée par un retour arrière : ne pas boucler dessus.
  if (!otaJournalAllows(res.version)) {
    nextOtaCheckMs = millis() + OTA_RETRY_AFTER_FAIL_MS;
    return;
  }

  // Les conditions ont pu changer pendant le contrôle (un doigt s'est posé,
  // le menu s'est ouvert) : on revérifie, et on reporte plutôt que d'imposer.
  if (!otaMayStart()) {
    Serial.println("[ota] mise à jour annoncée, écran occupé : nouvel essai dans 1 min");
    nextOtaCheckMs = millis() + OTA_DEFER_MS;
    return;
  }

  // La version VISÉE est notée par otaApply(), APRÈS Update.end() réussi
  // (2.3.1) : notée ici, un téléchargement coupé puis un redémarrage l'auraient
  // fait passer pour un rejet.
  Serial.printf("[ota] installation de %s (actuel %s)\n", res.version, GLOWSCREEN_FW_VERSION);
  cancelGesture();
  otaRunning = true;
  otaFailed = false;
  // Contrat v2.2 : l'attente longue est suspendue pendant le téléchargement.
  pingSuspend(true);
  backlightWake();  // une mise à jour se regarde : pas d'écran atténué
  mode = UiMode::Ota;
  grid.drawOtaScreen(GLOWSCREEN_FW_VERSION, res.version);
  if (!netRequestOtaApply()) {
    grid.drawOtaFailure("file réseau pleine");
    otaFailed = true;
    otaFailUntilMs = millis() + 4000;
  }
}

static void onOtaDone(const NetResult &res) {
  if (res.outcome == OtaOutcome::Success) {
    Serial.println("[ota] redémarrage sur le nouveau firmware");
    Serial.flush();
    delay(500);
    ESP.restart();
    return;  // jamais atteint
  }
  // Échec : le firmware actuel est intact. On le dit à l'écran, on laisse le
  // message visible quelques secondes, puis on reprend le fonctionnement
  // normal. Un OTA raté ne doit JAMAIS immobiliser l'écran.
  otaJournalClearTarget();  // rien n'a été installé : pas de rejet à mémoriser
  grid.drawOtaFailure(otaOutcomeLabel(res.outcome));
  otaFailed = true;
  otaFailUntilMs = millis() + 4000;
  nextOtaCheckMs = millis() + OTA_RETRY_AFTER_FAIL_MS;
}

static void onNetResult(NetResult &res) {
  switch (res.kind) {
    case NetEvent::LayoutDone:   onLayoutDone(res); break;
    case NetEvent::PressDone:    onPressDone(res); break;
    case NetEvent::FirmwareDone: onFirmwareDone(res); break;
    case NetEvent::OtaProgress:
      if (mode == UiMode::Ota && !otaFailed) grid.drawOtaProgress(res.pct);
      break;
    case NetEvent::OtaDone:      onOtaDone(res); break;
    case NetEvent::ProbeDone:    onProbeDone(res); break;
  }
}

// --- Commandes à distance (contrat v2.2) -------------------------------------

static void showMessage(const char *text, uint16_t durationS) {
  cancelGesture();  // un doigt posé ne doit pas actionner la case sous le message
  if (mode != UiMode::Home) showHome();  // referme réglages, portail, identification
  if (state == AppState::ENROLL) return;  // pas de grille sous un enrôlement
  backlightWake();
  messageShown = true;
  messageUntilMs = millis() + (uint32_t)durationS * 1000UL;
  grid.drawMessage(text);
}

static void startIdentify(uint16_t durationS) {
  cancelGesture();
  messageShown = false;
  mode = UiMode::Identify;
  identifyUntilMs = millis() + (uint32_t)durationS * 1000UL;
  identifyNextMs = millis();
  identifyPhase = 0;
  backlightWake();
  char ip[16];
  strlcpy(ip, wifi.connected() ? WiFi.localIP().toString().c_str() : "hors ligne", sizeof(ip));
  grid.drawIdentify(layoutKnown ? cur->name : "GlowScreen", wifi.macPretty(), ip,
                    GLOWSCREEN_FW_VERSION);
  simpleTouchDisarm();
}

static void endIdentify() {
  setLed(false, false, false);
  ledcWrite(UI_BL_CHANNEL, UI_BL_FULL);
  backlightDim = false;
  lastActivityMs = millis();
  showHome();
}

static void handleCmd(const RemoteCmd &c) {
  if (c.verb == CmdVerb::None) return;
  // Livraison « au plus une fois » côté serveur ; côté carte, on ignore une
  // commande dont le `seq` est celui de la dernière exécutée.
  if (haveCmdSeq && c.seq == lastCmdSeq) {
    Serial.printf("[cmd] #%lu déjà exécutée : ignorée\n", (unsigned long)c.seq);
    return;
  }
  haveCmdSeq = true;
  lastCmdSeq = c.seq;
  Serial.printf("[cmd] #%lu « %s »\n", (unsigned long)c.seq, c.name);

  const bool busy = uiBusy();
  switch (c.verb) {
    case CmdVerb::Reboot:
      // Un ping retenu, parti avant l'OTA, peut encore livrer un `reboot` :
      // couper un téléchargement en cours ne casserait rien (la partition
      // n'est basculée qu'après vérification) mais le gâcherait.
      if (otaRunning) {
        Serial.println("[cmd] reboot refusé : mise à jour en cours");
        return;
      }
      // Avant validation, un redémarrage déclencherait le retour arrière et
      // ferait REJETER une version peut-être saine (contrat v3.0).
      if (fwPendingVerify) {
        Serial.println("[cmd] reboot refusé : firmware pas encore validé");
        return;
      }
      scheduleReboot("commande Jeedom", 1000);
      break;
    case CmdVerb::Identify:
      if (busy) break;
      startIdentify(c.duration);
      break;
    case CmdVerb::Message:
      if (busy) break;
      showMessage(c.text, c.duration);
      break;
    case CmdVerb::Page:
      if (busy) break;
      cancelGesture();  // la page change sous le doigt
      if (mode != UiMode::Home) showHome();
      closeMessage();
      backlightWake();
      if (state != AppState::ENROLL && !grid.goToPage(c.page)) {
        Serial.printf("[cmd] page %u inexistante (%u pages)\n", c.page, cur->pageCount);
      }
      break;
    case CmdVerb::Calibrate:
      if (busy) break;
      startCalibration();
      break;
    case CmdVerb::Ota:
      // Toujours soumis aux deux verrous du serveur : on ne fait qu'avancer
      // le contrôle. Et jamais sur un firmware pas encore validé.
      if (fwPendingVerify) {
        Serial.println("[cmd] ota refusée : firmware courant pas encore validé");
      } else {
        nextOtaCheckMs = millis();
      }
      break;
    case CmdVerb::Wifi:
      if (c.ssid[0] == '\0') {
        Serial.println("[cmd] wifi sans ssid : ignorée");
      } else if (otaRunning) {
        // Couper la liaison sous un téléchargement le ferait échouer.
        Serial.println("[cmd] wifi refusée : mise à jour en cours");
      } else {
        // Hôte et clé inchangés : l'essai porte sur le Wi-Fi seul.
        DeviceConfig cfg;
        deviceConfig.copy(cfg);
        startCredTrial(c.ssid, c.pass, cfg.host, cfg.apikey, false);
        memset(&cfg, 0, sizeof(cfg));
      }
      break;
    case CmdVerb::Unknown:
    default:
      Serial.printf("[cmd] verbe inconnu « %s » : ignoré\n", c.name);
      return;
  }
  if (busy && c.verb != CmdVerb::Reboot && c.verb != CmdVerb::Ota && c.verb != CmdVerb::Wifi) {
    Serial.println("[cmd] écran occupé (calibration, OTA ou redémarrage) : ignorée");
  }
}

// --- Rafraîchissement par le ping ---------------------------------------------

// Applique le tableau `states` du ping (contrat v1.2, réindexé en v2.0) :
// seules les pastilles qui ont réellement bougé sont repeintes, la grille
// n'est pas redessinée — et les boutons des pages non affichées sont mis à
// jour dans le modèle sans provoquer le moindre tracé.
// Renvoie false si le tableau est incohérent -> l'appelant force un `layout`.
static bool applyStates(const PingInfo &info) {
  if (info.stateCount != cur->count) {
    Serial.printf("[api] states de taille %u pour %u boutons : ignoré\n", info.stateCount,
                  cur->count);
    return false;
  }
  // `states` fait autorité : s'il contredit l'état optimiste affiché après un
  // `press`, on adopte sa valeur sans autre forme de procès (contrat v1.3).
  for (uint8_t i = 0; i < cur->count; i++) {
    LayoutButton &b = cur->buttons[i];
    // Navigation et tuile `view` occupent une case de `states`, toujours null.
    if (b.mode == BTN_MODE_NAV || b.mode == BTN_MODE_VIEW) continue;
    // Contrat v2.0 : `states` est indexé par l'`id` GLOBAL, pas par le rang
    // de stockage. Les deux coïncident quand le plugin est bien rangé, mais
    // s'appuyer sur l'`id` rend la lecture insensible à un changement
    // d'ordre des pages.
    if (b.id < 0 || b.id >= (int32_t)info.stateCount) continue;
    const int8_t fresh = info.states[b.id];
    const bool wasPending = grid.isPending(i);
    if (b.state == fresh && !wasPending) continue;
    b.state = fresh;
    grid.setPending(i, false);
    grid.refreshButton(i);
  }
  // Pas d'écriture NVS ici : le cache ne sert qu'à peupler l'écran pendant les
  // deux secondes précédant le premier `layout`. Ne pas user la flash pour ça.
  return true;
}

// Demande un `layout` dès que possible SANS effacer le backoff en cours : un
// ping réussi prouve que le serveur répond aux pings, pas que `layout` (plus
// lourd, et en échec à l'instant) a cessé d'échouer. Jusqu'ici un ping remettait
// `layoutFailures` à zéro et relançait aussitôt : un `layout` cassé côté
// serveur était réessayé à chaque ping au lieu d'espacer jusqu'à 60 s.
static void scheduleLayoutSoon() {
  // Backoff en cours (échéance À VENIR, et plausible) : on le respecte.
  // Sinon — rien de prévu, ou une échéance périmée depuis des semaines —
  // tout de suite.
  if (due(nextLayoutTryMs, LAYOUT_RETRY_MAX_MS)) nextLayoutTryMs = millis();
}

// `values` du ping (schéma 3) : MÊME règle que `states` — longueur différente
// du nombre de tuiles connues -> ignoré, `layout` forcé. Indexé par l'`id`
// global. Seules les tuiles `view` dont la valeur ou le ton a changé sont
// repeintes, et drawButton() ne trace rien pour une page non affichée.
static bool applyValues(const PingInfo &info) {
  if (info.valueCount != cur->count) {
    Serial.printf("[api] values de taille %u pour %u tuiles : ignoré\n", info.valueCount,
                  cur->count);
    return false;
  }
  for (uint8_t i = 0; i < cur->count; i++) {
    const LayoutButton &b = cur->buttons[i];
    if (b.mode != BTN_MODE_VIEW) continue;
    if (b.id < 0 || b.id >= (int32_t)info.valueCount) continue;
    const TileValue &fresh = info.values[b.id];
    if (tileValueEqual(fresh, curValues->v[i])) continue;
    curValues->v[i] = fresh;  // volatil : jamais en NVS
    grid.refreshButton(i);
  }
  return true;
}

static void onPing(const PingResult &pr) {
  const JeedomResult r = pr.r;
  const PingInfo &info = pr.info;
  noteApiAnswer(r);
  lastApiResult = r;
  if (r == JeedomResult::Ok) {
    applyBanner(info.banner);
    // Preuves de validation : la tâche ping tourne, et l'attente longue
    // aussi si elle était demandée (retenue ou non, terminée normalement).
    pingOkSeen = true;
    if (pr.longPoll) longPollPingSeen = true;
  }
  refreshBanner(r);

  // La commande est traitée QUEL QUE SOIT l'état : le serveur l'a déjà
  // retirée de sa file (livraison au plus une fois), l'ignorer la perdrait.
  if (r == JeedomResult::Ok) handleCmd(info.cmd);

  // Un ping retenu peut revenir après un changement d'état (layout en cours) :
  // seul RUN en tire des conclusions sur la mise en page.
  if (state != AppState::RUN) return;

  if (r == JeedomResult::ErrUnknownDevice) {
    // L'équipement a été supprimé dans Jeedom pendant le fonctionnement.
    layoutStale = true;
    scheduleLayoutSoon();
    goTo(AppState::LAYOUT);      // LAYOUT confirmera et basculera en ENROLL
    return;
  }
  if (r == JeedomResult::ErrBadRequest) {
    // Défaut de programmation : on journalise, on garde la cadence normale.
    Serial.println("[ping] bad_request : requête malformée, pas de reprise");
    return;
  }
  if (r != JeedomResult::Ok) return;

  Serial.printf("[ping] v%ld, RSSI %d dBm%s%s\n", (long)info.version, wifi.rssi(),
                info.hasStates ? "" : " (pas de champ states)",
                pingLongPollActive() ? ", attente longue" : "");

  // 1) Changement de configuration : rechargement complet.
  if (!layoutKnown || info.version != cur->version) {
    Serial.printf("[api] version %ld -> %ld, rechargement de la mise en page\n",
                  (long)(layoutKnown ? cur->version : -1), (long)info.version);
    layoutStale = true;  // les `id` affichés ne sont plus sûrs
    scheduleLayoutSoon();
    goTo(AppState::LAYOUT);
    return;
  }

  // 2) Valeurs des tuiles `view` (schéma 3). Absentes d'un plugin plus
  //    ancien : rien à faire, les tuiles restent à « — ».
  if (info.hasValues && !applyValues(info)) {
    layoutStale = true;  // les `id` affichés ne sont plus sûrs
    scheduleLayoutSoon();
    goTo(AppState::LAYOUT);
    return;
  }

  // 3) Mise à jour des pastilles depuis `states` (v1.2).
  if (info.hasStates) {
    pingsSansStates = 0;
    if (applyStates(info)) {
      lastPingAppliedStartMs = pr.startedMs ? pr.startedMs : 1;
    } else {
      layoutStale = true;
      scheduleLayoutSoon();
      goTo(AppState::LAYOUT);
    }
    return;
  }

  // 4) Plugin sans `states` : repli périodique par `layout` complet.
  if (++pingsSansStates >= LAYOUT_RESYNC_PINGS) {
    pingsSansStates = 0;
    Serial.println("[api] pas de champ states : resynchronisation par layout");
    scheduleLayoutSoon();
    goTo(AppState::LAYOUT);
  }
}

// --- Validation du firmware (retour arrière automatique) -------------------

static void updateFirmwareVerification() {
  if (!fwPendingVerify) return;

  // Attente longue annoncée : il faut qu'un ping retenu ait abouti. Non
  // annoncée : le ping simple suffit.
  const bool longPollProved = !pingLongPollActive() || longPollPingSeen;
  const bool uptimeOk = millis() >= OTA_VERIFY_MIN_UPTIME_MS;
  // Preuves 4 et 5 normales, OU leur remplacement sur un écran non déclaré
  // (ENROLL : deux `unknown_device` espacées d'au moins une minute).
  const bool runProofs = pingOkSeen && longPollProved && fwCheckOkSeen;
  const bool enrollProofs = state == AppState::ENROLL && enrollProof;
  if (screenReady && wifi.connected() && apiAnswered && (runProofs || enrollProofs) &&
      uptimeOk) {
    if (enrollProofs && !runProofs) Serial.println("[ota] validé en ENROLL (unknown_device x2)");
    otaMarkValid();
    otaJournalClearTarget();  // la version visée a fait ses preuves
    fwPendingVerify = false;
    // Le premier contrôle OTA n'est autorisé qu'une fois ce cap franchi.
    nextOtaCheckMs = millis() + OTA_FIRST_CHECK_MS;
    return;
  }

  if ((int32_t)(millis() - verifyDeadlineMs) >= 0) {
    Serial.printf("[ota] non valide après %lu s (écran=%d wifi=%d api=%d ping=%d "
                  "attente-longue=%d firmware=%d enroll=%d)\n",
                  (unsigned long)(OTA_VERIFY_TIMEOUT_MS / 1000), screenReady,
                  wifi.connected(), apiAnswered, pingOkSeen, longPollProved, fwCheckOkSeen,
                  enrollProof);
    otaRollback();  // ne revient pas
  }
}

// --- Boucle : morceaux -------------------------------------------------------

static void requestLayout() {
  if (netRequestLayout()) {
    layoutInFlight = true;
  } else {
    nextLayoutTryMs = millis() + 1000;
  }
}

// Machine à états réseau : ne fait que DÉCIDER et DEMANDER, jamais attendre.
static void stepStateMachine() {
  const uint32_t now = millis();
  switch (state) {
    case AppState::BOOT:
      goTo(AppState::WIFI);
      break;

    case AppState::WIFI:
      if (wifi.connected()) {
        layoutFailures = 0;
        nextLayoutTryMs = now;
        goTo(AppState::LAYOUT);
      }
      break;

    case AppState::LAYOUT:
      if (!wifi.connected()) {
        goTo(AppState::WIFI);
      } else if (!layoutInFlight && due(nextLayoutTryMs, LAYOUT_RETRY_MAX_MS)) {
        requestLayout();
      }
      break;

    case AppState::ENROLL:
      // Écran non déclaré : on n'affiche aucun bouton, on réinterroge Jeedom
      // toutes les 10 s et on bascule tout seul dès qu'il répond.
      if (!layoutInFlight && due(nextLayoutTryMs, LAYOUT_RETRY_MAX_MS)) {
        if (wifi.connected()) {
          requestLayout();
        } else {
          nextLayoutTryMs = now + ENROLL_RETRY_MS;
        }
      }
      break;

    case AppState::RUN:
      // L'OTA passe en dernier : jamais pendant un appui en cours de
      // traitement, jamais sur un firmware non encore validé.
      // Le contrôle part aussi AVANT validation (c'est une de ses preuves) ;
      // l'installation, elle, reste soumise à otaMayStart() à la réponse.
      if (!fwCheckInFlight && due(nextOtaCheckMs, OTA_RETRY_AFTER_FAIL_MS) && otaMayCheck()) {
        if (netRequestFirmware()) {
          fwCheckInFlight = true;
          nextOtaCheckMs = now + OTA_CHECK_PERIOD_MS;
        }
      }
      break;
  }
}

static void onWifiChange() {
  refreshBanner(wifi.connected() ? lastApiResult : JeedomResult::ErrNoWifi);
  if (wifi.connected()) {
    wifiDown = false;
    // Portail ouvert faute de Wi-Fi : il n'a plus de raison d'être (contrat).
    // Ouvert depuis le menu, il reste — l'utilisateur est peut-être en train
    // de changer de réseau.
    if (portalActive() && portalAuto && !s_trial.active) {
      Serial.println("[portail] Wi-Fi retrouvé : fermeture");
      closePortal();
      if (mode == UiMode::Portal) showHome();
    }
  } else {
    wifiDown = true;
    wifiDownSinceMs = millis();
    if (state == AppState::RUN) goTo(AppState::WIFI);  // sans effacer l'écran
  }
  if (state == AppState::ENROLL && mode == UiMode::Home) showHome();  // pied de page
}

// Interaction propre au mode courant.
static void stepUi() {
  const uint32_t now = millis();
  switch (mode) {
    case UiMode::Home:
      if (state == AppState::ENROLL) {
        // Écran d'enrôlement : un toucher réveille, un appui maintenu 5 s
        // ouvre le menu (calibration, portail, sans câble).
        if (pollSimpleTouch(MENU_HOLD_MS).kind == TapKind::Hold) openSettings();
      } else {
        // Toujours tactile, même sans mise en page : le bandeau reste la
        // porte d'entrée du menu sur une carte neuve qui n'a jamais eu de
        // Wi-Fi.
        pollTouch();
        grid.tickClock();
      }
      if (messageShown && due(messageUntilMs, 600000UL)) closeMessage();
      break;

    case UiMode::Settings: {
      const TapEvt e = pollSimpleTouch(0);
      if (e.kind == TapKind::Tap) {
        settingsUntilMs = now + SETTINGS_IDLE_MS;
        switch (grid.settingsHitTest(e.x, e.y)) {
          case ScreenHit::Back:      showHome(); return;
          case ScreenHit::Calibrate: startCalibration(); return;
          case ScreenHit::Portal:    openPortal(false); return;
          case ScreenHit::Reboot:
            if (rebootConfirm) {
              scheduleReboot("menu Réglages", 800);
              return;
            }
            rebootConfirm = true;
            rebootConfirmUntilMs = now + 3000;
            fillSettings(s_settings);
            grid.drawSettings(s_settings);
            break;
          default:
            break;
        }
      }
      if (rebootConfirm && due(rebootConfirmUntilMs, 3000)) {
        rebootConfirm = false;
        fillSettings(s_settings);
        grid.drawSettings(s_settings);
      }
      if (due(settingsUntilMs, SETTINGS_IDLE_MS)) {
        showHome();
      } else if (due(screenRefreshMs, SCREEN_REFRESH_MS)) {
        screenRefreshMs = now + SCREEN_REFRESH_MS;
        fillSettings(s_settings);
        grid.drawSettingsInfo(s_settings);
      }
      break;
    }

    case UiMode::Portal: {
      const TapEvt e = pollSimpleTouch(0);
      if (e.kind == TapKind::Tap) {
        switch (grid.portalHitTest(e.x, e.y)) {
          case ScreenHit::Back:  // masquer : le portail reste ouvert
            showHome();
            return;
          case ScreenHit::ClosePortal:
            closePortal();
            showHome();
            refreshBanner(lastApiResult);
            return;
          default:
            break;
        }
      }
      if (due(screenRefreshMs, SCREEN_REFRESH_MS)) {
        screenRefreshMs = now + SCREEN_REFRESH_MS;
        PortalInfo p;
        fillPortal(p);
        grid.drawPortalStatus(p);
      }
      break;
    }

    case UiMode::Calib:
      pollCalibration();
      break;

    case UiMode::Identify:
      // Clignotement rétroéclairage + LED RVB : de quoi repérer l'écran dans
      // un couloir. Un toucher l'arrête.
      if (pollSimpleTouch(0).kind == TapKind::Tap || due(identifyUntilMs, 120000UL)) {
        endIdentify();
        break;
      }
      if (due(identifyNextMs, 400)) {
        identifyNextMs = now + 400;
        identifyPhase++;
        ledcWrite(UI_BL_CHANNEL, (identifyPhase & 1) ? UI_BL_FULL : 20);
        const uint8_t c = identifyPhase % 3;
        setLed(c == 0, c == 1, c == 2);
      }
      break;

    case UiMode::Ota:
      if (otaFailed && due(otaFailUntilMs, 4000)) {
        otaFailed = false;
        otaRunning = false;
        pingSuspend(false);
        showHome();
        refreshBanner(lastApiResult);
      }
      break;

    case UiMode::Notice:
      break;
  }
}

// --- setup / loop ----------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== GlowScreen32 ===");

  // LED RVB éteinte (actif bas) : elle ne sert qu'à la commande `identify`.
  pinMode(CYD_LED_R, OUTPUT);
  pinMode(CYD_LED_G, OUTPUT);
  pinMode(CYD_LED_B, OUTPUT);
  setLed(false, false, false);

  // La référence à GLOW_FW_TAG n'est pas décorative : c'est elle qui garantit
  // que le marqueur survit à `--gc-sections` et reste présent dans le .bin,
  // où le plugin Jeedom va le chercher (voir fw_version.h).
  Serial.printf("[boot] %s sur la partition %s\n", GLOW_FW_TAG, otaRunningPartition());

  // ⚠️ On NE déclare PAS le firmware sain ici. S'il vient d'être installé par
  // OTA, il doit d'abord prouver que l'écran, le Wi-Fi et l'API fonctionnent
  // (voir updateFirmwareVerification). Marquer valide dès setup() reviendrait
  // à désactiver le filet de sécurité tout en croyant l'avoir.
  // Retour arrière à constater AVANT tout : une version visée qui n'est pas
  // celle qui tourne a été rejetée, et ne doit plus être réinstallée.
  otaJournalBoot(GLOWSCREEN_FW_VERSION);
  fwPendingVerify = otaPendingVerify();
  if (fwPendingVerify) {
    verifyDeadlineMs = millis() + OTA_VERIFY_TIMEOUT_MS;
    Serial.printf("[ota] firmware à confirmer : %lu s pour faire ses preuves\n",
                  (unsigned long)(OTA_VERIFY_TIMEOUT_MS / 1000));
  }

  // Configuration locale : NVS, ou valeurs d'usine au premier démarrage.
  deviceConfig.begin();
  DeviceConfig cfg;
  deviceConfig.copy(cfg);
  strlcpy(s_host, cfg.host, sizeof(s_host));
  apikeyOk = jeedomApikeyConfigured(cfg.apikey);
  if (!apikeyOk) {
    Serial.println("[api] clé API absente : aucune requête ne sera émise. La saisir par le");
    Serial.println("[api] portail Wi-Fi (Réglages > Wi-Fi) ou dans src/secrets.h.");
  }

  tft.init();
  tft.setRotation(1);  // paysage, 320x240
  screenReady = true;  // première des trois conditions de validation

  // ⚠️ APRÈS tft.init() : TFT_eSPI pilote lui-même IO21 à l'initialisation
  // (build flag -DTFT_BL=21). L'attacher au LEDC avant serait écrasé.
  backlightInit();

  touchSpi.begin(CYD_TOUCH_CLK, CYD_TOUCH_MISO, CYD_TOUCH_MOSI, CYD_TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(1);

  layoutClear(s_bundleA.layout);
  layoutClear(s_bundleB.layout);
  memset(&s_bundleA.values, 0, sizeof(TileValues));  // « — » jusqu'au premier ping
  memset(&s_bundleB.values, 0, sizeof(TileValues));
  grid.begin(&tft);
  grid.setFwVersion(GLOWSCREEN_FW_VERSION);
  grid.setWifi(false);
  grid.setApi(UiLink::Offline, "Démarrage");

  // Cache NVS : les boutons s'affichent avant même d'avoir du réseau.
  // Un cache appartenant à une autre MAC est refusé et effacé. ⚠️ Lu ICI,
  // avant le lancement des tâches : ensuite, seule la tâche réseau touche au
  // cache (et à ses tampons de travail statiques).
  store.begin();
  layoutKnown = store.load(*cur, wifi.deviceId());
  grid.setLayout(cur, curValues);

  // L'identité de la carte est sa MAC : rien n'est compilé en dur, le même
  // binaire tourne sur tout le parc (ex. carte de test : 24:6f:28:12:34:56).
  wifi.begin(cfg.ssid, cfg.pass);
  netTasksBegin(&store, cfg.host, cfg.apikey, wifi.deviceId(), &s_bundleB);
  memset(&cfg, 0, sizeof(cfg));  // pas de secret qui traîne sur la pile

  const uint32_t now = millis();
  nextOtaCheckMs = now + OTA_FIRST_CHECK_MS;
  wifiDownSinceMs = now;
  nextStackLogMs = now + STACK_LOG_FIRST_MS;

  Serial.printf("[boot] device=%s, cache=%s, clé API=%s, RAM libre %u o\n", wifi.deviceId(),
                layoutKnown ? "oui" : "non", apikeyOk ? "ok" : "ABSENTE",
                (unsigned)ESP.getFreeHeap());

  refreshBanner(JeedomResult::ErrNoWifi);
  goTo(AppState::WIFI);
}

void loop() {
  wifi.loop();
  // Pendant qu'un appareil est sur le portail, la station espace ses essais :
  // chacun balaie tous les canaux et fait décrocher le point d'accès.
  wifi.setRetryCeiling(portalClients() ? 60000 : 30000);
  portalService();
  // Portail ouvert depuis le menu et oublié : refermé après 20 min sans
  // client (l'automatique, lui, se referme au retour du Wi-Fi).
  if (portalRunning() && !portalAuto && !s_trial.active) {
    if (portalClients()) {
      portalLastClientMs = millis();
    } else if (millis() - portalLastClientMs >= PORTAL_MENU_IDLE_MS) {
      Serial.println("[portail] 20 min sans client : fermeture");
      closePortal();
      if (mode == UiMode::Portal) showHome();
    }
  }

  // Filet de sécurité : à traiter en premier, avant toute autre logique.
  updateFirmwareVerification();

  // Toute transition de la liaison Wi-Fi se voit immédiatement au bandeau.
  if (wifi.consumeChange()) onWifiChange();
  if (!wifi.connected() && state == AppState::RUN) goTo(AppState::WIFI);

  // Essai d'identifiants (commande `wifi`, portail) : NVS seulement en cas
  // de succès complet ; sinon les anciens sont déjà rétablis.
  stepCredTrial();

  // Résultats des tâches réseau : appliqués ICI, seule tâche qui dessine.
  while (netPollResult(s_net)) onNetResult(s_net);
  while (PingResult *pr = pingPollResult()) {
    onPing(*pr);
    pingReturnResult(pr);  // rendu aussitôt : la tâche ping en a besoin pour relancer
  }

  // Le ping ne tourne qu'en RUN, et jamais pendant un OTA.
  pingConfigure(state == AppState::RUN && wifi.connected() && apikeyOk && !otaRunning,
                layoutKnown ? (uint32_t)cur->poll : 30, backlightAsleep());

  stepStateMachine();
  stepUi();

  // Portail de secours : sans Wi-Fi depuis 5 min, et seulement sur l'écran
  // d'accueil (jamais au milieu d'une calibration ou d'un menu).
  if (wifiDown && !portalActive() && mode == UiMode::Home && !messageShown && !otaRunning &&
      millis() - wifiDownSinceMs >= PORTAL_AUTO_MS) {
    openPortal(true);
  }
  // Formulaire du portail : rien n'est écrit avant un essai réussi.
  static PortalSubmission sub;
  if (portalTakeSubmission(sub)) {
    startCredTrial(sub.ssid, sub.pass, sub.host, sub.apikey, true);
    memset(&sub, 0, sizeof(sub));
  }
  if (rebootPending && due(rebootAtMs, 5000)) {
    Serial.flush();
    ESP.restart();
  }

  // L'atténuation ne doit jamais surprendre un doigt posé, un enrôlement, ni
  // un écran qui attend d'être lu.
  const bool keepAwake = touchDown || stDown || state == AppState::ENROLL || messageShown ||
                         mode == UiMode::Identify || mode == UiMode::Calib ||
                         mode == UiMode::Ota || mode == UiMode::Notice;
  if (!keepAwake) backlightLoop();

  // Niveau maximal des piles : au démarrage, puis toutes les 10 minutes.
  if (due(nextStackLogMs, STACK_LOG_PERIOD_MS)) {
    nextStackLogMs = millis() + STACK_LOG_PERIOD_MS;
    netLogStacks();
  }

  delay(5);  // laisse respirer les tâches Wi-Fi/IDLE sans figer l'UI
}
