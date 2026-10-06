// Outils de tracé partagés entre button_grid.cpp et screens.cpp. Interne au
// module ui/ : rien d'autre ne doit en dépendre.
#pragma once

#include <TFT_eSPI.h>

// Charge `font` sur l'écran SEULEMENT si ce n'est pas déjà elle : chaque
// chargement réalloue les métriques (1 680 o en 7 malloc). Voir button_grid.cpp.
void uiUseTftFont(TFT_eSPI &tft, const uint8_t *font);

// Tronque `src` pour tenir dans `maxW` px, avec « … », sur une frontière UTF-8.
void uiFitLabel(TFT_eSPI &dev, const char *src, int16_t maxW, char *out, size_t outLen);
