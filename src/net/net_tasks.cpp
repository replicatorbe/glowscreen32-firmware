#include "net_tasks.h"

#include <WiFi.h>
#include <esp_heap_caps.h>

#include "../fw_version.h"
#include "../store/layout_store.h"
#include "portal.h"

// --- Dimensionnement ---------------------------------------------------------
//
// Piles : la tâche Arduino tenait TOUT (HTTP + parsing + dessin) en 8 ko.
// La tâche réseau n'a plus le dessin, mais garde HTTPClient + WiFiClient +
// le parsing ArduinoJson ; la tâche ping, un seul appel. Les valeurs réelles
// sont mesurées et journalisées (netLogStacks) : c'est sur elles, pas sur ces
// estimations, qu'il faudra resserrer.
static const uint32_t NET_STACK = 8192;
static const uint32_t PING_STACK = 6144;

// ⚠️ CŒUR 1, À LA PRIORITÉ DE loopTask (2.3.1). Jusqu'en 2.3.0 ces tâches
// tournaient sur le cœur 0, dont la tâche IDLE est surveillée par le chien de
// garde AVEC PANIQUE (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0) : toute attente
// active de 5 s y fait redémarrer la carte. Il en reste une qu'on ne contrôle
// pas : HTTPClient::handleHeaderResponse() lit les en-têtes par
// readStringUntil(), donc Stream::timedRead(), qui tourne en boucle sans
// bloquer tant qu'une ligne d'en-tête est coupée entre deux segments TCP — le
// même mécanisme que le rst=wdt de la 2.2.1 (corrigé, lui, dans BoundedStream).
//
// Deux parades possibles : réécrire la lecture des en-têtes (un client HTTP
// maison, du code neuf sur le chemin critique de l'OTA), ou ôter au défaut sa
// conséquence. On choisit la seconde : sur le cœur 1, IDLE n'est pas
// surveillée, et à priorité ÉGALE à loopTask le temps partagé de FreeRTOS
// (tranches de 1 ms) garantit que l'affichage continue de tourner même si une
// de ces tâches boucle — au pire un écran ralenti quelques secondes, jamais
// un redémarrage, jamais un écran figé. Une priorité SUPÉRIEURE affamerait
// l'UI pendant une telle boucle ; inférieure, ce sont les tâches réseau qui
// seraient affamées par le dessin. lwIP et le pilote Wi-Fi restent sur le
// cœur 0. Le `press` passe toujours devant : la tâche net le sert avant tout
// autre travail, et le ping a sa propre tâche.
static const BaseType_t NET_CORE = 1;
static const UBaseType_t NET_PRIO = 1;   // = loopTask (CONFIG_ARDUINO_RUNNING_CORE 1, prio 1)
static const UBaseType_t PING_PRIO = 1;

// Files. Un `press` par case : on peut taper huit boutons d'affilée pendant
// un `layout` qui peine, aucun ne sera perdu.
static const UBaseType_t PRESS_QUEUE_LEN = 8;
static const UBaseType_t JOB_QUEUE_LEN = 4;
static const UBaseType_t RESULT_QUEUE_LEN = 8;

// Attente longue (contrat v2.2).
static const uint32_t LONGPOLL_RELAUNCH_MS = 200;    // écart minimal entre deux pings
static const uint32_t LONGPOLL_BACKOFF_MIN_MS = 2000;
static const uint32_t LONGPOLL_BACKOFF_MAX_MS = 60000;
// Une réponse plus rapide que cela, avec une `rev` INCHANGÉE, signifie que le
// serveur n'a pas retenu la requête alors qu'il aurait dû. Relancer à 200 ms
// reviendrait à le marteler à 5 requêtes par seconde : on se rabat sur 2 s.
static const uint32_t LONGPOLL_SUSPECT_MS = 1000;

// --- État partagé -------------------------------------------------------------

enum class NetJob : uint8_t { Layout, Firmware, OtaApply, Probe };

struct PressJob {
  uint8_t index;
  uint8_t schema;  // schéma dans lequel `id` a été reçu
  int32_t id;
};

static QueueHandle_t s_qPress = nullptr;
static QueueHandle_t s_qJob = nullptr;
static QueueHandle_t s_qResult = nullptr;
static QueueHandle_t s_qPing = nullptr;      // PingResult* rempli, vers l'UI
static QueueHandle_t s_qPingFree = nullptr;  // PingResult* rendu par l'UI
static QueueHandle_t s_qFreeLayout = nullptr;  // le tampon dont dispose la tâche réseau
static TaskHandle_t s_netTask = nullptr;
static TaskHandle_t s_pingTask = nullptr;

// Une instance de client PAR TÂCHE : aucun état partagé, aucune connexion
// partagée.
static JeedomClient s_netApi;
static JeedomClient s_pingApi;
static LayoutStore *s_store = nullptr;

// Annonce du dernier contrôle firmware. 300 octets, propriété exclusive de la
// tâche réseau : l'UI n'en reçoit que la version, pour l'écran.
static FirmwareInfo s_fw;
static bool s_fwReady = false;

// Paramètres de l'essai d'identifiants. Écrits par l'UI AVANT de déposer la
// demande, lus par la tâche réseau ensuite ; l'UI n'en dépose jamais un
// second tant que le premier n'a pas répondu.
static char s_probeHost[64], s_probeKey[100], s_probeDevice[LAYOUT_DEVICE_LEN];
static uint32_t s_probeGen = 0;

// Pilotage du ping, écrit par l'UI ET par la tâche ping : sous spinlock.
struct PingCtl {
  bool     enabled = false;
  bool     suspended = false;
  uint32_t pollSec = 30;
  bool     dimmed = false;
  uint16_t featWait = 0;
  char     rev[JEEDOM_REV_LEN] = {0};
  bool     poke = false;
  uint32_t pokeAtMs = 0;
};
static PingCtl s_ctl;
static portMUX_TYPE s_ctlMux = portMUX_INITIALIZER_UNLOCKED;

static bool longPollReady(const PingCtl &c) { return c.featWait > 0 && c.rev[0] != '\0'; }

static uint32_t v21PeriodMs(const PingCtl &c) {
  // Cadence v2.1 : `poll` écran allumé, au plus 2 x `poll` atténué. Le
  // facteur a un JUMEAU côté plugin (seuil hors ligne = 3 x 2 x poll).
  uint32_t p = (c.pollSec ? c.pollSec : 30) * 1000UL;
  if (c.dimmed) p *= 2;  // UI_IDLE_POLL_FACTOR, contrat v2.1 : jamais au-delà
  return p;
}

// --- Tâche réseau ---------------------------------------------------------------

static void postResult(const NetResult &r) { xQueueSend(s_qResult, &r, portMAX_DELAY); }

// NetResult fait ~120 octets : en statique plutôt que sur la pile de la tâche,
// qui en empile déjà un pour postResult() par appel.
static NetResult s_res;

static void doPress(const PressJob &j) {
  s_res = NetResult();
  s_res.kind = NetEvent::PressDone;
  s_res.startedMs = millis();
  s_res.index = j.index;
  s_res.id = j.id;
  s_res.r = s_netApi.press(j.id, j.schema, s_res.press);
  postResult(s_res);
}

static void drainPresses() {
  PressJob j;
  while (xQueueReceive(s_qPress, &j, 0) == pdTRUE) doPress(j);
}

static void doLayout() {
  LayoutBundle *buf = nullptr;
  if (xQueueReceive(s_qFreeLayout, &buf, pdMS_TO_TICKS(2000)) != pdTRUE || !buf) {
    // L'UI n'a pas rendu l'ancien tampon : ne peut pas arriver si elle suit
    // le protocole. On le dit plutôt que d'écrire dans un tampon affiché.
    Serial.println("[net] aucun tampon Layout libre : layout abandonné");
    s_res = NetResult();
    s_res.kind = NetEvent::LayoutDone;
    s_res.r = JeedomResult::ErrServer;
    postResult(s_res);
    return;
  }

  // Les tentatives sont enchaînées ICI et non dans le client : un appui fait
  // pendant qu'un `layout` peine passe entre deux essais, au lieu d'attendre
  // jusqu'à 15 s derrière lui.
  BannerInfo banner;
  ServerExtras extras;
  JeedomResult r = JeedomResult::ErrTransport;
  for (uint8_t attempt = 1; attempt <= JEEDOM_ATTEMPTS; attempt++) {
    r = s_netApi.fetchLayout(buf->layout, buf->values, banner, extras, 1);
    // Seuls les échecs de transport se réessaient ; une erreur applicative
    // ne se corrigera pas en 250 ms.
    if (r != JeedomResult::ErrTransport && r != JeedomResult::ErrParse) break;
    if (attempt < JEEDOM_ATTEMPTS) {
      drainPresses();
      vTaskDelay(pdMS_TO_TICKS(JEEDOM_RETRY_DELAY_MS));
      drainPresses();
    }
  }

  s_res = NetResult();
  s_res.kind = NetEvent::LayoutDone;
  s_res.r = r;
  if (r == JeedomResult::Ok) {
    // Écriture NVS ici, hors de la tâche d'affichage. `save()` ne réécrit
    // que si la STRUCTURE a changé. Ses deux tampons de travail sont
    // statiques et n'appartiennent qu'à cette tâche une fois le démarrage
    // passé (l'UI ne lit le cache qu'avant de lancer les tâches).
    s_store->save(buf->layout);  // les valeurs `view`, volatiles, ne vont PAS en NVS
    s_res.layout = buf;  // cession : l'UI rendra l'ANCIEN tampon
    s_res.banner = banner;
    s_res.extras = extras;
  } else {
    xQueueSend(s_qFreeLayout, &buf, 0);  // le tampon reste chez nous
  }
  postResult(s_res);
}

static void doFirmware() {
  s_fwReady = false;
  const JeedomResult r = s_netApi.checkFirmware(GLOWSCREEN_FW_VERSION, s_fw);
  s_res = NetResult();
  s_res.kind = NetEvent::FirmwareDone;
  s_res.r = r;
  // Preuve 5 de validation (contrat) : toute réponse JSON bien formée, y
  // compris `firmware_unavailable` ou une annonce incomplète.
  s_res.jsonOk = s_netApi.lastJsonOk();
  s_res.update = (r == JeedomResult::Ok) && s_fw.update;
  if (s_res.update) {
    strlcpy(s_res.version, s_fw.version, sizeof(s_res.version));
    s_fwReady = true;
  }
  postResult(s_res);
}

// Progression : postée SANS attendre. Si l'UI a pris du retard, un
// pourcentage sauté n'a aucune importance ; bloquer le téléchargement pour
// l'afficher en aurait une.
static void otaProgressToUi(uint8_t pct, uint32_t done, uint32_t total) {
  NetResult m;
  m.kind = NetEvent::OtaProgress;
  m.pct = pct;
  xQueueSend(s_qResult, &m, 0);
  if (pct % 10 == 0) {
    Serial.printf("[ota] %u%% (%lu/%lu octets)\n", pct, (unsigned long)done,
                  (unsigned long)total);
  }
}

static void doOta() {
  OtaOutcome outcome = OtaOutcome::ErrHttp;
  if (s_fwReady) {
    outcome = otaApply(s_fw, otaProgressToUi);
  } else {
    Serial.println("[ota] aucune annonce valide en mémoire : rien à installer");
  }
  s_fwReady = false;
  s_res = NetResult();
  s_res.kind = NetEvent::OtaDone;
  s_res.outcome = outcome;
  postResult(s_res);
}

static void doProbe() {
  // Client jetable : l'hôte et la clé essayés ne doivent pas remplacer ceux
  // des tâches tant que l'essai n'a pas réussi (et ne les remplaceront qu'au
  // redémarrage, après écriture en NVS).
  JeedomClient probe;
  probe.begin(s_probeHost, s_probeKey, s_probeDevice);
  PingParams p;  // ni `rev` ni `wait` : réponse immédiate, aucune commande livrée
  s_res = NetResult();
  s_res.kind = NetEvent::ProbeDone;
  s_res.gen = s_probeGen;
  // PingInfo fait ~1,7 ko depuis le schéma 3 : sur le tas, le temps de
  // l'essai (rare), plutôt qu'en BSS permanente ou sur la pile de la tâche.
  PingInfo *info = new (std::nothrow) PingInfo();
  s_res.r = info ? probe.ping(*info, p) : JeedomResult::ErrServer;
  delete info;
  postResult(s_res);
}

static void netTask(void *) {
  for (;;) {
    // Les appuis d'abord : c'est la seule demande qu'un humain attend.
    PressJob pj;
    if (xQueueReceive(s_qPress, &pj, 0) == pdTRUE) {
      doPress(pj);
      continue;
    }
    NetJob job;
    if (xQueueReceive(s_qJob, &job, 0) == pdTRUE) {
      switch (job) {
        case NetJob::Layout:   doLayout();   break;
        case NetJob::Firmware: doFirmware(); break;
        case NetJob::OtaApply: doOta();      break;
        case NetJob::Probe:    doProbe();    break;
      }
      continue;
    }
    // Rien à faire : on dort jusqu'à la prochaine demande (notification).
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
  }
}

// --- Tâche ping -----------------------------------------------------------------

// Deux tampons statiques (~1,8 ko chacun), jamais sur une pile : l'un se
// remplit pendant que l'UI applique l'autre.
static PingResult s_pingBufA, s_pingBufB;

static void pingTask(void *) {
  bool wasOn = false;
  bool fixed = false;        // échéance fixe (attente longue / backoff) ou cadence v2.1
  uint32_t fixedDelay = 0;
  uint32_t backoff = 0;
  uint32_t lastEnd = millis();

  for (;;) {
    PingCtl c;
    portENTER_CRITICAL(&s_ctlMux);
    c = s_ctl;
    portEXIT_CRITICAL(&s_ctlMux);

    // 1) Autorisé ? Sinon on attend sans rien émettre.
    const bool on = c.enabled && !c.suspended && WiFi.status() == WL_CONNECTED;
    if (!on) {
      wasOn = false;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
      continue;
    }
    if (!wasOn) {
      // Première échéance après (ré)activation. Attente longue : tout de
      // suite, la `rev` du `layout` fait retenir la requête. Sinon : `poll`
      // secondes, comme la v2.1 après un `layout`.
      wasOn = true;
      lastEnd = millis();
      backoff = 0;
      fixed = longPollReady(c);
      fixedDelay = 0;
    }

    // 2) Échéance, recalculée à chaque tranche : la cadence v2.1 change avec
    //    l'atténuation, et l'UI peut l'avancer (pingPoke).
    uint32_t due = lastEnd + (fixed ? fixedDelay : v21PeriodMs(c));
    if (c.poke && (int32_t)(c.pokeAtMs - due) < 0) due = c.pokeAtMs;
    const int32_t left = (int32_t)(due - millis());
    if (left > 0) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(left < 250 ? left : 250));
      continue;
    }
    portENTER_CRITICAL(&s_ctlMux);
    s_ctl.poke = false;
    portEXIT_CRITICAL(&s_ctlMux);

    // 3) Le ping. Attente longue seulement si la capacité est annoncée ET
    //    qu'on a une `rev` à présenter : sans `rev`, le serveur répond
    //    aussitôt, et relancer à 200 ms le martèlerait.
    PingParams p;
    p.rssi = (int16_t)WiFi.RSSI();
    // `rev` accompagne CHAQUE ping dès qu'on en connaît une : c'est la
    // condition pour que le plugin livre une commande à distance, attente
    // longue ou non. Sans `wait`, elle ne fait rien retenir.
    char sentRev[JEEDOM_REV_LEN] = {0};
    memcpy(sentRev, c.rev, sizeof(sentRev));
    if (sentRev[0]) p.rev = sentRev;
    if (longPollReady(c)) {
      // Contrat : la carte envoie min(features.wait, poll).
      uint32_t w = c.featWait;
      if (c.pollSec && c.pollSec < w) w = c.pollSec;
      p.wait = (uint16_t)w;
    }
    // Un tampon libre : l'UI rend le précédent après l'avoir appliqué. Si elle
    // est occupée, on attend — contre-pression naturelle, aucune commande à
    // distance ne se perd.
    PingResult *outBuf = nullptr;
    xQueueReceive(s_qPingFree, &outBuf, portMAX_DELAY);
    PingResult &out = *outBuf;
    const uint32_t t0 = millis();
    out.startedMs = t0;
    out.longPoll = p.wait > 0;
    out.r = s_pingApi.ping(out.info, p);
    const uint32_t elapsed = millis() - t0;

    // Capacités et `rev` : la réponse fait foi. Un ping SANS `features`
    // (plugin redescendu en v2.1) coupe l'attente longue.
    if (out.r == JeedomResult::Ok) {
      const ServerExtras &x = out.info.extras;
      portENTER_CRITICAL(&s_ctlMux);
      s_ctl.featWait = x.hasFeatures ? x.features.wait : 0;
      if (x.rev[0]) memcpy(s_ctl.rev, x.rev, sizeof(s_ctl.rev));
      portEXIT_CRITICAL(&s_ctlMux);
    }

    // L'UI consomme ; si elle est occupée, on attend — c'est une
    // contre-pression naturelle, et aucune commande à distance ne se perd.
    // Les décisions ci-dessous relisent le résultat : on le copie d'abord
    // dans des locales, le tampon cesse d'être à nous dès l'envoi.
    const JeedomResult pr = out.r;
    char gotRev[JEEDOM_REV_LEN];
    memcpy(gotRev, out.info.extras.rev, sizeof(gotRev));
    xQueueSend(s_qPing, &outBuf, portMAX_DELAY);
    lastEnd = millis();

    // 4) Prochaine échéance.
    if (p.wait > 0) {
      if (pr == JeedomResult::Ok) {
        backoff = 0;
        fixed = true;
        fixedDelay = LONGPOLL_RELAUNCH_MS;
        // Garde-fou sur le TEMPS seul, pas sur `rev` : une `rev` qui change à
        // chaque réponse (serveur défaillant) ferait sinon relancer à 200 ms
        // indéfiniment. Une réponse quasi immédiate n'a pas été retenue :
        // quelle qu'en soit la cause, on ne relance pas avant 2 s.
        if (elapsed < LONGPOLL_SUSPECT_MS) {
          fixedDelay = LONGPOLL_BACKOFF_MIN_MS;
          if (!strcmp(sentRev, gotRev)) {
            Serial.println("[ping] réponse immédiate à rev inchangée : relance à 2 s");
          }
        }
      } else {
        // Contrat : 2 s, doublé, plafonné à 60 s.
        backoff = backoff ? backoff * 2 : LONGPOLL_BACKOFF_MIN_MS;
        if (backoff > LONGPOLL_BACKOFF_MAX_MS) backoff = LONGPOLL_BACKOFF_MAX_MS;
        fixed = true;
        fixedDelay = backoff;
      }
    } else {
      portENTER_CRITICAL(&s_ctlMux);
      const bool ready = longPollReady(s_ctl);
      portEXIT_CRITICAL(&s_ctlMux);
      // Capacité découverte par ce ping : on bascule sans attendre `poll`.
      fixed = (pr == JeedomResult::Ok) && ready;
      fixedDelay = LONGPOLL_RELAUNCH_MS;
      backoff = 0;
    }
  }
}

// --- Interface ------------------------------------------------------------------

void netTasksBegin(LayoutStore *store, const char *host, const char *apikey,
                   const char *device, LayoutBundle *spare) {
  s_store = store;
  s_netApi.begin(host, apikey, device);
  s_pingApi.begin(host, apikey, device);

  s_qPress = xQueueCreate(PRESS_QUEUE_LEN, sizeof(PressJob));
  s_qJob = xQueueCreate(JOB_QUEUE_LEN, sizeof(NetJob));
  s_qResult = xQueueCreate(RESULT_QUEUE_LEN, sizeof(NetResult));
  s_qPing = xQueueCreate(2, sizeof(PingResult *));
  s_qPingFree = xQueueCreate(2, sizeof(PingResult *));
  PingResult *a = &s_pingBufA, *b = &s_pingBufB;
  xQueueSend(s_qPingFree, &a, 0);
  xQueueSend(s_qPingFree, &b, 0);
  s_qFreeLayout = xQueueCreate(1, sizeof(LayoutBundle *));
  xQueueSend(s_qFreeLayout, &spare, 0);

  xTaskCreatePinnedToCore(netTask, "net", NET_STACK, nullptr, NET_PRIO, &s_netTask, NET_CORE);
  xTaskCreatePinnedToCore(pingTask, "ping", PING_STACK, nullptr, PING_PRIO, &s_pingTask,
                          NET_CORE);
  Serial.printf("[net] tâches lancées (piles %lu + %lu o, files %u o), heap %u o\n",
                (unsigned long)NET_STACK, (unsigned long)PING_STACK,
                (unsigned)(PRESS_QUEUE_LEN * sizeof(PressJob) +
                           RESULT_QUEUE_LEN * sizeof(NetResult) + 4 * sizeof(void *)),
                (unsigned)ESP.getFreeHeap());
}

static bool pushJob(NetJob j) {
  if (xQueueSend(s_qJob, &j, 0) != pdTRUE) return false;
  xTaskNotifyGive(s_netTask);
  return true;
}

bool netRequestLayout() { return pushJob(NetJob::Layout); }

bool netRequestProbe(const char *host, const char *apikey, const char *device, uint32_t gen) {
  s_probeGen = gen;
  strlcpy(s_probeHost, host ? host : "", sizeof(s_probeHost));
  strlcpy(s_probeKey, apikey ? apikey : "", sizeof(s_probeKey));
  strlcpy(s_probeDevice, device ? device : "", sizeof(s_probeDevice));
  return pushJob(NetJob::Probe);
}
bool netRequestFirmware() { return pushJob(NetJob::Firmware); }
bool netRequestOtaApply() { return pushJob(NetJob::OtaApply); }

bool netRequestPress(uint8_t index, int32_t id, uint8_t schema) {
  const PressJob j = {index, schema, id};
  if (xQueueSend(s_qPress, &j, 0) != pdTRUE) return false;
  xTaskNotifyGive(s_netTask);
  return true;
}

void netReturnLayout(LayoutBundle *buf) {
  if (buf) xQueueSend(s_qFreeLayout, &buf, 0);
}

bool netPollResult(NetResult &out) { return xQueueReceive(s_qResult, &out, 0) == pdTRUE; }

PingResult *pingPollResult() {
  PingResult *r = nullptr;
  return xQueueReceive(s_qPing, &r, 0) == pdTRUE ? r : nullptr;
}

void pingReturnResult(PingResult *r) {
  if (r) xQueueSend(s_qPingFree, &r, 0);
}

void pingConfigure(bool enabled, uint32_t pollSec, bool dimmed) {
  bool wake = false;
  portENTER_CRITICAL(&s_ctlMux);
  wake = (enabled && !s_ctl.enabled) || (s_ctl.dimmed != dimmed);
  s_ctl.enabled = enabled;
  s_ctl.pollSec = pollSec;
  s_ctl.dimmed = dimmed;
  portEXIT_CRITICAL(&s_ctlMux);
  if (wake && s_pingTask) xTaskNotifyGive(s_pingTask);
}

void pingAdoptExtras(const ServerExtras &x) {
  portENTER_CRITICAL(&s_ctlMux);
  s_ctl.featWait = x.hasFeatures ? x.features.wait : 0;
  if (x.rev[0]) memcpy(s_ctl.rev, x.rev, sizeof(s_ctl.rev));
  portEXIT_CRITICAL(&s_ctlMux);
}

void pingSuspend(bool suspended) {
  portENTER_CRITICAL(&s_ctlMux);
  s_ctl.suspended = suspended;
  portEXIT_CRITICAL(&s_ctlMux);
  if (s_pingTask) xTaskNotifyGive(s_pingTask);
}

void pingPoke(uint32_t delayMs) {
  const uint32_t at = millis() + delayMs;
  portENTER_CRITICAL(&s_ctlMux);
  if (!s_ctl.poke || (int32_t)(at - s_ctl.pokeAtMs) < 0) {
    s_ctl.poke = true;
    s_ctl.pokeAtMs = at;
  }
  portEXIT_CRITICAL(&s_ctlMux);
  if (s_pingTask) xTaskNotifyGive(s_pingTask);
}

bool pingLongPollActive() {
  portENTER_CRITICAL(&s_ctlMux);
  const bool r = longPollReady(s_ctl);
  portEXIT_CRITICAL(&s_ctlMux);
  return r;
}

void netLogStacks() {
  // Sur ESP32, uxTaskGetStackHighWaterMark() compte en OCTETS (StackType_t
  // y vaut uint8_t) : c'est la marge jamais entamée depuis le démarrage.
  // Portail : marge relevée PAR SA PROPRE TÂCHE (portalStackMargin). Lire son
  // handle ici et l'interroger serait une course : la tâche peut se détruire
  // entre les deux, et uxTaskGetStackHighWaterMark lirait un TCB libéré.
  const uint32_t portal = portalStackMargin();
  Serial.printf("[pile] marge min : ui %u o, net %u/%lu o, ping %u/%lu o",
                (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                s_netTask ? (unsigned)uxTaskGetStackHighWaterMark(s_netTask) : 0,
                (unsigned long)NET_STACK,
                s_pingTask ? (unsigned)uxTaskGetStackHighWaterMark(s_pingTask) : 0,
                (unsigned long)PING_STACK);
  if (portal) {
    Serial.printf(", portail %u o", (unsigned)portal);
  }
  Serial.printf(" | heap %u o (min %u), bloc %u o\n", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
