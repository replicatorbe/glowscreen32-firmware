// Charte graphique et géométrie de l'interface — un seul endroit à modifier.
//
// Toutes les couleurs sont données en RGB888 et converties à la compilation :
// écrire RGB565(0x3D,0xD6,0x8C) reste relisible, 0x3EB1 ne l'est pas.
#pragma once

#include <Arduino.h>

#define RGB565(r, g, b) \
  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// --- Palette ---------------------------------------------------------------
//
// Sombre par construction : un écran mural allumé en permanence dans une pièce
// de vie doit s'effacer quand on ne le regarde pas. Le fond n'est pas un noir
// pur mais un bleu très sombre — sur cette dalle, le noir absolu fait ressortir
// les fuites du rétroéclairage et donne un rendu sale.

static const uint16_t UI_BG         = RGB565(0x0B, 0x0E, 0x14);  // fond général
static const uint16_t UI_BANNER_BG  = RGB565(0x11, 0x15, 0x1D);  // bandeau haut/bas
static const uint16_t UI_SURFACE    = RGB565(0x1A, 0x1F, 0x2B);  // tuile au repos
static const uint16_t UI_SURFACE_HI = RGB565(0x2B, 0x33, 0x45);  // tuile enfoncée
static const uint16_t UI_HAIRLINE   = RGB565(0x25, 0x2B, 0x38);  // filets, contours
static const uint16_t UI_TEXT       = RGB565(0xE8, 0xEC, 0xF2);  // texte principal
static const uint16_t UI_TEXT_DIM   = RGB565(0x7C, 0x87, 0x98);  // texte secondaire
static const uint16_t UI_OK         = RGB565(0x3D, 0xD6, 0x8C);  // vert
static const uint16_t UI_WARN       = RGB565(0xF5, 0xA5, 0x24);  // ambre
static const uint16_t UI_ERROR      = RGB565(0xF0, 0x56, 0x7A);  // rouge
static const uint16_t UI_ACCENT     = RGB565(0x4C, 0x9A, 0xFF);  // bleu, pastille active

// Tons des tuiles `view` (contrat v3.0). `tone` est un SENS transmis par le
// plugin ; la couleur, elle, est fixée ICI, identique sur tout le parc : une
// porte ouverte a la même couleur sur tous les écrans de la maison, quelle que
// soit la couleur choisie pour la tuile. `neutral` garde la couleur de la
// tuile. Ce sont les couleurs d'état du bandeau : un orange y veut déjà dire
// « attention », un rouge « anomalie ».
static const uint16_t UI_TONE_OK    = UI_OK;     // vert
static const uint16_t UI_TONE_WARN  = UI_WARN;   // orange
static const uint16_t UI_TONE_ALERT = UI_ERROR;  // rouge

// --- Géométrie -------------------------------------------------------------

static const int16_t UI_SCREEN_W = 320;
static const int16_t UI_SCREEN_H = 240;

static const int16_t UI_BANNER_H = 28;  // bandeau haut : titre + info + heure
static const int16_t UI_NAV_H    = 24;  // bandeau bas : retour + pastilles + suivant

// Largeur des deux zones tactiles du bandeau bas (gauche = retour / page
// précédente, droite = page suivante).
//
// ⚠️ Dimensionnées pour un DOIGT sur une dalle RÉSISTIVE bas de gamme, pas
// pour un curseur. La première version n'offrait qu'un triangle de 8 px dans
// un coin, avec 46x22 px de zone sensible : introuvable à l'œil, difficile à
// viser, et le balayage — peu fiable sur ce type de dalle — restait la seule
// autre sortie. Une navigation dont on peut rester prisonnier n'est pas une
// navigation.
static const int16_t UI_NAV_ZONE_W = 96;

static const int16_t UI_MARGIN   = 4;   // marge latérale de la grille
static const int16_t UI_GAP      = 5;   // gouttière entre deux tuiles
static const int16_t UI_RADIUS   = 8;   // rayon des coins de tuile

static const int16_t UI_GRID_TOP    = UI_BANNER_H + 2;             // 30
static const int16_t UI_GRID_BOTTOM = UI_SCREEN_H - UI_NAV_H - 2;  // 218
static const int16_t UI_GRID_W = UI_SCREEN_W - 2 * UI_MARGIN;      // 312
static const int16_t UI_GRID_H = UI_GRID_BOTTOM - UI_GRID_TOP;     // 188

// Dimensions d'une tuile pour une grille donnée. Les valeurs utiles :
//   3x3 -> 100x59 (défaut du contrat)   3x2 -> 100x91
//   4x3 ->  74x59                       4x2 ->  74x91
inline int16_t uiCellW(uint8_t cols) {
  if (cols < 1) cols = 3;
  return (int16_t)((UI_GRID_W - (cols - 1) * UI_GAP) / cols);
}
inline int16_t uiCellH(uint8_t rows) {
  if (rows < 1) rows = 3;
  return (int16_t)((UI_GRID_H - (rows - 1) * UI_GAP) / rows);
}

// Plus grande tuile possible (grille 3x2) : c'est la taille du sprite alloué
// une fois pour toutes. 100 x 91 x 2 octets = 18 200 octets.
//
// ⚠️ Un sprite PLEIN ÉCRAN est hors de portée : 320x240x2 = 153 602 octets en
// UN SEUL bloc contigu, alors que TFT_eSPI n'emprunte la PSRAM que si
// psramFound() — jamais sur cette carte — et que le plus gros bloc libre tombe
// à 100-120 ko une fois la pile Wi-Fi levée. createSprite() renverrait
// nullptr EN SILENCE. Le sprite par tuile, lui, passe largement.
// (Le sprite réel fait la taille EXACTE d'une tuile — voir
// ButtonGrid::ensureSprite(). Ces deux valeurs bornent le pire cas.)
static const int16_t UI_SPRITE_W = 100;
static const int16_t UI_SPRITE_H = 91;

// --- Rétroéclairage --------------------------------------------------------
//
// IO21 est piloté par logiciel. On l'atténue après un temps d'inactivité : un
// panneau mural à pleine luminosité toute la nuit est une nuisance, et le
// premier toucher qui le réveille ne doit RIEN déclencher (voir main.cpp).
// ⚠️ Les deux valeurs d'origine (24/255 après 1 min) étaient trop agressives :
// sur cette dalle, 24/255 ne laisse qu'une bouillie grise où plus rien ne se
// distingue — au point de faire croire à un défaut d'affichage. Un panneau
// mural doit rester LISIBLE au repos, pas s'éteindre : on l'atténue pour ne
// pas éclairer la pièce, pas pour le rendre inutilisable.
static const uint8_t  UI_BL_FULL     = 255;
static const uint8_t  UI_BL_DIM      = 90;        // ~35 %, encore parfaitement lisible
static const uint32_t UI_BL_IDLE_MS  = 180000UL;  // 3 min avant atténuation
static const uint8_t  UI_BL_CHANNEL  = 0;        // canal LEDC
static const uint32_t UI_BL_FREQ_HZ  = 5000;
static const uint8_t  UI_BL_RES_BITS = 8;

// Facteur d'espacement du `ping` quand l'écran est ATTÉNUÉ — contrat v2.1.
//
// Écran allumé, la carte suit `poll` à la lettre. Atténué, plus personne ne
// regarde : rafraîchir des pastilles que nul ne lit coûte une requête PHP au
// serveur, par écran, toutes les 30 secondes et toute la nuit.
//
// Le réveil, lui, déclenche un `ping` IMMÉDIAT (voir backlightWake) : c'est ce
// qui garantit que quelqu'un qui s'approche voit des états frais, et non le
// reliquat du dernier rafraîchissement nocturne. Sans cette contrepartie,
// espacer reviendrait à échanger du trafic inutile contre de l'information
// périmée au seul moment où elle sert.
//
// ⚠️ LA MÊME CONSTANTE EXISTE CÔTÉ PLUGIN (glowscreen32::IDLE_POLL_FACTOR), et
// ce n'est pas une duplication décorative : le seuil « hors ligne » du plugin
// se calcule sur 3 x CE facteur x poll. Augmenter celui-ci sans l'autre ferait
// déclarer tout le parc hors ligne chaque nuit — une alerte qui crie au loup
// toutes les nuits est une alerte qu'on finit par ignorer, y compris le jour
// où un écran meurt pour de bon. Le contrat plafonne à 2.
static const uint32_t UI_IDLE_POLL_FACTOR = 2;

// --- Gestes ----------------------------------------------------------------
//
// Un balayage est un déplacement franchement horizontal, rapide. Les seuils
// sont volontairement exigeants : sur une dalle résistive, un appui appuyé
// dérive de quelques pixels, et prendre cette dérive pour un balayage ferait
// changer de page au lieu d'allumer une lampe.
static const int16_t  UI_SWIPE_MIN_DX     = 55;   // pixels parcourus
static const int16_t  UI_SWIPE_MAX_DY     = 40;   // au-delà, ce n'est plus horizontal
static const uint32_t UI_SWIPE_MAX_MS     = 800;  // au-delà, c'est un appui traînant
// Tolérance de dérive d'un appui. Au-delà, le geste n'est plus considéré
// comme un appui et n'exécute RIEN.
//
// ⚠️ Réglé pour une dalle résistive bas de gamme, dont les coordonnées
// sautent de plusieurs pixels sous une pression ferme. Trop serré, le défaut
// ne se lit pas comme « geste ignoré » mais comme « le bouton ne répond
// pas » — et l'utilisateur appuie plus fort, ce qui aggrave la dérive.
// Reste largement sous UI_SWIPE_MIN_DX (55) : aucun risque de confondre un
// appui tremblé avec un balayage.
static const int16_t  UI_TAP_SLOP         = 28;

// Abscisse la plus à GAUCHE que le bloc d'état a le droit d'atteindre.
//
// ⚠️ Ce n'est PAS la position du bloc : elle se calcule à chaque tracé, à
// partir de ce que le bloc contient réellement. Une valeur fixe coûtait cher —
// mesuré, un nom d'écran de 16 caractères (139 px) était tronqué à 10 alors
// que 202 px étaient libres : 98 px perdus en permanence, pour un pire cas
// théorique (« -85 dBm · 21.4 °C · 14:32 ») qui ne se produisait jamais sur
// cet écran. Cette borne ne sert plus qu'à garantir une place minimale au
// titre quand le bloc d'état, lui, est vraiment chargé.
static const int16_t UI_STATUS_MIN = 88;

// Seuil en dessous duquel la qualité Wi-Fi mérite d'être affichée. Au-dessus,
// le bandeau reste silencieux. Repères relevés sur site : un `press` se perd
// vers -88 dBm, l'OTA meurt à -92/-93 dBm. -75 dBm est donc une alerte, pas
// un seuil de panne — elle laisse le temps de déplacer un répéteur.
static const int UI_RSSI_WEAK_DBM = -75;
