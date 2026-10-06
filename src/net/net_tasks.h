// Réseau hors de la tâche d'affichage (contrat v2.2, règle firmware).
//
// Jusqu'en v2.1, chaque appel HTTP s'exécutait dans la tâche qui lit le
// tactile : un `ping` lent figeait l'écran jusqu'à 10 s, un `layout` en échec
// jusqu'à 30 s, et TOUT APPUI FAIT PENDANT CE TEMPS ÉTAIT PERDU — y compris le
// premier appui après le réveil, puisque le `ping` de réveil partait dans le
// même tour de boucle.
//
// Depuis la v2.2, trois tâches :
//
//   loopTask (Arduino, cœur 1)   tactile, dessin, machine à états. N'émet
//                                AUCUN appel HTTP : elle dépose des demandes
//                                et applique les résultats.
//   "net"    (cœur 0, prio 2)    layout, press, contrôle firmware, OTA.
//                                Les `press` passent AVANT tout le reste, et
//                                entre deux tentatives d'un `layout`.
//   "ping"   (cœur 0, prio 1)    le `ping`, y compris retenu 25 s en attente
//                                longue, avec SA propre instance HTTPClient :
//                                un `press` n'attend jamais un ping retenu.
//
// Seule la tâche d'affichage touche à l'écran (SPI) et au `Layout` affiché.
// La mise en page reçue est parsée dans un tampon appartenant à la tâche
// réseau, puis transmise par ÉCHANGE DE PROPRIÉTÉ (double tampon) : l'UI rend
// l'ancien tampon en adoptant le nouveau. Aucun `Layout` n'est jamais partagé
// en écriture, et aucun n'est copié.
#pragma once

#include <Arduino.h>

#include "../model/layout.h"
#include "../ota/ota_updater.h"
#include "jeedom_client.h"

class LayoutStore;

// --- Résultats de la tâche réseau -------------------------------------------

enum class NetEvent : uint8_t {
  LayoutDone,    // `layout` ; si r == Ok, `layout` appartient désormais à l'UI
  PressDone,     // `press`
  FirmwareDone,  // contrôle `firmware`
  OtaProgress,   // progression du téléchargement (peut être sautée si la file est pleine)
  OtaDone,       // fin de l'OTA ; en cas de succès l'UI redémarre
  ProbeDone,     // essai d'identifiants : réponse de l'API avec l'hôte et la clé essayés
};

struct NetResult {
  NetEvent     kind = NetEvent::LayoutDone;
  JeedomResult r = JeedomResult::ErrTransport;
  // LayoutDone : mise en page + valeurs des tuiles `view`
  LayoutBundle *layout = nullptr;
  BannerInfo   banner;
  ServerExtras extras;
  // PressDone
  uint8_t      index = 0;   // rang de stockage du bouton au moment de l'appui
  int32_t      id = 0;      // `id` opaque envoyé — sert à vérifier le rang
  PressInfo    press;
  uint32_t     startedMs = 0;  // millis() à l'émission du `press`
  // FirmwareDone : la réponse était du JSON bien formé (preuve 5)
  bool         jsonOk = false;
  // ProbeDone : génération de l'essai d'identifiants qui l'a demandée
  uint32_t     gen = 0;
  // FirmwareDone
  bool         update = false;
  char         version[16] = {0};
  // OtaProgress / OtaDone
  uint8_t      pct = 0;
  OtaOutcome   outcome = OtaOutcome::ErrHttp;
};

// Résultat d'un `ping`. ~1,8 ko depuis le schéma 3 (`values` : 32 x 42 o) :
// il ne passe plus PAR VALEUR dans une file (trois copies : tâche, file, UI),
// mais par ÉCHANGE DE POINTEUR entre deux tampons statiques, exactement comme
// la mise en page. L'UI rend le tampon par pingReturnResult() après usage.
struct PingResult {
  JeedomResult r = JeedomResult::ErrTransport;
  bool         longPoll = false;  // la requête demandait l'attente longue (`wait` > 0)
  uint32_t     startedMs = 0;     // millis() à l'émission de la requête
  PingInfo     info;
};

// Lance les deux tâches. `spare` est le second tampon du double tampon : il
// appartient à la tâche réseau dès cet appel. `store` reçoit les mises en
// page fraîches (l'écriture NVS se fait dans la tâche réseau, hors UI).
void netTasksBegin(LayoutStore *store, const char *host, const char *apikey,
                   const char *device, LayoutBundle *spare);

// --- Demandes (tâche d'affichage) -------------------------------------------
// Toutes non bloquantes ; false si la file est pleine.
bool netRequestLayout();
// `schema` : celui dans lequel `id` a été reçu (Layout::schema).
bool netRequestPress(uint8_t index, int32_t id, uint8_t schema);
bool netRequestFirmware();
bool netRequestOtaApply();  // installe ce que le dernier contrôle a annoncé

// Essai d'identifiants (commande `wifi`, portail) : un `ping` simple émis
// avec `host`/`apikey` donnés, par un client CRÉÉ POUR L'OCCASION. Il part
// forcément après la demande — donc après l'association qu'on veut éprouver.
// Le résultat revient en NetEvent::ProbeDone. Un seul à la fois.
// `gen` revient dans le résultat : une réponse d'un essai PRÉCÉDENT ne doit
// jamais confirmer l'essai en cours.
bool netRequestProbe(const char *host, const char *apikey, const char *device, uint32_t gen);

// Rend à la tâche réseau un tampon `Layout` dont l'UI n'a plus l'usage.
void netReturnLayout(LayoutBundle *buf);

// Résultats, non bloquant.
bool netPollResult(NetResult &out);
// Le pointeur reste valide jusqu'à pingReturnResult(), qui le rend à la
// tâche ping. Tant qu'il n'est pas rendu, elle n'émet plus de ping.
PingResult *pingPollResult();
void pingReturnResult(PingResult *r);

// --- Pilotage du ping (tâche d'affichage) -----------------------------------

// `enabled` : l'UI veut des pings (état RUN, Wi-Fi associé, clé présente).
// `pollSec` et `dimmed` donnent la cadence v2.1 : `poll`, 2 x `poll` écran
// atténué (UI_IDLE_POLL_FACTOR). Appelable à chaque tour de boucle.
void pingConfigure(bool enabled, uint32_t pollSec, bool dimmed);

// Capacités et `rev` reçues dans un `layout` (le ping met à jour les siennes
// lui-même à chaque réponse).
void pingAdoptExtras(const ServerExtras &x);

// Suspend l'attente longue (OTA en cours). Un ping déjà retenu se termine
// seul, sans être relancé.
void pingSuspend(bool suspended);

// Avance le prochain ping à `delayMs` (0 = tout de suite) : réveil de
// l'écran, confirmation d'un `press` en attente. Une simple demande : l'UI ne
// fige plus jamais pour un ping.
void pingPoke(uint32_t delayMs);

// Vrai si l'attente longue est en service : capacité annoncée ET `rev` connue.
bool pingLongPollActive();

// Journalise le niveau maximal de pile atteint par chaque tâche.
void netLogStacks();
