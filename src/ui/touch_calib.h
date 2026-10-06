// Calibration du tactile XPT2046.
//
// Les valeurs brutes renvoyées par la dalle vont d'environ 200 à 3700 sur les
// deux axes (relevé de référence sur la carte 24:6f:28:12:34:56 :
// x=1711 y=2299 z=2099 pour un appui proche du centre).
//
// ⚠️ DEPUIS LA v2.2, LES BORNES CI-DESSOUS NE SONT PLUS QUE DES VALEURS
// D'USINE. Elles ne servent qu'au premier démarrage (ou après effacement de la
// NVS) : la calibration en service vit en NVS (store/device_config), et se
// refait À L'ÉCRAN — menu Réglages > Calibrer, ou commande `calibrate` depuis
// Jeedom. Compilées, extrapolées d'une seule dalle, elles contredisaient « un
// seul binaire pour tout le parc » : deux dalles résistives n'ont jamais les
// mêmes bornes, et recalibrer imposait de recompiler et de reflasher.
//
// L'outil autonome (environnement `calib`) reste disponible pour le banc :
//     pio run -e calib -t upload && pio device monitor
#pragma once

#include <Arduino.h>

// --- Bornes brutes d'usine ------------------------------------------------
static const int16_t TOUCH_RAW_X_MIN = 200;
static const int16_t TOUCH_RAW_X_MAX = 3700;
static const int16_t TOUCH_RAW_Y_MIN = 240;
static const int16_t TOUCH_RAW_Y_MAX = 3800;

// Bornes brutes en service. Un `min` SUPÉRIEUR au `max` est légitime : il
// encode un axe inversé, que map() traite naturellement. C'est ce que mesure
// la calibration quand la dalle est montée tête-bêche.
struct TouchCal {
  int16_t xMin, xMax, yMin, yMax;
};

static const TouchCal TOUCH_CAL_FACTORY = {TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX,
                                           TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX};

// Écart minimal entre les deux bornes d'un axe. En dessous, la calibration a
// été faite de travers (deux cibles touchées au même endroit) : l'appliquer
// rendrait l'écran inutilisable, et il faudrait un câble pour s'en sortir.
static const int16_t TOUCH_CAL_MIN_SPAN = 1500;

// Garde-fou appliqué À LA LECTURE de la NVS comme à l'issue d'une calibration.
inline bool touchCalPlausible(const TouchCal &c) {
  const int16_t v[4] = {c.xMin, c.xMax, c.yMin, c.yMax};
  for (int16_t x : v) {
    if (x < 0 || x > 4095) return false;  // le XPT2046 convertit sur 12 bits
  }
  return abs(c.xMax - c.xMin) >= TOUCH_CAL_MIN_SPAN &&
         abs(c.yMax - c.yMin) >= TOUCH_CAL_MIN_SPAN;
}

// --- Cibles de calibration ------------------------------------------------
// Quatre cibles à TOUCH_CAL_MARGIN px des bords : haut-gauche, haut-droite,
// bas-droite, bas-gauche. Partagé par l'outil `calib` et par la calibration
// à l'écran du firmware, pour qu'ils mesurent exactement la même chose.
static const uint8_t TOUCH_CAL_TARGETS = 4;
static const int16_t TOUCH_CAL_MARGIN = 20;
// Écart toléré à la cible de contrôle, en pixels.
static const int16_t TOUCH_CAL_CHECK_PX = 20;

inline void touchCalTargetPos(uint8_t i, int16_t w, int16_t h, int16_t &x, int16_t &y) {
  switch (i) {
    case 0:  x = TOUCH_CAL_MARGIN;         y = TOUCH_CAL_MARGIN;         break;
    case 1:  x = w - TOUCH_CAL_MARGIN - 1; y = TOUCH_CAL_MARGIN;         break;
    case 2:  x = w - TOUCH_CAL_MARGIN - 1; y = h - TOUCH_CAL_MARGIN - 1; break;
    case 3:  x = TOUCH_CAL_MARGIN;         y = h - TOUCH_CAL_MARGIN - 1; break;
    // Cible de CONTRÔLE (firmware 2.3.1) : le centre, mesuré avec les bornes
    // qu'on vient de calculer, avant de les écrire.
    default: x = w / 2;                    y = h / 2;                    break;
  }
}

// Bornes déduites des quatre relevés bruts, EXTRAPOLÉES jusqu'aux bords de la
// dalle : les cibles sont à 20 px du bord, pas dessus. Renvoie false si les
// relevés sont incohérents — deux cibles d'un même côté trop éloignées l'une
// de l'autre signalent une cible touchée au mauvais endroit.
inline bool touchCalFromSamples(const int32_t rx[4], const int32_t ry[4], int16_t w,
                                int16_t h, TouchCal &out) {
  // Les cibles 0 et 3 sont à gauche, 1 et 2 à droite ; 0 et 1 en haut.
  const int32_t left   = (rx[0] + rx[3]) / 2;
  const int32_t right  = (rx[1] + rx[2]) / 2;
  const int32_t top    = (ry[0] + ry[1]) / 2;
  const int32_t bottom = (ry[2] + ry[3]) / 2;
  const int32_t tolerance = 600;  // ~50 px d'écart sur un même côté
  if (abs(rx[0] - rx[3]) > tolerance || abs(rx[1] - rx[2]) > tolerance ||
      abs(ry[0] - ry[1]) > tolerance || abs(ry[2] - ry[3]) > tolerance) {
    return false;
  }
  const float spanX = (float)(right - left) / (float)(w - 2 * TOUCH_CAL_MARGIN - 1);
  const float spanY = (float)(bottom - top) / (float)(h - 2 * TOUCH_CAL_MARGIN - 1);
  const int32_t v[4] = {left - (int32_t)(spanX * TOUCH_CAL_MARGIN),
                        right + (int32_t)(spanX * TOUCH_CAL_MARGIN),
                        top - (int32_t)(spanY * TOUCH_CAL_MARGIN),
                        bottom + (int32_t)(spanY * TOUCH_CAL_MARGIN)};
  // L'extrapolation peut déborder de l'échelle 12 bits : on borne.
  out.xMin = (int16_t)constrain(v[0], 0, 4095);
  out.xMax = (int16_t)constrain(v[1], 0, 4095);
  out.yMin = (int16_t)constrain(v[2], 0, 4095);
  out.yMax = (int16_t)constrain(v[3], 0, 4095);
  return touchCalPlausible(out);
}

// --- Orientation ---------------------------------------------------------
// La rotation 1 (paysage 320x240) est déjà appliquée par la bibliothèque
// via touch.setRotation(1) ; ces drapeaux corrigent un axe encore inversé
// ou permuté selon l'exemplaire de dalle.
static const bool TOUCH_SWAP_XY  = false;
static const bool TOUCH_INVERT_X = false;
static const bool TOUCH_INVERT_Y = false;

// --- Pression ------------------------------------------------------------
//
// ⚠️ 400 N'EST PAS NOTRE CHOIX, c'est un PLANCHER IMPOSÉ par la bibliothèque.
// `XPT2046_Touchscreen::touched()` renvoie `zraw >= Z_THRESHOLD` avec
// Z_THRESHOLD figé à 400, et en dessous `update()` ne lit même pas les
// coordonnées (elles sont remises à zéro). Une hystérésis de pression — seuil
// d'entrée haut, seuil de maintien bas — a d'abord été écrite ici : elle était
// INERTE, la bibliothèque coupant avant que notre seuil bas n'ait son mot à
// dire. La descendre demanderait de remplacer le pilote.
//
// La parade au même problème est TEMPORELLE (voir TOUCH_RELEASE_GRACE_MS) :
// on tolère une coupure brève du contact au lieu d'y voir un relâchement.
static const uint16_t TOUCH_Z_MIN = 400;

// Durée pendant laquelle une PERTE de contact n'est pas encore un
// relâchement. La pression fluctue naturellement pendant un appui, et sur une
// dalle bas de gamme elle passe régulièrement sous le plancher de 400 : sans
// ce délai de grâce, un appui maintenu se fragmente en plusieurs appuis, ou
// s'annule. C'est la première cause de « le bouton ne répond pas ».
static const uint16_t TOUCH_RELEASE_GRACE_MS = 60;

// --- Filtrage ------------------------------------------------------------
//
// Les coordonnées sont passées à la MÉDIANE, pas à la moyenne : un unique
// échantillon aberrant — et une dalle résistive en produit — décale une
// moyenne, alors que la médiane l'ignore purement et simplement.
static const uint8_t TOUCH_MEDIAN_N = 5;

// Nombre d'échantillons avant de désigner un bouton. Décider sur le tout
// premier reviendrait à parier sur l'échantillon le plus bruité de la série :
// le contact vient de s'établir et la pression n'est pas encore stable.
// À ~6 ms par tour de boucle, trois échantillons coûtent moins de 20 ms —
// imperceptible, et le retour visuel reste immédiat comme l'exige le contrat.
static const uint8_t TOUCH_SETTLE_SAMPLES = 3;

// Échantillons consécutifs HORS de la tuile avant d'annuler l'appui.
// Annuler dès le premier rendait un seul point aberrant fatal : l'appui était
// perdu sans que rien ne l'indique.
static const uint8_t TOUCH_OFF_TARGET_N = 3;

// Contact plus bref que cela : parasite électrique, pas un doigt.
static const uint16_t TOUCH_MIN_CONTACT_MS = 35;

// Anti-rebond entre deux appuis distincts (ms). Abaissé de 250 à 120 : avec
// l'action déclenchée au relâchement et l'hystérésis ci-dessus, le rebond de
// contact est déjà traité, et 250 ms empêchaient deux appuis volontaires
// rapprochés.
static const uint16_t TOUCH_DEBOUNCE_MS = 120;

// Médiane d'au plus TOUCH_MEDIAN_N valeurs (tri par insertion : n <= 5).
inline int16_t touchMedian(const int16_t *v, uint8_t n) {
  if (n == 0) return 0;
  int16_t t[TOUCH_MEDIAN_N];
  for (uint8_t i = 0; i < n && i < TOUCH_MEDIAN_N; i++) t[i] = v[i];
  for (uint8_t i = 1; i < n; i++) {
    const int16_t k = t[i];
    int8_t j = (int8_t)(i - 1);
    while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
    t[j + 1] = k;
  }
  return t[n / 2];
}

// Convertit un couple brut XPT2046 en pixels écran (largeur x hauteur), avec
// les bornes `cal` — celles de la NVS dans le firmware.
inline void touchRawToScreen(const TouchCal &cal, int16_t rawX, int16_t rawY, int16_t width,
                             int16_t height, int16_t &outX, int16_t &outY) {
  if (TOUCH_SWAP_XY) {
    const int16_t t = rawX;
    rawX = rawY;
    rawY = t;
  }
  // map() diviserait par zéro sur un axe d'étendue nulle. touchCalPlausible()
  // l'interdit déjà ; ce repli ne coûte rien et ne dépend de personne.
  const TouchCal &c = (cal.xMin != cal.xMax && cal.yMin != cal.yMax) ? cal : TOUCH_CAL_FACTORY;
  long x = map((long)rawX, c.xMin, c.xMax, 0, width - 1);
  long y = map((long)rawY, c.yMin, c.yMax, 0, height - 1);
  if (TOUCH_INVERT_X) x = (width - 1) - x;
  if (TOUCH_INVERT_Y) y = (height - 1) - y;
  outX = (int16_t)constrain(x, 0, width - 1);
  outY = (int16_t)constrain(y, 0, height - 1);
}
