// Portail Wi-Fi de secours (contrat v2.2, « Configuration locale de la carte »).
//
// Sans Wi-Fi depuis 5 minutes — ou à la demande, depuis le menu Réglages —
// la carte ouvre EN PLUS de sa station un point d'accès
// `GlowScreen-<6 derniers caractères de la MAC>`, protégé par un mot de passe
// tiré au hasard et affiché SEULEMENT à l'écran : il faut être devant pour s'y
// connecter. Une page web minimaliste permet d'y saisir SSID, mot de passe,
// hôte Jeedom et clé API ; la carte redémarre ensuite sur ces valeurs.
//
// Pendant ce temps la station continue d'essayer son réseau habituel, et le
// point d'accès se referme dès qu'elle l'a retrouvé (décidé par l'UI).
//
// Répartition des rôles :
//   - l'UI (tâche d'affichage) lève et abaisse le point d'accès : toutes les
//     bascules de mode Wi-Fi restent ainsi dans une seule tâche ;
//   - la tâche "portal" (cœur 0), créée à l'ouverture et détruite à la
//     fermeture, fait tourner DNSServer + WebServer. Elle ne touche jamais à
//     l'écran. Rien n'est alloué tant que le portail est fermé.
#pragma once

#include <Arduino.h>

#include "../store/device_config.h"

// Réglages saisis dans le formulaire, à ÉPROUVER avant toute écriture NVS.
struct PortalSubmission {
  char ssid[CFG_SSID_LEN];
  char pass[CFG_PASS_LEN];
  char host[CFG_HOST_LEN];
  char apikey[CFG_APIKEY_LEN];
};

// Lève le point d'accès et lance la tâche. false si déjà ouvert ou refusé.
bool portalStart(const char *apName, const char *apPass, const char *device);

// Demande l'arrêt. Non bloquant : la tâche peut être au milieu d'une requête
// (WebServer attend jusqu'à 5 s un client lent).
void portalStop();

// À appeler à chaque tour de boucle : abaisse le point d'accès une fois la
// tâche sortie.
void portalService();

bool portalActive();       // point d'accès levé (ou en cours de fermeture)
bool portalRunning();      // ouvert et en service — PAS en cours de fermeture
uint8_t portalClients();   // appareils associés au point d'accès
// Formulaire soumis : l'UI le récupère (une fois) et lance un essai — Wi-Fi
// associé sur le SSID visé PUIS réponse de l'API avec l'hôte et la clé saisis.
// Rien n'est écrit en NVS par le portail : seulement par l'UI, après succès.
bool portalTakeSubmission(PortalSubmission &out);

// Issue du dernier essai, affichée en tête de la page (texte interne, sans
// saisie : pas d'échappement requis).
void portalSetStatus(const char *text);

// Marge de pile minimale de la tâche portail, relevée par elle-même ; 0 si
// le portail n'a jamais tourné.
uint32_t portalStackMargin();
