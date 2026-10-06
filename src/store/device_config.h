// Configuration locale de la carte, en NVS (contrat v2.2, « Configuration
// locale de la carte »).
//
// Identifiants Wi-Fi, hôte Jeedom, clé API et bornes de calibration tactile.
// Les valeurs de secrets.h et touch_calib.h ne sont plus que des VALEURS
// D'USINE : elles sont écrites en NVS au premier démarrage (ou après un
// effacement), puis ne servent plus.
//
// Pourquoi : changer le mot de passe du Wi-Fi imposait de décrocher et
// reflasher chaque écran en USB — l'OTA ne peut rien pour une carte qui a
// perdu le réseau. Et des bornes tactiles compilées, extrapolées d'une seule
// dalle, contredisaient « un seul binaire pour tout le parc ».
//
// Trois chemins pour les modifier sans câble : commandes `wifi` / `calibrate`
// depuis Jeedom, menu local (appui maintenu 5 s sur le bandeau), portail de
// secours (net/portal).
//
// ⚠️ Espace de noms DISTINCT du cache de mise en page (`glowscreen`) :
// LayoutStore::clear() vide son espace entier, il ne doit jamais emporter les
// identifiants Wi-Fi avec lui.
#pragma once

#include <Arduino.h>

#include "../ui/touch_calib.h"

#define CFG_SSID_LEN   33   // 32 octets (802.11) + terminateur
#define CFG_PASS_LEN   65   // 63 caractères de phrase WPA2, ou 64 hexadécimaux
#define CFG_HOST_LEN   64
#define CFG_APIKEY_LEN 100  // les clés Jeedom font 64 caractères aujourd'hui

struct DeviceConfig {
  char     ssid[CFG_SSID_LEN];
  char     pass[CFG_PASS_LEN];
  char     host[CFG_HOST_LEN];
  char     apikey[CFG_APIKEY_LEN];
  TouchCal cal;
};

class DeviceConfigStore {
 public:
  // Relit la NVS ; toute clé absente ou invalide est remplacée par sa valeur
  // d'usine, qui est alors ÉCRITE — elle ne servira plus jamais ensuite.
  void begin();

  // Copie cohérente de l'ensemble (sous verrou : le portail écrit depuis sa
  // propre tâche).
  void copy(DeviceConfig &out) const;

  // Bornes tactiles en service. Lues à chaque échantillon par la tâche
  // d'affichage, seule à les modifier : pas de verrou.
  const TouchCal &cal() const { return _cfg.cal; }

  // Écritures. Chacune met à jour la NVS PUIS la copie en RAM ; renvoie false
  // si la NVS a refusé (la copie en RAM n'est alors pas modifiée).
  bool saveWifi(const char *ssid, const char *pass);
  bool saveJeedom(const char *host, const char *apikey);
  bool saveCalib(const TouchCal &cal);

 private:
  DeviceConfig _cfg = {};
};

extern DeviceConfigStore deviceConfig;
