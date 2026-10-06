// Modèle de données partagé : la mise en page renvoyée par `action=layout`.
// Structures à taille fixe, sans allocation — la carte n'a pas de PSRAM et ce
// bloc est écrit tel quel en NVS (voir store/layout_store).
//
// ⚠️ DEUX RÈGLES qui ont chacune une raison chiffrée :
//
// 1. `Layout` fait ~1,6 ko. La tâche Arduino n'a que 8 ko de PILE. Toute
//    instance doit donc être STATIQUE ou GLOBALE, jamais une variable locale :
//    le chemin de sauvegarde en manipule trois à la fois (la fraîche, le cache
//    relu, la comparaison), sous HTTPClient qui a déjà consommé sa part.
//    En v1.4 la structure faisait 316 octets et le problème n'existait pas.
//
// 2. AUCUN champ volatil ici. `info` (la température du bandeau) et l'heure
//    changent à chaque ping ; s'ils étaient dans `Layout`, LayoutStore::save()
//    verrait le contenu différer et réécrirait la NVS toutes les 30 secondes.
//    Ils vivent dans l'état du bandeau (ui/button_grid), pas dans le modèle.
#pragma once

#include <Arduino.h>

// Version de schéma annoncée au plugin (en-tête X-GLOWSCREEN32-SCHEMA).
// 3 depuis le firmware 2.3.0 (contrat v3.0) : tuiles `view` et `ui.readonly`.
// Un plugin plus ancien répond en 2 ou en 1 — toujours lisible ici.
#define LAYOUT_SCHEMA 3

// Bornes NORMATIVES du contrat v2.0. Un dépassement est journalisé, jamais
// absorbé en silence : en v1.4 la troncature était muette et un écran pouvait
// afficher 6 boutons sur 8 sans que personne ne puisse le savoir.
#define LAYOUT_MAX_BUTTONS 32
#define LAYOUT_MAX_PAGES   4
#define LAYOUT_MAX_COLS    4
#define LAYOUT_MAX_ROWS    3
#define LAYOUT_MAX_SLOTS   (LAYOUT_MAX_COLS * LAYOUT_MAX_ROWS)

// Le contrat borne `label` et `title` à 24 CARACTÈRES, pas 24 octets : en UTF-8
// un « é » en coûte deux. Jusqu'en 2.3.0 les tampons faisaient 32 octets, et
// un libellé de 24 lettres accentuées (48 octets) était coupé EN SILENCE à
// 15 caractères. Depuis la 2.3.1 : 24 x 2 octets + terminateur, et toute
// troncature au-delà de 24 caractères est journalisée (layoutCopyUtf8Chars).
#define LAYOUT_TEXT_CHARS 24
#define LAYOUT_LABEL_LEN  49
#define LAYOUT_TITLE_LEN  49
#define LAYOUT_NAME_LEN   49
// `info` du bandeau : 16 caractères (contrat), volatil — hors de `Layout`.
#define LAYOUT_INFO_CHARS 16
#define LAYOUT_INFO_LEN   33
#define LAYOUT_DEVICE_LEN 13  // MAC normalisée « 246f28123456 » + terminateur

// Mode d'un bouton (contrat v2.0).
enum ButtonMode : uint8_t {
  BTN_MODE_ACTION = 0,  // déclenche toujours la même chose, `state` souvent null
  BTN_MODE_TOGGLE = 1,  // allume ou éteint selon l'état : la pastille a un sens
  BTN_MODE_NAV    = 2,  // ouvre une page — AUCUN appel réseau, jamais de `press`
  BTN_MODE_VIEW   = 3,  // MONTRE une valeur (schéma 3) — ne déclenche rien, jamais de `press`
};

// --- Tuiles `view` (contrat v3.0) -------------------------------------------
//
// ⚠️ `value` et `tone` sont VOLATILS, comme `state` et comme `info` au
// bandeau : ils changent quand une porte s'ouvre. Ils vivent donc HORS de
// `Layout`, qui est écrit tel quel en NVS — les y mettre ferait réécrire la
// flash à chaque porte qui claque. Au démarrage depuis le cache, une tuile
// `view` affiche « — » jusqu'au premier `ping` (contrat).
//
// Le contrat borne `value` à 16 CARACTÈRES UTF-8 : 40 octets couvrent le
// français (2 octets au plus par lettre accentuée, 3 pour « … » ou « € »),
// et la copie tronque toujours sur une frontière de caractère.
#define TILE_VALUE_CHARS 16
#define TILE_VALUE_LEN   40

// `tone` est un SENS, pas une couleur : la palette est dans ui/theme.h.
enum TileTone : uint8_t {
  TONE_NEUTRAL = 0,  // couleur de la tuile (`color`)
  TONE_OK      = 1,
  TONE_WARN    = 2,
  TONE_ALERT   = 3,
};

struct TileValue {
  char    text[TILE_VALUE_LEN];  // vide si `known` == 0
  uint8_t tone;                  // TileTone
  uint8_t known;                 // 0 : `value` null -> la carte affiche « — »
};

// Indexé comme `Layout::buttons` (rang de stockage), seules les tuiles `view`
// y ont un sens.
struct TileValues {
  TileValue v[LAYOUT_MAX_BUTTONS];
};


// État d'un bouton. Le contrat autorise `null` (commande sans état).
enum ButtonState : int8_t {
  BTN_STATE_UNKNOWN = -1,  // champ `state` à null
  BTN_STATE_OFF     = 0,
  BTN_STATE_ON      = 1,
};

struct LayoutButton {
  // ⚠️ `id` est OPAQUE depuis le contrat v1.3, et GLOBAL à l'écran depuis la
  // v2.0 : c'est le rang du bouton sur l'ensemble des pages, pas un rang par
  // page et surtout pas un identifiant de commande Jeedom. La carte ne
  // l'affiche jamais, ne trie pas dessus, n'en déduit rien — elle le renvoie
  // tel quel à `press`. C'est le plugin qui résout la commande à exécuter.
  int32_t  id;
  char     label[LAYOUT_LABEL_LEN];
  uint16_t color;   // couleur déjà convertie en RGB565
  int8_t   state;   // voir ButtonState — VOLATIL, non significatif en NVS
  uint8_t  mode;    // voir ButtonMode
  uint8_t  icon;    // IconId (ui/icons.h) — résolu au parsing, plus une chaîne
  uint8_t  page;    // page d'appartenance, 0 à LAYOUT_MAX_PAGES-1
  uint8_t  slot;    // case dans la grille, 0 à cols*rows-1
  uint8_t  target;  // page à ouvrir, UNIQUEMENT si mode == BTN_MODE_NAV
};

struct LayoutPage {
  char    title[LAYOUT_TITLE_LEN];
  uint8_t parent;     // page de retour
  uint8_t hasParent;  // 0 sur la page d'accueil
};

struct Layout {
  char    device[LAYOUT_DEVICE_LEN];
  char    name[LAYOUT_NAME_LEN];
  int32_t version;    // incrémenté par Jeedom à chaque changement
  int32_t poll;       // période de rafraîchissement conseillée (s)
  uint8_t schema;     // 1 ou 2, selon ce que le plugin a réellement renvoyé
  uint8_t count;      // nombre de boutons réellement remplis
  uint8_t pageCount;  // 1 à LAYOUT_MAX_PAGES
  uint8_t cols;       // 3 ou 4
  uint8_t rows;       // 2 ou 3
  uint8_t swipe;      // changement de page au balayage autorisé
  uint8_t clock;      // afficher l'heure au bandeau
  uint8_t readonly;   // `ui.readonly` (schéma 3) : AUCUN `press`, navigation conservée
  LayoutPage   pages[LAYOUT_MAX_PAGES];
  LayoutButton buttons[LAYOUT_MAX_BUTTONS];
};

// Une mise en page et ses valeurs volatiles : l'unité du DOUBLE TAMPON entre
// la tâche réseau et la tâche d'affichage (net/net_tasks). Seul `layout` part
// en NVS.
struct LayoutBundle {
  Layout     layout;
  TileValues values;
};

inline bool tileValueEqual(const TileValue &a, const TileValue &b) {
  return a.known == b.known && a.tone == b.tone && strcmp(a.text, b.text) == 0;
}

// Remet une mise en page à zéro. Défauts du contrat v2.0 : une page, grille
// 3x3, pas de balayage, pas d'horloge.
inline void layoutClear(Layout &l) {
  memset(&l, 0, sizeof(l));
  l.poll = 30;
  l.schema = 1;
  l.pageCount = 1;
  l.cols = 3;
  l.rows = 3;
}

// Copie une chaîne UTF-8 dans un champ à taille fixe SANS couper un caractère
// en deux. Tronquer au milieu d'une séquence multi-octets produirait un octet
// orphelin que le décodeur UTF-8 de TFT_eSPI afficherait en parasite.
inline void layoutCopyUtf8(char *dst, size_t dstLen, const char *src) {
  if (!src || dstLen == 0) {
    if (dstLen) dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n >= dstLen) {
    n = dstLen - 1;
    // Reculer tant qu'on est sur un octet de continuation (10xxxxxx).
    while (n > 0 && (src[n] & 0xC0) == 0x80) n--;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// Copie au plus `maxChars` CARACTÈRES UTF-8 (et au plus dstLen-1 octets),
// toujours sur une frontière de caractère. Renvoie vrai si la source a été
// TRONQUÉE : le contrat exige qu'un dépassement soit journalisé, jamais
// absorbé en silence — c'est à l'appelant de le dire, avec le contexte.
inline bool layoutCopyUtf8Chars(char *dst, size_t dstLen, const char *src, size_t maxChars) {
  layoutCopyUtf8(dst, dstLen, src);
  bool cut = src && strlen(src) != strlen(dst);
  size_t chars = 0;
  for (size_t i = 0; dst[i] != '\0'; i++) {
    if (((uint8_t)dst[i] & 0xC0) != 0x80 && ++chars > maxChars) {
      dst[i] = '\0';
      cut = true;
      break;
    }
  }
  return cut;
}

// Nombre de cases de la grille courante.
inline uint8_t layoutSlots(const Layout &l) {
  uint8_t c = l.cols ? l.cols : 3;
  uint8_t r = l.rows ? l.rows : 3;
  return (uint8_t)(c * r);
}

// Index global du bouton occupant `slot` sur `page`, ou -1 si la case est vide.
inline int8_t layoutAt(const Layout &l, uint8_t page, uint8_t slot) {
  for (uint8_t i = 0; i < l.count; i++) {
    if (l.buttons[i].page == page && l.buttons[i].slot == slot) return (int8_t)i;
  }
  return -1;
}

// Titre à afficher au bandeau pour une page : son `title`, ou à défaut le nom
// de l'écran (une configuration à une seule page n'a pas besoin de titre).
inline const char *layoutPageTitle(const Layout &l, uint8_t page) {
  if (page < l.pageCount && l.pages[page].title[0] != '\0') return l.pages[page].title;
  return l.name;
}
