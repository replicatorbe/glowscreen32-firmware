// ---------------------------------------------------------------------------
// Polices anti-crénelées embarquées en flash (format VLW de TFT_eSPI).
//
// POURQUOI : les polices bitmap intégrées de TFT_eSPI n'ont pas les accents,
// ce qui obligeait le firmware à translittérer (« Cinéma » -> « Cinema »).
// Ces trois polices portent tout le répertoire français.
//
// USAGE :
//     #include "ui/fonts/fonts.h"
//     tft.loadFont(FONT_UI_20);      // surcharge loadFont(const uint8_t[])
//     tft.drawString("Cinéma", x, y);
//     tft.unloadFont();
//
// ⚠️ loadFont() alloue en RAM ~9 octets par glyphe (140 glyphes => ~1,3 ko)
//    pour les métriques ; unloadFont() les rend. Charger une police à la fois.
// ⚠️ Les chaînes doivent être en UTF-8 : TFT_eSPI décode l'UTF-8 quand une
//    police lissée est chargée (decodeUTF8), il ne faut donc PAS translittérer.
//
// Jeu de caractères : ASCII 0x20-0x7E, plus
//     À Â Ä Ç È É Ê Ë Î Ï Ô Ö Ù Û Ü Ÿ à â ä ç è é ê ë î ï ô ö ù û ü ÿ Œ œ
//     « » ° · – — ’ … € ² ³
// Tout autre point de code est dessiné par TFT_eSPI comme un rectangle vide.
//
// FONTE : Inter (https://rsms.me/inter/), Copyright 2020 The Inter Project
// Authors, sous SIL Open Font License 1.1. Elle autorise l'embarquement dans
// un binaire redistribué ; la seule obligation pratique est de conserver cet
// avis et de ne pas vendre la fonte seule. Instance variable wght=500 pour le
// 16 px (une graisse plus lourde se boucherait), wght=600 pour 20 et 30 px.
//
// GÉNÉRATION : les .cpp sont produits par tools/assets/gen_vlw.py et relus
// par check_vlw.py, qui rejoue le parsing de TFT_eSPI::loadMetrics() et vérifie
// taille_fichier == 24 + 28*gCount + somme(largeur*hauteur). Ne jamais éditer
// les tableaux à la main.
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>

// 16 px — bandeau, textes secondaires.      18 240 octets, interligne 20 px.
extern const uint8_t FONT_UI_16[] PROGMEM;
extern const uint32_t FONT_UI_16_LEN;

// 20 px — libellés de boutons.              26 233 octets, interligne 25 px.
extern const uint8_t FONT_UI_20[] PROGMEM;
extern const uint32_t FONT_UI_20_LEN;

// 30 px — titres, enrôlement, écrans OTA.   50 788 octets, interligne 37 px.
extern const uint8_t FONT_UI_30[] PROGMEM;
extern const uint32_t FONT_UI_30_LEN;
