#include "device_config.h"

#include <Preferences.h>

#include "../model/layout.h"
#include "../secrets.h"

DeviceConfigStore deviceConfig;

static const char *CFG_NAMESPACE = "glowcfg";
// SSID et mot de passe dans UN SEUL blob : deux clés séparées pouvaient, sur
// une coupure entre les deux écritures, laisser un SSID neuf avec l'ancien
// mot de passe — une carte qui ne rejoint plus aucun réseau. Une écriture NVS
// d'une clé est atomique : le blob change entièrement, ou pas du tout.
static const char *KEY_WIFI = "wifi";
struct WifiBlob {
  char ssid[CFG_SSID_LEN];
  char pass[CFG_PASS_LEN];
};
// Hôte et clé : UN blob depuis la 2.3.1, pour la même raison que le Wi-Fi —
// un hôte neuf avec l'ancienne clé enverrait celle-ci à un autre serveur.
// Les clés séparées `host`/`apikey` de la 2.2.0-2.3.0 sont migrées au
// démarrage, puis effacées.
static const char *KEY_JEEDOM = "jeedom";
static const char *KEY_HOST_V22 = "host";
static const char *KEY_APIKEY_V22 = "apikey";
struct JeedomBlob {
  char host[CFG_HOST_LEN];
  char apikey[CFG_APIKEY_LEN];
};
static const char *KEY_CAL = "cal";

// L'UI enregistre pendant que les tâches réseau ou le portail lisent (copy()) :
// la copie en RAM est donc protégée. Un spinlock suffit, on n'y fait que des
// memcpy de quelques centaines d'octets — jamais d'accès NVS sous verrou.
static portMUX_TYPE s_cfgMux = portMUX_INITIALIZER_UNLOCKED;

void DeviceConfigStore::begin() {
  Preferences p;
  if (!p.begin(CFG_NAMESPACE, false)) {
    // NVS inutilisable : on tourne sur les valeurs d'usine, sans rien écrire.
    // L'écran reste fonctionnel ; seule la persistance des réglages manque.
    Serial.println("[cfg] NVS inaccessible : valeurs d'usine en mémoire seulement");
    layoutCopyUtf8(_cfg.ssid, sizeof(_cfg.ssid), WIFI_SSID);
    layoutCopyUtf8(_cfg.pass, sizeof(_cfg.pass), WIFI_PASS);
    layoutCopyUtf8(_cfg.host, sizeof(_cfg.host), JEEDOM_HOST);
    layoutCopyUtf8(_cfg.apikey, sizeof(_cfg.apikey), JEEDOM_APIKEY);
    _cfg.cal = TOUCH_CAL_FACTORY;
    return;
  }

  uint8_t seeded = 0;
  WifiBlob w = {};
  if (p.getBytesLength(KEY_WIFI) == sizeof(w) && p.getBytes(KEY_WIFI, &w, sizeof(w)) == sizeof(w)) {
    w.ssid[sizeof(w.ssid) - 1] = '\0';
    w.pass[sizeof(w.pass) - 1] = '\0';
  } else {
    if (p.isKey(KEY_WIFI)) Serial.println("[cfg] identifiants Wi-Fi illisibles : usine");
    layoutCopyUtf8(w.ssid, sizeof(w.ssid), WIFI_SSID);
    layoutCopyUtf8(w.pass, sizeof(w.pass), WIFI_PASS);
    p.putBytes(KEY_WIFI, &w, sizeof(w));
    seeded++;
  }
  memcpy(_cfg.ssid, w.ssid, sizeof(w.ssid));
  memcpy(_cfg.pass, w.pass, sizeof(w.pass));
  memset(&w, 0, sizeof(w));
  JeedomBlob j = {};
  if (p.getBytesLength(KEY_JEEDOM) == sizeof(j) &&
      p.getBytes(KEY_JEEDOM, &j, sizeof(j)) == sizeof(j)) {
    j.host[sizeof(j.host) - 1] = '\0';
    j.apikey[sizeof(j.apikey) - 1] = '\0';
  } else {
    // Migration 2.2.0/2.3.0 (clés séparées), sinon valeurs d'usine.
    const bool legacy = p.isKey(KEY_HOST_V22) && p.isKey(KEY_APIKEY_V22);
    if (!legacy || p.getString(KEY_HOST_V22, j.host, sizeof(j.host)) == 0 ||
        p.getString(KEY_APIKEY_V22, j.apikey, sizeof(j.apikey)) == 0) {
      memset(&j, 0, sizeof(j));
      layoutCopyUtf8(j.host, sizeof(j.host), JEEDOM_HOST);
      layoutCopyUtf8(j.apikey, sizeof(j.apikey), JEEDOM_APIKEY);
      seeded++;
    } else {
      Serial.println("[cfg] hôte et clé migrés en un seul enregistrement");
    }
    if (p.putBytes(KEY_JEEDOM, &j, sizeof(j)) == sizeof(j)) {
      if (p.isKey(KEY_HOST_V22)) p.remove(KEY_HOST_V22);
      if (p.isKey(KEY_APIKEY_V22)) p.remove(KEY_APIKEY_V22);
    }
  }
  memcpy(_cfg.host, j.host, sizeof(j.host));
  memcpy(_cfg.apikey, j.apikey, sizeof(j.apikey));
  memset(&j, 0, sizeof(j));

  // Bornes tactiles : revérifiées à la LECTURE. Un blob corrompu ou une
  // calibration aberrante rendrait l'écran intouchable, donc impossible à
  // recalibrer sans câble — on retombe sur l'usine plutôt.
  TouchCal cal;
  if (p.getBytesLength(KEY_CAL) == sizeof(cal) &&
      p.getBytes(KEY_CAL, &cal, sizeof(cal)) == sizeof(cal) && touchCalPlausible(cal)) {
    _cfg.cal = cal;
  } else {
    if (p.isKey(KEY_CAL)) Serial.println("[cfg] calibration en NVS invalide : usine");
    _cfg.cal = TOUCH_CAL_FACTORY;
    p.putBytes(KEY_CAL, &_cfg.cal, sizeof(_cfg.cal));
    seeded++;
  }
  p.end();

  // Ni le mot de passe ni la clé ne partent sur le port série.
  Serial.printf("[cfg] Wi-Fi \"%s\", Jeedom %s, tactile X %d..%d Y %d..%d%s\n", _cfg.ssid,
                _cfg.host, _cfg.cal.xMin, _cfg.cal.xMax, _cfg.cal.yMin, _cfg.cal.yMax,
                seeded ? " (valeurs d'usine écrites)" : "");
}

void DeviceConfigStore::copy(DeviceConfig &out) const {
  portENTER_CRITICAL(&s_cfgMux);
  memcpy(&out, &_cfg, sizeof(out));
  portEXIT_CRITICAL(&s_cfgMux);
}

bool DeviceConfigStore::saveWifi(const char *ssid, const char *pass) {
  WifiBlob w = {};
  layoutCopyUtf8(w.ssid, sizeof(w.ssid), ssid);
  layoutCopyUtf8(w.pass, sizeof(w.pass), pass);
  if (w.ssid[0] == '\0') return false;

  Preferences p;
  if (!p.begin(CFG_NAMESPACE, false)) return false;
  const bool ok = p.putBytes(KEY_WIFI, &w, sizeof(w)) == sizeof(w);
  p.end();
  if (!ok) return false;

  portENTER_CRITICAL(&s_cfgMux);
  memcpy(_cfg.ssid, w.ssid, sizeof(w.ssid));
  memcpy(_cfg.pass, w.pass, sizeof(w.pass));
  portEXIT_CRITICAL(&s_cfgMux);
  Serial.printf("[cfg] identifiants Wi-Fi enregistrés pour \"%s\"\n", w.ssid);
  memset(&w, 0, sizeof(w));
  return true;
}

bool DeviceConfigStore::saveJeedom(const char *host, const char *apikey) {
  JeedomBlob j = {};
  layoutCopyUtf8(j.host, sizeof(j.host), host);
  layoutCopyUtf8(j.apikey, sizeof(j.apikey), apikey);
  if (j.host[0] == '\0' || j.apikey[0] == '\0') return false;

  Preferences p;
  if (!p.begin(CFG_NAMESPACE, false)) return false;
  const bool ok = p.putBytes(KEY_JEEDOM, &j, sizeof(j)) == sizeof(j);
  p.end();
  if (!ok) {
    memset(&j, 0, sizeof(j));
    return false;
  }

  portENTER_CRITICAL(&s_cfgMux);
  memcpy(_cfg.host, j.host, sizeof(j.host));
  memcpy(_cfg.apikey, j.apikey, sizeof(j.apikey));
  portEXIT_CRITICAL(&s_cfgMux);
  Serial.printf("[cfg] hôte Jeedom et clé enregistrés ensemble : %s\n", j.host);
  memset(&j, 0, sizeof(j));
  return true;
}

bool DeviceConfigStore::saveCalib(const TouchCal &cal) {
  if (!touchCalPlausible(cal)) return false;
  Preferences p;
  if (!p.begin(CFG_NAMESPACE, false)) return false;
  const bool ok = p.putBytes(KEY_CAL, &cal, sizeof(cal)) == sizeof(cal);
  p.end();
  if (!ok) return false;
  _cfg.cal = cal;  // seule la tâche d'affichage lit et écrit la calibration
  Serial.printf("[cfg] calibration enregistrée : X %d..%d Y %d..%d\n", cal.xMin, cal.xMax,
                cal.yMin, cal.yMax);
  return true;
}
