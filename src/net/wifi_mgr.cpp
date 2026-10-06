#include "wifi_mgr.h"

#include <esp_mac.h>

// Durée maximale d'une tentative d'association avant de repartir de zéro.
// Un balayage de TOUS les canaux (voir begin) coûte ~2 s de plus qu'un
// balayage rapide : 12 s laissent largement la marge.
static const uint32_t WIFI_ATTEMPT_TIMEOUT_MS = 12000;
// Backoff entre deux tentatives : 2 s, doublé à chaque échec, plafonné (30 s
// par défaut, davantage quand le portail de secours a un client).
static const uint32_t WIFI_RETRY_BASE_MS = 2000;

// Nombre d'événements GOT_IP depuis le démarrage. Incrémenté par la tâche
// d'événements du Wi-Fi, lu par la tâche d'affichage. C'est la seule preuve
// qu'une association est NOUVELLE : juste après disconnect() + begin(),
// WiFi.status() peut encore rendre WL_CONNECTED quelques millisecondes — de
// quoi déclarer réussie une tentative qui n'a pas commencé.
static volatile uint32_t s_gotIpSeq = 0;
static void onGotIp(arduino_event_id_t, arduino_event_info_t) { s_gotIpSeq++; }

void WifiMgr::begin(const char *ssid, const char *pass) {
  strlcpy(_ssid, ssid ? ssid : "", sizeof(_ssid));
  strlcpy(_pass, pass ? pass : "", sizeof(_pass));

  WiFi.persistent(false);  // pas d'écriture NVS à chaque connexion
  WiFi.onEvent(onGotIp, ARDUINO_EVENT_WIFI_STA_GOT_IP);

  // Nom d'hôte `glowscreen-<6 hex>` (contrat v2.2) : c'est ce qui apparaît
  // dans la table DHCP du routeur. ⚠️ À poser AVANT WiFi.mode() : la pile ne
  // l'applique qu'au démarrage de l'interface station.
  snprintf(_hostname, sizeof(_hostname), "glowscreen-%s", macSuffix());
  WiFi.setHostname(_hostname);

  WiFi.mode(WIFI_STA);
  // Voir l'en-tête : un seul pilote pour les reconnexions, le nôtre.
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);  // latence plus régulière pour l'UI

  // Parmi plusieurs points d'accès du même SSID (répéteur), choisir le PLUS
  // FORT, pas le premier entendu. Le tri par signal est déjà le défaut, mais
  // il ne sert à rien avec le balayage rapide par défaut, qui s'arrête au
  // premier SSID trouvé — sur ce site, souvent le point d'accès lointain,
  // à -88 dBm, là où les `press` se perdent.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  Serial.printf("[wifi] nom d'hôte %s\n", _hostname);
  startAttempt();
}

void WifiMgr::startAttempt() {
  _attemptStartedMs = millis();
  if (_ssid[0] == '\0') {
    // Rien à essayer : le portail de secours s'ouvrira de lui-même.
    _state = WifiState::Lost;
    _retryAtMs = millis() + _retryCeilingMs;
    Serial.println("[wifi] aucun SSID configuré");
    return;
  }
  _attemptIpSeq = s_gotIpSeq;
  WiFi.disconnect(false, false);
  WiFi.begin(_ssid, _pass);
  _state = WifiState::Connecting;
  Serial.printf("[wifi] tentative de connexion a \"%s\"\n", _ssid);
}

void WifiMgr::tryCredentials(const char *ssid, const char *pass, uint32_t timeoutMs) {
  if (!ssid || ssid[0] == '\0') return;
  // Un essai déjà en cours garde la sauvegarde d'ORIGINE : la remplacer par
  // les identifiants du premier essai ferait « revenir » sur un réseau qui
  // n'a jamais marché.
  if (!_trial) {
    memcpy(_backupSsid, _ssid, sizeof(_ssid));
    memcpy(_backupPass, _pass, sizeof(_pass));
  }
  strlcpy(_ssid, ssid, sizeof(_ssid));
  strlcpy(_pass, pass ? pass : "", sizeof(_pass));
  _trial = true;
  _trialDeadlineMs = millis() + timeoutMs;
  _trialOutcome = WifiTrial::None;
  _failures = 0;
  if (_state == WifiState::Connected) _changed = true;
  Serial.printf("[wifi] essai de \"%s\" pendant %lu s\n", _ssid,
                (unsigned long)(timeoutMs / 1000));
  _trialIpSeq = s_gotIpSeq;
  startAttempt();
}

bool WifiMgr::trialAssociated() const {
  return _trial && _state == WifiState::Connected && s_gotIpSeq != _trialIpSeq &&
         WiFi.SSID() == _ssid;
}

void WifiMgr::confirmTrial() {
  if (!trialAssociated()) return;
  _trial = false;
  _trialOutcome = WifiTrial::Succeeded;
  Serial.printf("[wifi] essai de \"%s\" réussi (association + API)\n", _ssid);
}

WifiTrial WifiMgr::consumeTrialOutcome() {
  const WifiTrial t = _trialOutcome;
  _trialOutcome = WifiTrial::None;
  return t;
}

void WifiMgr::loop() {
  const uint32_t now = millis();
  const bool up = (WiFi.status() == WL_CONNECTED);

  // Essai d'identifiants : le succès vient de confirmTrial() ; ici on ne
  // fait que l'abandonner au délai.
  if (_trial) {
    if ((int32_t)(now - _trialDeadlineMs) >= 0) {
      _trial = false;
      _trialOutcome = WifiTrial::Reverted;
      memcpy(_ssid, _backupSsid, sizeof(_ssid));
      memcpy(_pass, _backupPass, sizeof(_pass));
      _failures = 0;
      Serial.printf("[wifi] essai sans liaison : retour à \"%s\"\n", _ssid);
      if (_state == WifiState::Connected) _changed = true;
      startAttempt();
      return;
    }
  }

  switch (_state) {
    case WifiState::Idle:
      break;

    case WifiState::Connecting:
      // Une IP obtenue DEPUIS le lancement de cette tentative, pas un reste
      // de la liaison qu'on vient de couper.
      if (up && s_gotIpSeq != _attemptIpSeq) {
        _state = WifiState::Connected;
        _failures = 0;
        _changed = true;
        Serial.printf("[wifi] connecte a \"%s\", IP %s (RSSI %d dBm, canal %d)\n",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                      (int)WiFi.channel());
      } else if (now - _attemptStartedMs > WIFI_ATTEMPT_TIMEOUT_MS) {
        if (_failures < 8) _failures++;
        _state = WifiState::Lost;
        uint32_t wait = WIFI_RETRY_BASE_MS << (_failures - 1);
        if (wait > _retryCeilingMs) wait = _retryCeilingMs;
        _retryAtMs = now + wait;
        Serial.printf("[wifi] echec, nouvelle tentative dans %lu ms\n",
                      (unsigned long)wait);
      }
      break;

    case WifiState::Connected:
      if (!up) {
        _state = WifiState::Lost;
        _retryAtMs = now + WIFI_RETRY_BASE_MS;
        _changed = true;
        Serial.println("[wifi] liaison perdue");
      }
      break;

    case WifiState::Lost:
      if (up) {
        // N'arrive plus guère sans reconnexion automatique, mais la pile
        // retente une fois d'elle-même après le tout premier échec.
        _state = WifiState::Connected;
        _failures = 0;
        _changed = true;
        Serial.println("[wifi] reconnecte");
      } else if ((int32_t)(now - _retryAtMs) >= 0) {
        startAttempt();
      }
      break;
  }
}

bool WifiMgr::consumeChange() {
  bool c = _changed;
  _changed = false;
  return c;
}

const char *WifiMgr::deviceId() {
  if (_deviceId[0] == '\0') {
    // MAC STATION, la même que WiFi.macAddress() : l'identité d'un écran déjà
    // déclaré dans Jeedom ne change pas avec la v2.2.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(_deviceId, sizeof(_deviceId), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  return _deviceId;
}

const char *WifiMgr::macPretty() {
  if (_macPretty[0] == '\0') {
    const char *d = deviceId();
    snprintf(_macPretty, sizeof(_macPretty), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
             d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11]);
  }
  return _macPretty;
}
