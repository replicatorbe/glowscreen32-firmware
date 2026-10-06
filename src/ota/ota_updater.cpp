#include "ota_updater.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <mbedtls/md.h>
#include <sdkconfig.h>

// Le retour arrière automatique dépend du BOOTLOADER, pas de notre code : il
// doit être compilé avec CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE. Le bootloader
// précompilé d'arduino-esp32 l'active (tools/sdk/esp32/sdkconfig). Ce garde-fou
// prévient si une future version du framework le désactivait : le firmware
// continuerait de fonctionner, mais SANS filet — c'est exactement le genre de
// régression qu'on ne découvre que le jour où on en a besoin.
#ifndef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#warning "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE absent : pas de retour arriere automatique apres OTA !"
#endif

// Timeouts du téléchargement : plus généreux que les appels d'API, le binaire
// fait ~1 Mo et l'écriture en flash entrecoupe la lecture du flux.
static const uint16_t OTA_HTTP_TIMEOUT_MS = 10000;
// Au-delà de ce silence, on considère le flux mort et on abandonne.
static const uint32_t OTA_STALL_TIMEOUT_MS = 15000;

// Tampon de transfert. Volontairement en BSS et non sur la pile : la tâche
// réseau ne dispose que de 8 ko et la carte n'a pas de PSRAM. 1 ko fixe,
// prévisible, plutôt qu'une allocation dont l'échec surviendrait au pire
// moment (pendant une mise à jour).
static uint8_t otaBuffer[1024];

// ⚠️ Ces libellés sont AFFICHÉS À L'ÉCRAN (drawOtaFailure), pas seulement
// journalisés : ils portent donc les accents. La translittération datait de la
// v1.4, où les polices bitmap de TFT_eSPI ne couvraient pas le Latin-1 ; les
// polices VLW embarquées depuis la v2.0 portent tout le répertoire français.
// Sans cela on affichait « image refusee » juste sous « Mise à jour
// interrompue », ce qui se remarque.
const char *otaOutcomeLabel(OtaOutcome o) {
  switch (o) {
    case OtaOutcome::Success:     return "OK";
    case OtaOutcome::ErrHttp:     return "téléchargement";
    case OtaOutcome::ErrSize:     return "taille";
    case OtaOutcome::ErrBegin:    return "partition";
    case OtaOutcome::ErrWrite:    return "écriture flash";
    case OtaOutcome::ErrTruncated:return "flux interrompu";
    case OtaOutcome::ErrSha256:   return "empreinte SHA-256";
    case OtaOutcome::ErrFinalize: return "image refusée";
  }
  return "?";
}

// Compare une empreinte binaire à sa représentation hexadécimale, sans tenir
// compte de la casse.
static bool digestMatches(const uint8_t digest[32], const char *expectedHex) {
  char computed[65];
  for (uint8_t i = 0; i < 32; i++) snprintf(computed + i * 2, 3, "%02x", digest[i]);
  computed[64] = '\0';
  bool same = (strcasecmp(computed, expectedHex) == 0);
  if (!same) {
    Serial.printf("[ota] SHA-256 calcule  %s\n", computed);
    Serial.printf("[ota] SHA-256 attendu  %s\n", expectedHex);
  }
  return same;
}

OtaOutcome otaApply(const FirmwareInfo &info, OtaProgressCb onProgress) {
  Serial.printf("[ota] telechargement de %s\n", info.url);

  HTTPClient http;
  http.setConnectTimeout(OTA_HTTP_TIMEOUT_MS);
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);
  http.setReuse(false);
  http.useHTTP10(true);  // corps brut, Content-Length obligatoire (contrat)
  if (!http.begin(info.url)) return OtaOutcome::ErrHttp;

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[ota] HTTP %d sur le binaire\n", code);
    http.end();
    return OtaOutcome::ErrHttp;
  }

  const int contentLength = http.getSize();
  // La taille annoncée par le contrat fait foi : un écart signale un fichier
  // tronqué ou un mauvais binaire, inutile d'effacer la partition pour rien.
  if (info.size > 0 && contentLength > 0 && (uint32_t)contentLength != info.size) {
    Serial.printf("[ota] taille %d != %lu annoncee\n", contentLength,
                  (unsigned long)info.size);
    http.end();
    return OtaOutcome::ErrSize;
  }
  const uint32_t total = (contentLength > 0) ? (uint32_t)contentLength : info.size;
  if (total == 0) {
    http.end();
    return OtaOutcome::ErrSize;
  }

  // Ouvre la partition INACTIVE. Le firmware en cours n'est jamais touché.
  if (!Update.begin(total)) {
    Serial.printf("[ota] Update.begin refuse : %s\n", Update.errorString());
    http.end();
    return OtaOutcome::ErrBegin;
  }

  mbedtls_md_context_t md;
  mbedtls_md_init(&md);
  // API générique : stable entre mbedtls 2.x et 3.x, contrairement aux
  // fonctions mbedtls_sha256_*_ret() qui ont disparu.
  mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&md);

  WiFiClient *stream = http.getStreamPtr();
  uint32_t written = 0;
  uint32_t lastDataMs = millis();
  uint8_t lastPct = 255;
  OtaOutcome result = OtaOutcome::Success;

  while (written < total) {
    const size_t avail = stream->available();
    if (avail == 0) {
      if (!http.connected() && stream->available() == 0) {
        result = OtaOutcome::ErrTruncated;
        break;
      }
      if (millis() - lastDataMs > OTA_STALL_TIMEOUT_MS) {
        Serial.println("[ota] flux muet, abandon");
        result = OtaOutcome::ErrTruncated;
        break;
      }
      delay(2);  // rend la main aux tâches Wi-Fi et au watchdog
      continue;
    }

    size_t toRead = avail < sizeof(otaBuffer) ? avail : sizeof(otaBuffer);
    if (toRead > total - written) toRead = total - written;
    const int n = stream->readBytes(otaBuffer, toRead);
    if (n <= 0) {
      delay(2);
      continue;
    }

    mbedtls_md_update(&md, otaBuffer, (size_t)n);
    if (Update.write(otaBuffer, (size_t)n) != (size_t)n) {
      Serial.printf("[ota] ecriture flash en echec : %s\n", Update.errorString());
      result = OtaOutcome::ErrWrite;
      break;
    }
    written += (uint32_t)n;
    lastDataMs = millis();
    // ⚠️ Depuis la v2.2 ce téléchargement tourne dans la tâche réseau, sur le
    // CŒUR 0, dont la tâche IDLE est surveillée par le chien de garde AVEC
    // panique. Quand le flux débite sans trou, la boucle enchaîne lecture et
    // écriture flash sans jamais bloquer : IDLE ne tournerait plus, et la
    // carte redémarrerait au bout de 5 s, en pleine écriture. Une milliseconde
    // par bloc de 1 ko coûte ~1 s sur un binaire de 1,2 Mo.
    vTaskDelay(1);

    const uint8_t pct = (uint8_t)((written * 100ULL) / total);
    if (onProgress && pct != lastPct) {
      lastPct = pct;
      onProgress(pct, written, total);
    }
  }

  uint8_t digest[32];
  mbedtls_md_finish(&md, digest);
  mbedtls_md_free(&md);
  http.end();

  if (result == OtaOutcome::Success && written != total) {
    result = OtaOutcome::ErrTruncated;
  }

  // ⚠️ La vérification a lieu AVANT Update.end() : c'est end() qui bascule la
  // partition d'amorçage. Une empreinte fausse ne doit jamais aller plus loin.
  if (result == OtaOutcome::Success && !digestMatches(digest, info.sha256)) {
    result = OtaOutcome::ErrSha256;
  }

  if (result != OtaOutcome::Success) {
    Update.abort();  // la partition inactive reste inutilisable, sans effet
    Serial.printf("[ota] echec (%s) apres %lu/%lu octets -- on reste sur %s\n",
                  otaOutcomeLabel(result), (unsigned long)written,
                  (unsigned long)total, otaRunningPartition());
    return result;
  }

  if (!Update.end(true)) {
    Serial.printf("[ota] Update.end refuse : %s\n", Update.errorString());
    return OtaOutcome::ErrFinalize;
  }
  if (!Update.isFinished()) return OtaOutcome::ErrFinalize;

  // Version VISÉE notée MAINTENANT, et pas avant le téléchargement : seule une
  // image écrite, vérifiée et rendue amorçable peut être « rejetée » par un
  // retour arrière. Notée plus tôt, un téléchargement coupé puis une coupure
  // de courant l'auraient fait passer pour un rejet. Si la NVS refuse, on
  // RENONCE : on remet l'amorçage sur la partition courante plutôt que
  // d'installer une version dont un rejet ne serait pas mémorisé.
  if (!otaJournalSetTarget(info.version)) {
    Serial.println("[ota] NVS indisponible : installation annulée (rejet non mémorisable)");
    esp_ota_set_boot_partition(esp_ota_get_running_partition());
    return OtaOutcome::ErrFinalize;
  }

  Serial.printf("[ota] %lu octets verifies, bascule sur la partition inactive\n",
                (unsigned long)written);
  return OtaOutcome::Success;
}

// --- Filet de sécurité -----------------------------------------------------

// ⚠️ SANS CETTE FONCTION, LE RETOUR ARRIÈRE N'EXISTE PAS.
//
// Le cœur Arduino (esp32-hal-misc.c, initArduino) appelle verifyRollbackLater()
// AVANT setup() : si elle renvoie false — la version faible par défaut —, il
// déclare lui-même l'image saine dès le démarrage. otaPendingVerify() renvoyait
// alors toujours false et tout le filet décrit dans docs/ota.md était inopérant,
// sans le moindre symptôme : l'OTA 1.4.0 -> 1.4.1 a réussi sans jamais
// l'éprouver. Renvoyer true laisse la décision à updateFirmwareVerification().
extern "C" bool verifyRollbackLater() { return true; }

bool otaPendingVerify() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!running) return false;
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(running, &st) != ESP_OK) return false;
  return st == ESP_OTA_IMG_PENDING_VERIFY;
}

void otaMarkValid() {
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    Serial.printf("[ota] firmware declare sain sur %s : retour arriere annule\n",
                  otaRunningPartition());
  } else {
    Serial.printf("[ota] esp_ota_mark_app_valid a echoue (%d)\n", (int)err);
  }
}

void otaRollback() {
  Serial.println("[ota] validation impossible : retour a la version precedente");
  Serial.flush();
  // Marque l'image invalide et redémarre sur la partition précédente.
  esp_ota_mark_app_invalid_rollback_and_reboot();
  // On n'arrive ici que s'il n'existe aucune partition de repli exploitable.
  Serial.println("[ota] aucune partition de repli : simple redemarrage");
  Serial.flush();
  ESP.restart();
}

// --- Mémoire des versions rejetées --------------------------------------------

static const char *OTA_NVS_NS = "glowota";
static const char *OTA_KEY_TARGET = "target";
static const char *OTA_KEY_REJECTED = "rejected";

static void journalRead(Preferences &p, const char *key, char *out, size_t len) {
  out[0] = '\0';
  if (p.isKey(key) && p.getString(key, out, len) == 0) out[0] = '\0';
}

void otaJournalBoot(const char *current) {
  Preferences p;
  if (!p.begin(OTA_NVS_NS, false)) return;
  char target[16], rejected[16];
  journalRead(p, OTA_KEY_TARGET, target, sizeof(target));
  journalRead(p, OTA_KEY_REJECTED, rejected, sizeof(rejected));
  if (target[0] && strcmp(target, current) != 0) {
    if (esp_ota_get_last_invalid_partition() != nullptr) {
      p.putString(OTA_KEY_REJECTED, target);
      Serial.printf("[ota] RETOUR ARRIÈRE constaté : %s a été rejetée, elle ne sera plus "
                    "installée (toujours en %s)\n", target, current);
    } else {
      Serial.printf("[ota] installation de %s interrompue, toujours en %s\n", target, current);
    }
    p.remove(OTA_KEY_TARGET);
  } else if (rejected[0]) {
    Serial.printf("[ota] version rejetée mémorisée : %s\n", rejected);
  }
  p.end();
}

bool otaJournalSetTarget(const char *version) {
  Preferences p;
  if (!p.begin(OTA_NVS_NS, false)) return false;
  const bool ok = p.putString(OTA_KEY_TARGET, version) > 0;
  p.end();
  return ok;
}

void otaJournalClearTarget() {
  Preferences p;
  if (!p.begin(OTA_NVS_NS, false)) return;
  if (p.isKey(OTA_KEY_TARGET)) p.remove(OTA_KEY_TARGET);
  p.end();
}

bool otaJournalAllows(const char *version) {
  Preferences p;
  if (!p.begin(OTA_NVS_NS, false)) return true;
  char rejected[16];
  journalRead(p, OTA_KEY_REJECTED, rejected, sizeof(rejected));
  bool allowed = true;
  if (rejected[0]) {
    if (strcmp(rejected, version) == 0) {
      Serial.printf("[ota] %s proposée mais déjà REJETÉE par un retour arrière : pas "
                    "d'installation (publier une autre version)\n", version);
      allowed = false;
    } else {
      Serial.printf("[ota] nouvelle version %s : oubli de la version rejetée %s\n", version,
                    rejected);
      p.remove(OTA_KEY_REJECTED);
    }
  }
  p.end();
  return allowed;
}

const char *otaRunningPartition() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  return running ? running->label : "?";
}
