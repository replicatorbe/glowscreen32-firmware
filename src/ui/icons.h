// ---------------------------------------------------------------------------
// Pictogrammes des tuiles : 39 glyphes en ALPHA 8 BITS, en flash.
//
// Le contrat d'API transporte un NOM (« bulb », « shutter »…) pris dans un
// vocabulaire FERMÉ. Le firmware le résout UNE FOIS au parsing, vers un
// IconId tenant sur un octet, et ne conserve jamais la chaîne : Layout est
// recopié en NVS et chaque octet économisé l'est 32 fois.
//
// FORMAT : un octet par pixel, rangée par rangée, sans en-tête. L'octet est
// l'OPACITÉ du pixel — 0 transparent, 255 opaque, et tout l'intervalle sert.
// Le tracé se fait donc en mélangeant, pour chaque pixel :
//     couleur = (avant_plan * alpha + fond * (255 - alpha)) / 255
// exactement comme TFT_eSPI le fait pour ses polices lissées (alphaBlend).
// Un seuillage binaire redonnerait le crénelage qu'on cherche à supprimer.
//
// DEUX TAILLES, issues de la même source vectorielle — la grande n'est PAS
// un agrandissement de la petite, elle est rastérisée séparément :
//     24x24 ->  576 octets, tuiles des grilles à 3 rangées
//     40x40 -> 1600 octets, grandes tuiles des grilles à 2 rangées
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>

// ⚠️ L'ORDRE EST CELUI DU CONTRAT D'API : un IconId est écrit tel quel en
// NVS par layout_store. Ne jamais insérer au milieu ni réordonner — seulement
// ajouter avant ICON_COUNT, et de concert avec le plugin Jeedom.
enum IconId : uint8_t {
  ICON_NONE = 0, ICON_BULB, ICON_LAMP, ICON_CEILING, ICON_STRIP,
  ICON_PLUG, ICON_POWER, ICON_GATE, ICON_GARAGE, ICON_DOOR,
  ICON_WINDOW, ICON_SHUTTER, ICON_BLIND, ICON_LOCK, ICON_HEAT,
  ICON_COOL, ICON_FAN, ICON_THERMO, ICON_WATER, ICON_VALVE,
  ICON_TV, ICON_MUSIC, ICON_SPEAKER, ICON_CAMERA, ICON_ALARM,
  ICON_SHIELD, ICON_SCENE, ICON_MOVIE, ICON_NIGHT, ICON_SUN,
  ICON_MOON, ICON_COFFEE, ICON_FOLDER, ICON_HOME, ICON_GRID,
  ICON_CAR, ICON_MOWER, ICON_VACUUM, ICON_BELL, ICON_CLOCK,
  ICON_COUNT
};

static const int16_t ICON_W = 24;       // petite taille
static const int16_t ICON_H = 24;
static const int16_t ICON_BIG_W = 40;   // grande taille
static const int16_t ICON_BIG_H = 40;

// Longueur des deux tampons, pour éviter de réécrire 24*24 à la main.
static const size_t ICON_A24_BYTES = 24u * 24u;   //  576
static const size_t ICON_A40_BYTES = 40u * 40u;   // 1600

// Nom du contrat -> identifiant. Insensible à la casse. Nom inconnu,
// chaîne vide ou NULL -> ICON_NONE.
IconId iconFromName(const char *name);

// Nom canonique d'un identifiant (utile aux journaux). Jamais NULL.
const char *iconName(IconId id);

// 576 octets d'alpha (24*24), ou nullptr pour ICON_NONE et pour tout
// identifiant hors bornes : l'appelant se rabat sur le seul libellé.
const uint8_t *iconAlpha24(IconId id);

// 1600 octets d'alpha (40*40), mêmes conventions.
const uint8_t *iconAlpha40(IconId id);
