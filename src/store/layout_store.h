// Cache NVS de la dernière mise en page connue.
// Objectif : afficher les boutons immédiatement au démarrage, même sans
// Wi-Fi ni serveur Jeedom (règle « démarrage hors ligne » du contrat).
#pragma once

#include "../model/layout.h"

class LayoutStore {
 public:
  // Ouvre l'espace de noms NVS. À appeler une fois au démarrage.
  bool begin();

  // Relit la mise en page en cache. Renvoie false si elle est absente,
  // d'un format incompatible, ou si elle appartient à un AUTRE écran :
  // `expectedDevice` est la MAC normalisée de la carte. Un cache hérité
  // (carte reflashée, NVS clonée) est effacé plutôt qu'affiché.
  bool load(Layout &out, const char *expectedDevice);

  // Écrit la mise en page. N'écrit rien si elle est identique à celle en
  // cache : la flash NVS a un nombre de cycles d'écriture limité.
  bool save(const Layout &in);

  // Efface le cache (outil de dépannage).
  void clear();

 private:
  bool _ready = false;
};
