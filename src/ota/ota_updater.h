// Mise à jour du firmware par le réseau (contrat d'API v1.4) et **filet de
// sécurité au démarrage**.
//
// Deux responsabilités bien distinctes :
//
//  1. `otaApply()` télécharge un binaire, vérifie son empreinte SHA-256 AVANT
//     de basculer la partition d'amorçage, puis redémarre.
//  2. Les fonctions `otaPendingVerify()` / `otaMarkValid()` / `otaRollback()`
//     implémentent le retour arrière automatique : un firmware fraîchement
//     installé doit FAIRE SES PREUVES avant d'être déclaré sain. Sans cela,
//     un bug réseau transforme une mise à jour en panne matérielle — il faut
//     décrocher l'écran du mur et le rebrancher en USB.
#pragma once

#include <Arduino.h>

#include "../net/jeedom_client.h"

// Progression du téléchargement : `pct` de 0 à 100.
typedef void (*OtaProgressCb)(uint8_t pct, uint32_t done, uint32_t total);

enum class OtaOutcome : uint8_t {
  Success,       // écrit et vérifié : la carte va redémarrer
  ErrHttp,       // binaire injoignable ou code HTTP inattendu
  ErrSize,       // taille annoncée != taille reçue
  ErrBegin,      // partition inactive inutilisable (Update.begin)
  ErrWrite,      // échec d'écriture en flash
  ErrTruncated,  // flux interrompu avant la fin
  ErrSha256,     // empreinte non conforme -> on ne bascule PAS
  ErrFinalize,   // Update.end() a refusé l'image
};

const char *otaOutcomeLabel(OtaOutcome o);

// Télécharge, vérifie et installe. Ne bascule la partition d'amorçage QUE si
// le SHA-256 correspond. En cas d'échec, rien n'est modifié : la carte
// continue de tourner sur le firmware actuel.
OtaOutcome otaApply(const FirmwareInfo &info, OtaProgressCb onProgress);

// --- Filet de sécurité -----------------------------------------------------

// Vrai si l'image en cours d'exécution vient d'être installée par OTA et
// n'a pas encore été déclarée saine. Le bootloader la rejettera au prochain
// redémarrage tant que `otaMarkValid()` n'a pas été appelée.
bool otaPendingVerify();

// Déclare l'image saine et annule le retour arrière.
// ⚠️ À n'appeler QU'APRÈS avoir vérifié que la carte fonctionne réellement
// (écran, Wi-Fi, réponse de l'API). L'appeler depuis setup() reviendrait à
// désactiver la protection tout en croyant l'avoir.
void otaMarkValid();

// Marque l'image invalide et redémarre sur la partition précédente.
// Ne revient pas, sauf s'il n'existe aucune partition de repli.
void otaRollback();

// Nom de la partition en cours d'exécution (« app0 » / « app1 »), pour les traces.
const char *otaRunningPartition();

// --- Mémoire des versions rejetées (NVS, espace `glowota`) ------------------
//
// Sans elle, une version qui échoue à sa validation revient en arrière... puis
// est proposée de nouveau par le serveur, retéléchargée, réinstallée, rejetée :
// une boucle d'installations toutes les quelques minutes, chacune avec ses
// 5 à 10 minutes d'écran possiblement inutilisable.
//
//   avant d'installer      -> otaJournalSetTarget(version)   « visée »
//   au démarrage           -> otaJournalBoot(version courante)
//        visée == courante : on attend la validation ;
//        visée != courante ET une partition a été invalidée : RETOUR ARRIÈRE,
//          la visée devient `rejected` ;
//        visée != courante sans partition invalidée : installation interrompue
//          (coupure, échec de téléchargement), visée simplement effacée.
//   à la validation        -> otaJournalClearTarget()
//   à l'annonce d'une mise à jour -> otaJournalAllows(version) : refuse la
//        version `rejected` ; une version différente efface `rejected`.
void otaJournalBoot(const char *currentVersion);
bool otaJournalSetTarget(const char *version);
void otaJournalClearTarget();
bool otaJournalAllows(const char *version);
