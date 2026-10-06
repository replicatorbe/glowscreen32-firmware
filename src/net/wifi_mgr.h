// Gestion Wi-Fi non bloquante : la connexion et les reconnexions sont pilotées
// par des appels répétés à loop(), jamais par une attente active.
//
// ⚠️ C'est le SEUL module qui décide quand (re)lancer une association. La
// reconnexion automatique de la pile Arduino (`setAutoReconnect`) est coupée
// depuis la v2.2 : jusqu'en v2.1 les deux mécanismes coexistaient, et le
// `disconnect()` + `begin()` de startAttempt() provoquait un événement de
// déconnexion auquel la pile répondait par son propre `begin()` — deux
// associations lancées en même temps, l'une annulant l'autre.
#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include "../store/device_config.h"

enum class WifiState : uint8_t {
  Idle,        // pas encore démarré
  Connecting,  // tentative en cours
  Connected,   // associé, IP obtenue
  Lost,        // déconnecté, attente avant nouvelle tentative
};

// Issue d'un essai d'identifiants (commande `wifi`, contrat v2.2).
enum class WifiTrial : int8_t {
  None = 0,
  Succeeded = 1,  // associé ET l'API a répondu (confirmTrial) : à écrire en NVS
  Reverted = -1,  // pas de succès sous le délai : anciens identifiants rétablis
};

class WifiMgr {
 public:
  // Mémorise les identifiants, règle nom d'hôte et stratégie de balayage, et
  // lance la première tentative.
  void begin(const char *ssid, const char *pass);

  // À appeler à chaque tour de boucle. Ne bloque jamais.
  void loop();

  WifiState state() const { return _state; }
  bool connected() const { return _state == WifiState::Connected; }

  // Vrai une seule fois après chaque transition Connecté <-> déconnecté,
  // pour que l'appelant redessine le bandeau ou relance une requête.
  bool consumeChange();

  IPAddress ip() const { return WiFi.localIP(); }
  int rssi() const { return WiFi.RSSI(); }

  // Adresse MAC normalisée « 246f28123456 » : identifiant de l'écran côté
  // Jeedom. C'est la SEULE source d'identité de la carte — rien n'est compilé
  // en dur, le même binaire tourne sur toutes les cartes du parc.
  // Lue dans l'eFuse (MAC station) : disponible avant même d'allumer le Wi-Fi,
  // ce qu'exige le nom d'hôte, qui doit être posé AVANT WiFi.mode().
  const char *deviceId();

  // Même adresse en forme lisible « 24:6f:28:12:34:56 », pour l'affichage.
  const char *macPretty();

  // Les 6 derniers caractères de la MAC (« 123456 ») : suffixe du nom d'hôte
  // et du point d'accès de secours.
  const char *macSuffix() { return deviceId() + 6; }

  // SSID visé (pas forcément associé), pour les écrans d'information.
  const char *targetSsid() const { return _ssid; }

  // Commande `wifi` / portail : essaie `ssid`/`pass` pendant `timeoutMs`.
  // Le succès se décide en DEUX temps :
  //   1. trialAssociated() : une IP obtenue APRÈS le début de l'essai
  //      (événement GOT_IP, pas WiFi.status(), qui peut rester à CONNECTED
  //      quelques ms après le disconnect()), sur le SSID visé ;
  //   2. confirmTrial() : appelée par l'UI quand l'API a répondu à une
  //      requête émise après cette association.
  // Sans les deux avant le délai : retour aux identifiants précédents,
  // WifiTrial::Reverted, et l'appelant n'écrit RIEN.
  void tryCredentials(const char *ssid, const char *pass, uint32_t timeoutMs);
  bool trialRunning() const { return _trial; }
  bool trialAssociated() const;
  void confirmTrial();
  WifiTrial consumeTrialOutcome();
  const char *trialPass() const { return _pass; }

  // Plafond du backoff entre deux tentatives. Relevé pendant que le portail
  // de secours a un client : chaque tentative balaie tous les canaux, et le
  // point d'accès, qui partage la radio, décroche pendant ce temps.
  void setRetryCeiling(uint32_t ms) { _retryCeilingMs = ms; }

 private:
  void startAttempt();
  uint32_t _attemptIpSeq = 0;  // compteur GOT_IP au lancement de la tentative
  uint32_t _trialIpSeq = 0;    // idem au début de l'essai

  char _ssid[CFG_SSID_LEN] = {0};
  char _pass[CFG_PASS_LEN] = {0};
  char _backupSsid[CFG_SSID_LEN] = {0};
  char _backupPass[CFG_PASS_LEN] = {0};
  bool _trial = false;
  uint32_t _trialDeadlineMs = 0;
  WifiTrial _trialOutcome = WifiTrial::None;

  WifiState _state = WifiState::Idle;
  bool _changed = false;
  uint32_t _attemptStartedMs = 0;
  uint32_t _retryAtMs = 0;
  uint32_t _retryCeilingMs = 30000;
  uint8_t _failures = 0;
  char _deviceId[13] = {0};
  char _macPretty[18] = {0};
  char _hostname[24] = {0};
};
