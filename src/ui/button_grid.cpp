#include "button_grid.h"

#include "fonts/fonts.h"
#include "icons.h"
#include "ui_internal.h"

// --- Gestion des polices ---------------------------------------------------
//
// TFT_eSPI ne garde QU'UNE police lissée chargée à la fois par instance :
// loadFont() libère la précédente et réalloue les tables de métriques, soit
// 12 octets par glyphe en 7 malloc distincts (Smooth_font.cpp) — 1 680 octets
// pour nos 140 glyphes.
// En changer à chaque tuile fragmenterait la heap pour rien. On suit donc ce
// qui est chargé sur chaque instance et on ne recharge que si nécessaire.
//
// Répartition normale : FONT_UI_16 reste sur l'écran (bandeaux), FONT_UI_20
// reste sur le sprite (libellés). Dans ce régime il n'y a AUCUN rechargement.
static const uint8_t *g_tftFont = nullptr;
static const uint8_t *g_sprFont = nullptr;

static void useFont(TFT_eSPI &dev, const uint8_t *font, const uint8_t *&current) {
  if (current == font) return;
  if (current) dev.unloadFont();
  dev.loadFont(font);
  current = font;
}

// Pour les écrans pleins (screens.cpp) : même suivi, même instance d'écran.
void uiUseTftFont(TFT_eSPI &tft, const uint8_t *font) { useFont(tft, font, g_tftFont); }

// --- Petites opérations de couleur -----------------------------------------

static uint16_t lighten565(uint16_t c, uint8_t amount);

// `t`/255 de `a` par-dessus `b`. Travaille directement dans l'espace 5/6/5 :
// la perte est invisible et ça évite trois conversions par pixel.
static uint16_t mix565(uint16_t a, uint16_t b, uint8_t t) {
  const uint32_t u = t, v = 255u - t;
  const uint32_t r = (((a >> 11) & 0x1F) * u + ((b >> 11) & 0x1F) * v) / 255u;
  const uint32_t g = (((a >> 5) & 0x3F) * u + ((b >> 5) & 0x3F) * v) / 255u;
  const uint32_t l = ((a & 0x1F) * u + (b & 0x1F) * v) / 255u;
  return (uint16_t)((r << 11) | (g << 5) | l);
}

static uint16_t lighten565(uint16_t c, uint8_t amount) {
  uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  r = (uint8_t)min(31, r + (amount >> 3));
  g = (uint8_t)min(63, g + (amount >> 2));
  b = (uint8_t)min(31, b + (amount >> 3));
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static const uint16_t UI_TEXT_DARK = RGB565(0x0B, 0x0E, 0x14);

// Linéarisation sRGB EXACTE, tabulée. 512 octets de RAM, remplis une fois au
// démarrage — le calcul en virgule flottante ne se paie jamais à l'affichage.
//
// ⚠️ Une approximation en gamma 2 (le simple carré) a d'abord été utilisée ici.
// Elle paraît inoffensive — « exacte à quelques pour cent » — mais elle
// SURESTIME systématiquement les teintes sombres saturées, jusqu'à 0,27 de
// contraste sur #34495E. Deux décisions en dépendent, et toutes deux se sont
// trompées : la boucle du plafond s'arrêtait un cran trop tôt, et surtout
// contrast565() choisissait du texte SOMBRE à 3,86:1 sur #5A7184 alors que du
// texte clair y donnait 4,45:1 — soit l'inverse de ce qu'elle est censée faire.
static uint16_t s_srgbLin[256];  // 0..4095

static void initLuminanceTable() {
  for (int i = 0; i < 256; i++) {
    const float c = (float)i / 255.0f;
    const float l = (c <= 0.04045f) ? (c / 12.92f)
                                    : powf((c + 0.055f) / 1.055f, 2.4f);
    s_srgbLin[i] = (uint16_t)(l * 4095.0f + 0.5f);
  }
}

// Luminance relative, échelle 0..4095.
static uint32_t relLum565(uint16_t c) {
  const uint32_t r = s_srgbLin[((c >> 11) & 0x1F) * 255 / 31];
  const uint32_t g = s_srgbLin[((c >> 5) & 0x3F) * 255 / 63];
  const uint32_t b = s_srgbLin[(c & 0x1F) * 255 / 31];
  return (2126u * r + 7152u * g + 722u * b) / 10000u;
}

// L'offset de 0,05 de la formule WCAG, ramené à l'échelle 0..4095.
static const uint32_t UI_WCAG_OFFSET = 205;

// Texte sombre ou texte clair sur `bg` : on retient celui qui donne le
// meilleur contraste, mesuré à la formule WCAG.
//
// ⚠️ La version précédente comparait une moyenne pondérée à un seuil fixe
// (`lum > 140`). Une moyenne pondérée N'EST PAS une luminance : elle ignore la
// correction gamma, et elle se trompait franchement sur les teintes saturées
// de milieu de gamme. Mesuré sur la palette réelle du plugin, elle choisissait
// du texte clair sur #1ABC9C à **2,07:1** — sous le minimum de 3:1 que WCAG
// exige pour du grand texte, donc un libellé qui se perd à trois mètres, ce
// qui est exactement ce que ces tuiles doivent éviter.
static uint16_t contrast565(uint16_t bg) {
  const uint32_t L = relLum565(bg);
  const uint32_t off = UI_WCAG_OFFSET;
  const uint32_t withLight = (relLum565(UI_TEXT) + off) * 1000 / (L + off);
  const uint32_t withDark = (L + off) * 1000 / (relLum565(UI_TEXT_DARK) + off);
  return (withDark > withLight) ? UI_TEXT_DARK : UI_TEXT;
}

// --- Texte -----------------------------------------------------------------

// Tronque `src` pour tenir dans `maxW` en ajoutant « … ». Coupe uniquement sur
// une frontière de caractère UTF-8 : un octet de continuation orphelin
// s'afficherait en parasite.
void uiFitLabel(TFT_eSPI &dev, const char *src, int16_t maxW, char *out, size_t outLen) {
  layoutCopyUtf8(out, outLen, src ? src : "");
  if (dev.textWidth(out) <= maxW) return;

  size_t n = strlen(out);
  while (n > 0) {
    do {
      n--;
    } while (n > 0 && (out[n] & 0xC0) == 0x80);
    // « … » en UTF-8 : E2 80 A6
    if (n + 4 < outLen) {
      out[n] = (char)0xE2;
      out[n + 1] = (char)0x80;
      out[n + 2] = (char)0xA6;
      out[n + 3] = '\0';
    } else {
      out[n] = '\0';
    }
    if (dev.textWidth(out) <= maxW) return;
  }
  out[0] = '\0';
}

// Dessine une icône en ALPHA 8 BITS, fusionnée sur `bg`.
//
// Pourquoi de l'alpha et plus du 1 bit : le texte est rendu par des polices
// VLW anti-crénelées. Des pictogrammes en noir et blanc pur à côté font
// immédiatement « bricolage » — c'est le premier défaut que l'utilisateur a
// relevé sur le rendu. Le coût est de 85 ko de flash, sur 837 ko libres.
//
// Deux tailles GRAVÉES séparément (24 et 40 px) plutôt qu'un agrandissement :
// doubler des pixels donne des marches, interpoler donne du flou. Chacune est
// rastérisée depuis la même source vectorielle.
//
// `bg` est passé explicitement — l'icône est toujours posée sur un aplat connu
// (`base`), donc inutile de relire l'écran pixel par pixel, ce qui serait lent
// et impossible dans un sprite fraîchement rempli.
static void drawIconAlpha(TFT_eSPI &g, int16_t x, int16_t y, uint8_t icon,
                          uint16_t fg, uint16_t bg, bool big) {
  const uint8_t *a = big ? iconAlpha40((IconId)icon) : iconAlpha24((IconId)icon);
  if (!a) return;  // ICON_NONE : la tuile se rabat sur son seul libellé
  const int16_t w = big ? ICON_BIG_W : ICON_W;
  const int16_t h = big ? ICON_BIG_H : ICON_H;

  for (int16_t row = 0; row < h; row++) {
    const uint8_t *line = a + (size_t)row * w;
    int16_t runStart = -1;  // suite de pixels pleinement opaques
    for (int16_t col = 0; col <= w; col++) {
      const uint8_t alpha = (col < w) ? line[col] : 0;
      if (alpha == 255) {
        if (runStart < 0) runStart = col;
        continue;
      }
      // Les traits pleins partent en segments horizontaux : bien plus rapide
      // qu'un drawPixel par point, et c'est l'essentiel de la surface.
      if (runStart >= 0) {
        g.drawFastHLine(x + runStart, y + row, col - runStart, fg);
        runStart = -1;
      }
      if (alpha) g.drawPixel(x + col, y + row, g.alphaBlend(alpha, fg, bg));
    }
  }
}

// --- Cycle de vie ----------------------------------------------------------

void ButtonGrid::begin(TFT_eSPI *tft) {
  initLuminanceTable();  // avant tout calcul de contraste
  _tft = tft;
  _tft->setTextWrap(false);

  // Le sprite n'est PAS alloué ici : sa taille dépend de la grille, qui vient
  // de la mise en page. Voir ensureSprite(), appelée par setLayout().
  useFont(*_tft, FONT_UI_16, g_tftFont);
  Serial.printf("[ui] init, heap libre %u o\n", (unsigned)ESP.getFreeHeap());
}

// Le sprite fait la taille EXACTE d'une tuile, jamais davantage.
//
// ⚠️ Un sprite plus grand que la tuile serait un piège : pushSprite(x, y)
// pousse le sprite ENTIER, donc les rangées excédentaires écraseraient la
// gouttière et le haut de la tuile suivante. Dimensionner au plus juste évite
// le problème à la source, et économise de la RAM au passage :
//     grille 3x3 -> 100x59 = 11 802 o     (au lieu de 100x91 = 18 202 o)
//     grille 3x2 -> 100x91 = 18 202 o     (le pire cas, cf. UI_SPRITE_*)
// TFT_eSPI alloue w*h+1 pixels (Sprite.cpp), d'où le +2 octets.
//
// Un sprite PLEIN ÉCRAN, lui, reste hors de portée : 320x240x2 = 153 602
// octets d'un seul tenant, quand TFT_eSPI n'emprunte la PSRAM que si
// psramFound() — jamais sur cette carte. createSprite() renverrait nullptr
// EN SILENCE.
void ButtonGrid::ensureSprite() {
  const int16_t w = uiCellW(cols());
  const int16_t h = uiCellH(rowsUsed());
  if (_sprite && _spriteW == w && _spriteH == h) return;

  if (!_sprite) {
    _sprite = new TFT_eSprite(_tft);
    _sprite->setColorDepth(16);
    _sprite->setTextWrap(false);
  } else {
    _sprite->deleteSprite();
  }
  _spriteW = _spriteH = 0;

  if (!_sprite->createSprite(w, h)) {
    // Heap trop fragmentée : on continue en traçant directement à l'écran.
    // C'est moins joli (clignotement à chaque tuile) mais parfaitement
    // fonctionnel — un écran dégradé vaut mieux qu'un écran noir.
    Serial.printf("[ui] sprite %dx%d indisponible : tracé direct, heap %u o\n", w, h,
                  (unsigned)ESP.getFreeHeap());
    delete _sprite;
    _sprite = nullptr;
    g_sprFont = nullptr;
    return;
  }
  _spriteW = w;
  _spriteH = h;
  // La police vit sur l'objet, pas sur le tampon : elle survit à un
  // deleteSprite(), mais pas à la destruction de l'objet.
  useFont(*_sprite, FONT_UI_20, g_sprFont);
  Serial.printf("[ui] sprite %dx%d (%u o), heap libre %u o\n", w, h,
                (unsigned)(w * h * 2), (unsigned)ESP.getFreeHeap());
}

void ButtonGrid::setLayout(const Layout *layout, const TileValues *values, bool keepPage) {
  _layout = layout;
  _values = values;
  // Jusqu'en v2.1, toute nouvelle mise en page ramenait à l'accueil : une
  // modification faite dans Jeedom éjectait de sa sous-page quelqu'un qui
  // était en train de s'en servir. On reste où l'on est si la page existe
  // encore.
  if (!keepPage || !_layout || _page >= _layout->pageCount) _page = 0;
  clearPending();
  _pressed = -1;
  ensureSprite();  // la grille a pu changer de dimensions
  drawAll();
}

void ButtonGrid::uncover() {
  _cover = UiCover::None;
  _shownMinute = -1;
  ensureSprite();
  drawAll();
}

void ButtonGrid::setFwVersion(const char *v) {
  layoutCopyUtf8(_fwVersion, sizeof(_fwVersion), v);
}

// --- Navigation ------------------------------------------------------------

bool ButtonGrid::goToPage(uint8_t page) {
  if (!_layout || page >= _layout->pageCount) return false;
  if (page == _page) return true;
  const uint8_t previousRows = rowsUsed();
  _page = page;
  _pressed = -1;

  // ⚠️ Si la hauteur des tuiles change, les nouvelles cases ne recouvrent plus
  // les anciennes et des bandes de la page précédente RESTENT à l'écran :
  // mesuré, 1 500 pixels en descendant d'une page à 3 rangées vers une page à
  // 2, et 2 000 en remontant — dont des morceaux d'icônes flottant dans le
  // vide. C'est l'ajustement automatique des rangées qui le provoque : les
  // deux vont ensemble.
  //
  // On n'efface QUE dans ce cas. À géométrie constante, le redessin case par
  // case reste suffisant, et évite le flash noir de ~60 ms.
  if (rowsUsed() != previousRows && gridShown()) {
    _tft->fillRect(0, UI_GRID_TOP, UI_SCREEN_W, UI_GRID_H, UI_BG);
  }

  // La hauteur des tuiles dépend du nombre de rangées occupées : le sprite
  // doit suivre.
  ensureSprite();
  // Pas de fillScreen : chaque case est repeinte, y compris les vides. C'est
  // ce qui évite le flash noir de ~60 ms à chaque changement de page.
  drawBanner();
  drawGrid();
  drawNav();
  return true;
}

bool ButtonGrid::goBack() {
  if (!_layout || _page >= _layout->pageCount) return false;
  const LayoutPage &p = _layout->pages[_page];
  if (!p.hasParent) return false;
  return goToPage(p.parent);
}

bool ButtonGrid::nextPage() {
  if (!_layout || _layout->pageCount < 2) return false;
  return goToPage((uint8_t)((_page + 1) % _layout->pageCount));
}

bool ButtonGrid::prevPage() {
  if (!_layout || _layout->pageCount < 2) return false;
  return goToPage((uint8_t)((_page + _layout->pageCount - 1) % _layout->pageCount));
}

// --- Composition d'une tuile ------------------------------------------------

// Rangées réellement occupées sur la page affichée.
//
// ⚠️ La grille s'ajuste à SON CONTENU, page par page. Une page de 4 boutons
// sur une grille 3x3 laissait sinon 113 px de noir absolu sous la première
// rangée — 47 % de la hauteur utile, peints exactement de la couleur des
// gouttières. Le résultat ne se lisait pas comme « des emplacements libres »
// mais comme un écran qui a planté en cours de tracé.
//
// En prime, les tuiles passent de 100x59 à 100x91 : les cibles tactiles
// doublent de surface, ce qui compte sur une dalle résistive.
uint8_t ButtonGrid::rowsUsed() const {
  if (!_layout) return 3;
  const uint8_t c = cols();
  const uint8_t slots = layoutSlots(*_layout);
  uint8_t maxRow = 0;
  bool any = false;
  for (uint8_t i = 0; i < _layout->count; i++) {
    const LayoutButton &b = _layout->buttons[i];
    if (b.page != _page || b.slot >= slots) continue;
    const uint8_t r = (uint8_t)(b.slot / c);
    if (!any || r > maxRow) maxRow = r;
    any = true;
  }
  if (!any) return 1;                      // page vide : une rangée, pas trois
  const uint8_t used = (uint8_t)(maxRow + 1);
  return used > rows() ? rows() : used;
}

void ButtonGrid::slotRect(uint8_t slot, int16_t &x, int16_t &y, int16_t &w,
                          int16_t &h) const {
  const uint8_t c = cols();
  w = uiCellW(c);
  h = uiCellH(rowsUsed());
  x = (int16_t)(UI_MARGIN + (slot % c) * (w + UI_GAP));
  y = (int16_t)(UI_GRID_TOP + (slot / c) * (h + UI_GAP));
}

void ButtonGrid::paintTile(const LayoutButton &b, bool highlight, bool pending,
                           int16_t x, int16_t y, int16_t w, int16_t h) {
  // On compose dans le sprite si on l'a, sinon directement à l'écran.
  const bool useSprite = haveSprite() && w == _spriteW && h == _spriteH;
  TFT_eSPI &g = useSprite ? *(TFT_eSPI *)_sprite : *_tft;
  const int16_t ox = useSprite ? 0 : x;
  const int16_t oy = useSprite ? 0 : y;
  const uint8_t *&fontSlot = useSprite ? g_sprFont : g_tftFont;

  // Tuile `view` : rendu à part, mais dans le MÊME sprite et la même
  // géométrie — c'est une tuile parmi les autres, pas un widget étranger.
  if (b.mode == BTN_MODE_VIEW) {
    if (useSprite) {
      _sprite->fillSprite(UI_BG);
    } else {
      _tft->fillRect(x, y, w, h, UI_BG);
    }
    const uint8_t idx = (uint8_t)(&b - _layout->buttons);
    paintViewTile(g, b, _values ? &_values->v[idx] : nullptr, fontSlot, ox, oy, w, h);
    if (useSprite) _sprite->pushSprite(x, y);
    return;
  }

  // Un bouton ALLUMÉ prend toute la couleur ; sinon la tuile reste neutre et
  // la couleur ne subsiste qu'en bandeau supérieur. C'est ce qui rend l'état
  // lisible à trois mètres, ce que la v1.4 ne faisait pas : elle se contentait
  // d'assombrir la couleur à 27 %, une nuance qui ne se voit pas de loin.
  // --- État et couleurs -------------------------------------------------
  //
  // ⚠️ UNE TUILE ÉTEINTE GARDE SA COULEUR, en teinte sombre. Deux versions
  // précédentes s'y sont cassé les dents, chacune d'un côté :
  //
  //   v1.4 : la couleur à 27 % pour OFF, pleine pour ON. Chromatiquement
  //          cohérent, mais ON et OFF ne différaient que par la luminosité —
  //          indiscernable à trois mètres.
  //   v2.0 : OFF en gris neutre (UI_SURFACE), ON en couleur pleine. L'état
  //          devenait lisible, mais la grille virait au damier gris ponctué
  //          d'aplats criards, sans harmonie. Pire, sur cette dalle au noir
  //          médiocre, UI_SURFACE (#181C29) et le fond (#080C10) se
  //          confondent : une tuile éteinte devenait quasi INVISIBLE.
  //          Constaté en photo sur la carte de référence.
  //
  // La sortie tient en trois différences simultanées plutôt qu'une seule :
  // la teinte du fond (28 % -> 100 %), la couleur de l'icône (pastel ->
  // contrastée), et celle du libellé. Chaque tuile reste identifiable par sa
  // couleur, et son état se lit d'un coup d'œil.
  const bool on = (b.mode == BTN_MODE_TOGGLE && b.state == BTN_STATE_ON);

  uint16_t base, fg, iconFg;
  if (on) {
    // ⚠️ Plafond, symétrique du plancher ci-dessous.
    //
    // Relever le fond ÉTEINT l'a rapproché du fond ALLUMÉ : mesuré, l'écart
    // allumé/éteint est tombé à 1,49:1 sur #34495E et 2,12:1 sur #8E44AD.
    // Sur ces teintes sombres l'état ne se lisait plus sur le fond, seulement
    // sur la couleur de l'icône. On éclaircit donc l'allumé jusqu'à ce qu'il
    // soit nettement plus lumineux que l'éteint — une couleur franche n'est
    // pas touchée, elle dépasse le seuil d'emblée.
    // Le critère est un CONTRASTE WCAG, pas un rapport de luminances brutes :
    // sur deux teintes sombres, l'offset de 0,05 de la formule domine, et un
    // rapport de 2,5 entre luminances peut ne donner que 1,58:1 de contraste.
    // Un premier essai comparait les luminances et ne se déclenchait jamais.
    const uint32_t off = UI_WCAG_OFFSET;
    const uint32_t offLum = relLum565(mix565(b.color, UI_SURFACE, 100)) + off;
    base = b.color;
    uint8_t lift = 0;
    while ((relLum565(base) + off) * 10 < offLum * 25 && lift < 160) {
      lift = (uint8_t)(lift + 20);
      base = lighten565(b.color, lift);
    }
    if (highlight) base = lighten565(base, 60);
    fg = iconFg = contrast565(base);
  } else {
    // ⚠️ Le mélange se fait sur UI_SURFACE, PAS sur le fond.
    //
    // Mélangée au fond, une couleur déjà sombre ne remonte rien : mesuré,
    // #34495E (bleu ardoise — la teinte que Jeedom propose par défaut pour les
    // boutons de navigation) tombait à 1,13:1 contre le fond, soit MOINS que
    // le gris neutre qu'on venait justement de remplacer. Le défaut revenait
    // par l'autre porte, sur la seule famille de couleurs qui en souffrait.
    // UI_SURFACE sert donc de plancher : une tuile éteinte est au minimum
    // aussi visible qu'avant (1,20:1), et porte en plus sa teinte.
    //     #34495E  1,13:1 -> 1,50:1        #9B59B6  1,40:1 -> 1,96:1
    //     #F1C40F  2,07:1 -> 3,50:1        #2D7FF9  1,46:1 -> 2,18:1
    base = mix565(b.color, UI_SURFACE, highlight ? 150 : 100);
    fg = UI_TEXT;
    // Icône en pastel de la couleur du bouton : c'est elle qui porte
    // l'identité, le libellé reste en blanc pour rester parfaitement lisible.
    iconFg = mix565(b.color, UI_TEXT, 110);
  }

  if (useSprite) {
    _sprite->fillSprite(UI_BG);
  } else {
    _tft->fillRect(x, y, w, h, UI_BG);
  }

  g.fillRoundRect(ox, oy, w, h, UI_RADIUS, base);

  if (highlight) {
    g.drawRoundRect(ox, oy, w, h, UI_RADIUS, UI_TEXT);
    g.drawRoundRect(ox + 1, oy + 1, w - 2, h - 2, UI_RADIUS - 1, UI_TEXT);
  }

  // Pastille « en attente » : creuse tant que le ping suivant n'a pas tranché
  // (contrat v1.3 — `press` renvoie un état attendu, pas constaté).
  if (pending) g.drawCircle(ox + w - 11, oy + 12, 4, fg);

  // Un bouton de navigation annonce qu'il ouvre autre chose.
  if (b.mode == BTN_MODE_NAV) {
    const int16_t cx = ox + w - 10, cy = oy + h / 2;
    g.fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 2, cy, UI_TEXT_DIM);
  }

  // --- Composition verticale : capuchon, icône, libellé -------------------
  //
  // Les hauteurs sont comptées, pas approchées. Sur une grille 3x3 la tuile
  // fait 59 px et le budget est serré :
  //     capuchon 3 + marge 3 + icône 24 + gouttière 3 + ligne 20 + marge 6
  // Une police de 20 px (interligne 25) y déborderait de 4 px sous la tuile,
  // rognant les jambages. On réserve donc le 20 px aux grandes tuiles.
  const bool bigTile = (h >= 80);
  const uint8_t *labelFont = bigTile ? FONT_UI_20 : FONT_UI_16;
  const int16_t lineH = bigTile ? 25 : 20;  // interligne réel des VLW
  const int16_t iconW = bigTile ? ICON_BIG_W : ICON_W;
  const int16_t iconH = bigTile ? ICON_BIG_H : ICON_H;
  const bool hasIcon = (b.icon != ICON_NONE) && (h >= 46);

  int16_t textY;
  if (hasIcon) {
    const int16_t iy = oy + (bigTile ? 10 : 6);
    drawIconAlpha(g, ox + (w - iconW) / 2, iy, b.icon, iconFg, base, bigTile);
    textY = iy + iconH + (bigTile ? 4 : 3);
    // Filet de sécurité : quelle que soit la grille, le libellé reste dans
    // la tuile. Mieux vaut le coller au bas que le laisser déborder.
    const int16_t maxY = oy + h - 2 - lineH;
    if (textY > maxY) textY = maxY;
  } else {
    // Sans icône, le libellé est centré — en tenant compte du capuchon de
    // 3 px, qui n'appartient pas à la zone de texte.
    textY = oy + 3 + (h - 3 - lineH) / 2;
  }

  // Libellé : la police de la tuile si ça tient, sinon la plus petite,
  // sinon tronqué sur une frontière UTF-8 avec « … ».
  char label[LAYOUT_LABEL_LEN + 4];
  // Un bouton `nav` porte un chevron à droite : le libellé doit lui céder la
  // place. Sans cette réserve, les deux boîtes se recouvrent sur 9 px et seule
  // la hauteur des hampes évite la collision — une marge d'un pixel que rien
  // dans le code ne garantit.
  const int16_t maxW = w - 8 - (b.mode == BTN_MODE_NAV ? 10 : 0);
  useFont(g, labelFont, fontSlot);
  if (labelFont != FONT_UI_16 && g.textWidth(b.label) > maxW) {
    useFont(g, FONT_UI_16, fontSlot);
  }
  // `base` est le fond RÉEL sous le texte : c'est lui que TFT_eSPI utilise
  // pour fusionner l'anti-crénelage. Un fond faux donnerait un liseré.
  g.setTextColor(fg, base);
  uiFitLabel(g, b.label, maxW, label, sizeof(label));

  g.setTextDatum(TC_DATUM);
  g.drawString(label, ox + w / 2, textY);
  g.setTextDatum(TL_DATUM);

  // ⚠️ Poussée SANS couleur transparente. Utiliser UI_BG comme transparent
  // serait un piège : contrast565() renvoie EXACTEMENT UI_BG pour le texte
  // sombre des tuiles claires, dont les pixels seraient alors percés — un
  // libellé invisible sur fond jaune ou cyan. Le sprite est pré-rempli en
  // UI_BG, les coins arrondis retombent donc sur la bonne couleur de fond.
  if (useSprite) _sprite->pushSprite(x, y);
}

// Contraste WCAG entre deux couleurs, x100 (300 = 3:1).
static uint32_t contrastX100(uint16_t a, uint16_t b) {
  uint32_t la = relLum565(a) + UI_WCAG_OFFSET, lb = relLum565(b) + UI_WCAG_OFFSET;
  if (la < lb) {
    const uint32_t t = la;
    la = lb;
    lb = t;
  }
  return la * 100 / lb;
}

// `fg` sur `bg` avec au moins 3:1 (WCAG, grand texte). Sinon on l'éclaircit
// par paliers ; s'il ne l'atteint toujours pas, le texte le plus contrasté
// (clair ou sombre). Mesuré en revue 2.3.0 : « alert » sur son propre fond
// sombre ne donnait que 2,79:1, le « — » d'une valeur inconnue 1,24:1.
static uint16_t readableOn(uint16_t fg, uint16_t bg) {
  static const uint32_t MIN_X100 = 300;
  uint16_t c = fg;
  for (uint8_t step = 0; step < 6 && contrastX100(c, bg) < MIN_X100; step++) {
    c = lighten565(c, 40);
  }
  return contrastX100(c, bg) >= MIN_X100 ? c : contrast565(bg);
}

// Tuile `view` (contrat v3.0).
//
//   ┌──────────────┐   capuchon 3 px : couleur du ton
//   │ [ic] Porte   │   ligne du haut : libellé 16 px (+ icône 24 px si la
//   │   Ouverte    │     tuile est GRANDE) ; valeur : la PLUS GRANDE des trois
//   └──────────────┘     polices qui tient, « … » en dernier recours, « — » si null
//
// Sur une tuile de moins de 80 px (grilles à 3 rangées : 59 px), l'icône
// disparaît : elle prenait 24 px de haut, et la valeur tombait en 16 px —
// illisible d'un coup d'œil, alors que c'est tout l'objet de la tuile. Sans
// elle, la valeur passe en 20 px. En 4 colonnes (74 px de large), pas d'icône
// non plus : le libellé n'aurait plus que 38 px.
//
// Couleurs : fond = teinte sombre du ton ; libellé et valeur CONTRASTÉS
// (readableOn, au moins 3:1) — la couleur du ton n'est gardée que si elle se
// lit sur son fond.
void ButtonGrid::paintViewTile(TFT_eSPI &g, const LayoutButton &b, const TileValue *v,
                               const uint8_t *&fontSlot, int16_t ox, int16_t oy, int16_t w,
                               int16_t h) {
  const bool known = v && v->known;
  const uint8_t tone = v ? v->tone : (uint8_t)TONE_NEUTRAL;
  uint16_t toneColor = b.color;  // neutral : la couleur de la tuile
  if (tone == TONE_OK) toneColor = UI_TONE_OK;
  else if (tone == TONE_WARN) toneColor = UI_TONE_WARN;
  else if (tone == TONE_ALERT) toneColor = UI_TONE_ALERT;

  const uint16_t base = mix565(toneColor, UI_SURFACE, 100);
  g.fillRoundRect(ox, oy, w, h, UI_RADIUS, base);
  g.fillRect(ox + UI_RADIUS, oy, w - 2 * UI_RADIUS, 3, toneColor);

  const bool big = h >= 80;
  const bool hasIcon = big && w >= 90 && (b.icon != ICON_NONE);
  const int16_t topY = oy + 4;
  int16_t lx = ox + 6;
  if (hasIcon) {
    drawIconAlpha(g, lx, topY + 2, b.icon, readableOn(mix565(toneColor, UI_TEXT, 110), base),
                  base, false);
    lx += ICON_W + 4;
  }
  useFont(g, FONT_UI_16, fontSlot);
  char label[LAYOUT_LABEL_LEN + 4];
  const int16_t rowH = hasIcon ? ICON_H + 4 : 20;
  if (hasIcon) {
    uiFitLabel(g, b.label, (int16_t)(ox + w - 6 - lx), label, sizeof(label));
    g.setTextDatum(ML_DATUM);
    g.setTextColor(contrast565(base), base);
    g.drawString(label, lx, topY + 2 + ICON_H / 2);
  } else {
    // Libellé centré sur toute la largeur : en 4 colonnes, chaque pixel compte.
    uiFitLabel(g, b.label, (int16_t)(w - 8), label, sizeof(label));
    g.setTextDatum(TC_DATUM);
    g.setTextColor(contrast565(base), base);
    g.drawString(label, ox + w / 2, topY);
  }

  // Valeur : de la plus grande police à la plus petite. Chaque essai recharge
  // la police du sprite (métriques réallouées) : on ne repeint une tuile
  // `view` que quand SA valeur change, jamais à chaque ping.
  const int16_t areaTop = topY + rowH;
  const int16_t areaH = oy + h - 2 - areaTop;
  const int16_t maxW = w - 8;
  // « — » (U+2014, dans les 140 glyphes) : valeur inconnue ou périmée.
  const char *text = known ? v->text : "\xE2\x80\x94";
  static const uint8_t *const FONTS[3] = {FONT_UI_30, FONT_UI_20, FONT_UI_16};
  static const int16_t LINE_H[3] = {37, 25, 20};  // interlignes réels des VLW
  uint8_t pick = 2;
  for (uint8_t i = 0; i < 2; i++) {
    if (LINE_H[i] > areaH + 2) continue;  // 2 px : les jambages tiennent dans le bas
    useFont(g, FONTS[i], fontSlot);
    if (g.textWidth(text) <= maxW) {
      pick = i;
      break;
    }
  }
  useFont(g, FONTS[pick], fontSlot);
  char fit[TILE_VALUE_LEN + 4];
  uiFitLabel(g, text, maxW, fit, sizeof(fit));
  uint16_t fg = (tone == TONE_NEUTRAL || !known) ? contrast565(base) : readableOn(toneColor, base);
  g.setTextColor(fg, base);
  g.setTextDatum(MC_DATUM);
  int16_t cy = areaTop + areaH / 2;
  if (areaH < LINE_H[pick]) cy = oy + h - 1 - LINE_H[pick] / 2;  // rester dans la tuile
  g.drawString(fit, ox + w / 2, cy);
  g.setTextDatum(TL_DATUM);
}

void ButtonGrid::drawButton(uint8_t index, bool highlight) {
  if (!gridShown() || !_layout || index >= _layout->count) return;
  const LayoutButton &b = _layout->buttons[index];
  if (b.page != _page) return;             // page non affichée : rien à repeindre
  if (b.slot >= layoutSlots(*_layout)) return;  // case hors grille (signalée au parsing)
  int16_t x, y, w, h;
  slotRect(b.slot, x, y, w, h);
  paintTile(b, highlight, _pending[index], x, y, w, h);
}

void ButtonGrid::drawEmptyCell(uint8_t slot) {
  if (!gridShown()) return;
  int16_t x, y, w, h;
  slotRect(slot, x, y, w, h);
  _tft->fillRect(x, y, w, h, UI_BG);
}

void ButtonGrid::drawGrid() {
  if (!gridShown() || !_layout) return;
  const uint8_t slots = slotsShown();
  for (uint8_t s = 0; s < slots; s++) {
    const int8_t idx = layoutAt(*_layout, _page, s);
    if (idx >= 0) {
      drawButton((uint8_t)idx, _pressed == idx);
    } else {
      drawEmptyCell(s);
    }
  }
}

// --- Bandeau haut -----------------------------------------------------------

bool ButtonGrid::localHm(uint8_t &hh, uint8_t &mm) const {
  if (!_clockSet) return false;
  const uint32_t elapsed = (millis() - _clockAtMs) / 1000UL;
  const int64_t local = (int64_t)_clockEpoch + (int64_t)elapsed + (int64_t)_tzOffset;
  const uint32_t secOfDay = (uint32_t)(((local % 86400) + 86400) % 86400);
  hh = (uint8_t)(secOfDay / 3600);
  mm = (uint8_t)((secOfDay % 3600) / 60);
  return true;
}

void ButtonGrid::setClock(uint32_t epochUtc, int32_t tzOffsetSec) {
  if (epochUtc == 0) return;  // le plugin n'a pas renvoyé d'horodatage
  _clockEpoch = epochUtc;
  _clockAtMs = millis();
  _tzOffset = tzOffsetSec;
  _clockSet = true;
}

void ButtonGrid::tickClock() {
  if (!_clockSet || !_layout || !_layout->clock) return;
  uint8_t hh, mm;
  if (!localHm(hh, mm)) return;
  const int16_t minute = (int16_t)hh * 60 + mm;
  if (minute == _shownMinute) return;
  drawStatus();
}

void ButtonGrid::setInfo(const char *info) {
  char next[sizeof(_info)];
  layoutCopyUtf8(next, sizeof(next), info ? info : "");
  if (strcmp(next, _info) == 0) return;
  memcpy(_info, next, sizeof(_info));
  drawStatus();
}

// Repeint la moitié droite du bandeau : anomalie, ou info + heure.
void ButtonGrid::drawStatus() {
  if (!bannerShown()) return;
  useFont(*_tft, FONT_UI_16, g_tftFont);
  const int16_t cy = (UI_BANNER_H - 1) / 2;
  const int16_t SEP = 13;  // point médian + son air de part et d'autre

  // Le bloc d'état est ASSEMBLÉ avant d'être tracé, pour connaître sa largeur
  // réelle. C'est ce qui permet de rendre au titre la place qu'il n'occupe
  // pas : la version précédente lui imposait un budget fixe, calculé sur un
  // pire cas théorique qui ne se produisait jamais sur cet écran.
  struct Seg { const char *text; uint16_t color; int16_t w; };
  Seg seg[3];
  uint8_t n = 0;
  char rssiBuf[14], infoBuf[sizeof(_info) + 4], clockBuf[8];
  int8_t infoIdx = -1, clockIdx = -1;

  const bool degraded = (_api == UiLink::Error) || (_api == UiLink::Offline) || !_wifiUp;

  // 1) Alerte Wi-Fi — la plus à gauche, et JAMAIS abandonnée. C'est le seul
  //    avertissement disponible avant que l'OTA n'échoue (il meurt à -92 dBm).
  //    Une version antérieure la laissait céder devant `info`, donc disparaître
  //    sans trace dès que la température était un peu longue.
  if (_wifiUp && _rssi != 0 && _rssi <= UI_RSSI_WEAK_DBM) {
    snprintf(rssiBuf, sizeof(rssiBuf), "%d dBm", _rssi);
    seg[n++] = {rssiBuf, UI_WARN, 0};
  }

  // 2) Anomalie, ou valeur du bandeau.
  if (degraded) {
    seg[n++] = {_apiLabel, (_api == UiLink::Offline || !_wifiUp) ? UI_WARN : UI_ERROR, 0};
  } else if (_info[0] != '\0') {
    layoutCopyUtf8(infoBuf, sizeof(infoBuf), _info);
    infoIdx = (int8_t)n;
    seg[n++] = {infoBuf, UI_TEXT_DIM, 0};
  }

  // 3) Heure.
  _shownMinute = -1;
  uint8_t hh, mm;
  if (!degraded && _layout && _layout->clock && localHm(hh, mm)) {
    snprintf(clockBuf, sizeof(clockBuf), "%02u:%02u", hh, mm);
    clockIdx = (int8_t)n;
    seg[n++] = {clockBuf, UI_TEXT, 0};
  }

  for (uint8_t i = 0; i < n; i++) seg[i].w = _tft->textWidth(seg[i].text);
  auto total = [&]() {
    int16_t t = 0;
    for (uint8_t i = 0; i < n; i++) t += seg[i].w;
    return (int16_t)(t + (n ? (n - 1) * SEP : 0));
  };
  auto drop = [&](int8_t idx) {
    for (uint8_t i = (uint8_t)idx; i + 1 < n; i++) seg[i] = seg[i + 1];
    n--;
    if (infoIdx > idx) infoIdx--;
    if (clockIdx > idx) clockIdx--;
  };

  // Si ça ne rentre pas, `info` cède en premier (on la tronque, puis on la
  // retire), l'heure ensuite. L'alerte Wi-Fi et l'anomalie ne cèdent jamais.
  const int16_t avail = (int16_t)(UI_SCREEN_W - 8 - UI_STATUS_MIN);
  if (total() > avail && infoIdx >= 0) {
    const int16_t budget = (int16_t)(avail - (total() - seg[infoIdx].w));
    uiFitLabel(*_tft, _info, budget, infoBuf, sizeof(infoBuf));
    if (infoBuf[0] == '\0') {
      drop(infoIdx);
      infoIdx = -1;
    } else {
      seg[infoIdx].w = _tft->textWidth(infoBuf);
    }
  }
  if (total() > avail && clockIdx >= 0) {
    drop(clockIdx);
    clockIdx = -1;
  }
  if (clockIdx >= 0) _shownMinute = (int16_t)hh * 60 + mm;

  const int16_t right = UI_SCREEN_W - 8;
  const int16_t left = (int16_t)(right - total());

  // On efface depuis le bord le plus à gauche jamais atteint : un bloc qui
  // rétrécit laisserait sinon ses pixels derrière lui, définitivement.
  int16_t clearFrom = left;
  if (_statusLeft >= 0 && _statusLeft < clearFrom) clearFrom = _statusLeft;
  clearFrom -= 8;
  if (clearFrom < 0) clearFrom = 0;
  _tft->fillRect(clearFrom, 0, UI_SCREEN_W - clearFrom, UI_BANNER_H - 1, UI_BANNER_BG);

  _tft->setTextDatum(MR_DATUM);
  int16_t x = right;
  for (int8_t i = (int8_t)n - 1; i >= 0; i--) {
    _tft->setTextColor(seg[i].color, UI_BANNER_BG);
    _tft->drawString(seg[i].text, x, cy);
    x -= seg[i].w;
    if (i > 0) {
      // Point médian : un carré de 2 px. fillCircle(r=1) ne dessine pas un
      // point mais une croix de cinq pixels — visible, et laide.
      _tft->fillRect(x - 7, cy - 1, 2, 2, UI_TEXT_DIM);
      x -= SEP;
    }
  }
  _tft->setTextDatum(TL_DATUM);

  // La frontière a bougé : le titre a gagné ou perdu de la place.
  if (left != _statusLeft) {
    _statusLeft = left;
    drawTitle();
  }
}

void ButtonGrid::drawTitle() {
  if (!bannerShown()) return;
  // ⚠️ Le repli ne doit JAMAIS valoir la largeur de l'écran : drawTitle()
  // effacerait alors 0..305, c'est-à-dire le bloc d'état qu'on vient de
  // tracer. Inatteignable avec les libellés actuels (pire cas réel
  // « -100 dBm · Injoignable » = 170 px), mais un libellé d'anomalie plus
  // long y mènerait.
  int16_t border = _statusLeft;
  if (border < 0) border = UI_SCREEN_W - 8;   // bloc d'état jamais tracé
  if (border < UI_STATUS_MIN) border = UI_STATUS_MIN;
  const int16_t budget = (int16_t)(border - 8 - 8);
  if (budget <= 0) return;

  _tft->fillRect(0, 0, border - 6, UI_BANNER_H - 1, UI_BANNER_BG);
  useFont(*_tft, FONT_UI_16, g_tftFont);
  _tft->setTextDatum(ML_DATUM);
  _tft->setTextColor(UI_TEXT, UI_BANNER_BG);
  const int16_t cy = (UI_BANNER_H - 1) / 2;

  // Chevron de retour devant le titre, sur une sous-page. Le bandeau entier
  // était DÉJÀ tactile, mais rien ne le disait : une zone de 320x28 px
  // invisible ne sert à personne. C'est désormais la cible la plus large et
  // la plus facile à viser de l'écran.
  int16_t tx = 8;
  if (_layout && _page < _layout->pageCount && _layout->pages[_page].hasParent) {
    _tft->fillTriangle(8, cy, 17, cy - 8, 17, cy + 8, UI_ACCENT);
    tx = 25;
  }

  char title[LAYOUT_TITLE_LEN + 4];
  const char *src = _layout ? layoutPageTitle(*_layout, _page) : "GlowScreen";
  uiFitLabel(*_tft, src, (int16_t)(budget - (tx - 8)), title, sizeof(title));
  _tft->drawString(title, tx, cy);
  _tft->setTextDatum(TL_DATUM);
}

void ButtonGrid::drawBanner() {
  if (!bannerShown()) return;
  _tft->fillRect(0, 0, UI_SCREEN_W, UI_BANNER_H - 1, UI_BANNER_BG);
  _tft->drawFastHLine(0, UI_BANNER_H - 1, UI_SCREEN_W, UI_HAIRLINE);
  _statusLeft = -1;  // force le retracé du titre par drawStatus()
  drawStatus();
}

void ButtonGrid::setWifi(bool up, int rssi) {
  _wifiUp = up;
  _rssi = rssi;
}

void ButtonGrid::setApi(UiLink link, const char *detail) {
  _api = link;
  layoutCopyUtf8(_apiLabel, sizeof(_apiLabel), detail);
}

// --- Bandeau bas : retour + pastilles de page -------------------------------

void ButtonGrid::drawNav() {
  if (!gridShown()) return;
  const int16_t y0 = UI_SCREEN_H - UI_NAV_H;
  _tft->fillRect(0, y0, UI_SCREEN_W, UI_NAV_H, UI_BANNER_BG);
  _tft->drawFastHLine(0, y0, UI_SCREEN_W, UI_HAIRLINE);
  if (!_layout) return;

  const int16_t cy = y0 + UI_NAV_H / 2;
  const bool hasParent = (_page < _layout->pageCount) && _layout->pages[_page].hasParent;
  const bool multi = _layout->pageCount > 1;

  // Chevron GAUCHE : remonter d'une page, ou page précédente.
  if (hasParent || multi) {
    const int16_t cx = 30;
    _tft->fillTriangle(cx - 7, cy, cx + 4, cy - 9, cx + 4, cy + 9, UI_TEXT);
  }

  // Chevron DROIT : page suivante.
  if (multi) {
    const int16_t cx = UI_SCREEN_W - 30;
    _tft->fillTriangle(cx + 7, cy, cx - 4, cy - 9, cx - 4, cy + 9, UI_TEXT);
  }

  // Pastilles de page — INDICATEUR d'abord, cible ensuite. Inutile d'en
  // dessiner une seule : elle n'apprendrait rien et laisserait croire qu'il y
  // a autre chose à voir.
  if (multi) {
    const int16_t pitch = 16;
    const int16_t total = (int16_t)(_layout->pageCount - 1) * pitch;
    int16_t x = UI_SCREEN_W / 2 - total / 2;
    for (uint8_t p = 0; p < _layout->pageCount; p++) {
      if (p == _page) {
        _tft->fillCircle(x, cy, 4, UI_ACCENT);
      } else {
        _tft->fillCircle(x, cy, 3, UI_TEXT_DIM);
      }
      x += pitch;
    }
  }
  // La version du firmware n'est plus ici : elle disputait sa place au chevron
  // droit, et Jeedom l'affiche déjà pour chaque écran du parc. Elle reste
  // visible là où elle sert vraiment — enrôlement et écrans de mise à jour.
}

void ButtonGrid::drawAll() {
  if (!gridShown()) {
    drawBanner();  // sous une superposition, le bandeau reste vivant
    return;
  }
  _tft->fillScreen(UI_BG);
  drawBanner();
  drawGrid();
  drawNav();
}

// --- Appuis et états --------------------------------------------------------

void ButtonGrid::setPressed(int8_t index) {
  const int8_t previous = _pressed;
  _pressed = index;
  if (previous >= 0 && previous != index) drawButton((uint8_t)previous, false);
  if (index >= 0) drawButton((uint8_t)index, true);
}

void ButtonGrid::refreshButton(uint8_t index) {
  drawButton(index, _pressed == (int8_t)index);
}

void ButtonGrid::setPending(uint8_t index, bool pending) {
  if (index < LAYOUT_MAX_BUTTONS) _pending[index] = pending;
}

void ButtonGrid::clearPending() {
  memset(_pending, 0, sizeof(_pending));
}

// --- Test de position -------------------------------------------------------

UiHit ButtonGrid::hitTest(int16_t x, int16_t y) const {
  UiHit hit;
  if (!_layout) return hit;

  // Bandeau haut : raccourci de remontée. Contrairement à la v1.4, il n'est
  // plus une zone morte — c'est la cible la plus facile à viser de loin.
  if (y < UI_BANNER_H) {
    if (_page < _layout->pageCount && _layout->pages[_page].hasParent) {
      hit.kind = UiHit::Back;
    }
    return hit;
  }

  // Bandeau bas : trois zones. Les deux latérales font UI_NAV_ZONE_W (96 px)
  // de large sur toute la hauteur du bandeau — dimensionnées pour un doigt sur
  // une dalle résistive, où le balayage n'est pas fiable. La navigation ne
  // doit JAMAIS dépendre d'un geste : on doit toujours pouvoir s'en sortir au
  // simple appui.
  if (y >= UI_GRID_BOTTOM) {
    const uint8_t count = _layout->pageCount;
    const bool hasParent = (_page < count) && _layout->pages[_page].hasParent;
    const bool multi = count > 1;

    if (x < UI_NAV_ZONE_W && (hasParent || multi)) {
      if (hasParent) {
        hit.kind = UiHit::Back;  // remonter prime sur « page précédente »
      } else {
        hit.kind = UiHit::Page;
        hit.index = (int8_t)((_page + count - 1) % count);
      }
      return hit;
    }
    if (x >= UI_SCREEN_W - UI_NAV_ZONE_W && multi) {
      hit.kind = UiHit::Page;
      hit.index = (int8_t)((_page + 1) % count);
      return hit;
    }

    // Zone centrale : saut direct par les pastilles.
    if (multi) {
      const int16_t pitch = 16;
      const int16_t total = (int16_t)(count - 1) * pitch;
      const int16_t x0 = UI_SCREEN_W / 2 - total / 2;
      const int16_t rel = x - (x0 - pitch / 2);
      if (rel >= 0) {
        const int16_t p = rel / pitch;
        if (p < (int16_t)count) {
          hit.kind = UiHit::Page;
          hit.index = (int8_t)p;
        }
      }
    }
    return hit;
  }

  // Grille. La division entière rattache naturellement les gouttières à la
  // case qui précède : un appui pile entre deux tuiles n'est pas perdu.
  const uint8_t c = cols(), r = rowsUsed();
  const int16_t w = uiCellW(c), h = uiCellH(r);
  if (x < UI_MARGIN || y < UI_GRID_TOP) return hit;
  const int16_t col = (x - UI_MARGIN) / (w + UI_GAP);
  const int16_t row = (y - UI_GRID_TOP) / (h + UI_GAP);
  if (col >= c || row >= r) return hit;

  const int8_t idx = layoutAt(*_layout, _page, (uint8_t)(row * c + col));
  if (idx >= 0) {
    const uint8_t mode = _layout->buttons[idx].mode;
    // Tuile `view` : un toucher ne fait RIEN, pas même un retour visuel
    // (contrat v3.0). Écran en lecture seule : seuls les `nav` restent
    // actifs. Les écarter ICI garantit qu'aucun chemin plus loin — retour
    // visuel, `press` — ne peut les voir.
    if (mode == BTN_MODE_VIEW) return hit;
    if (_layout->readonly && mode != BTN_MODE_NAV) return hit;
    hit.kind = UiHit::Button;
    hit.index = idx;
  }
  return hit;
}

// --- Écrans pleins ----------------------------------------------------------

void ButtonGrid::drawEnroll(const char *macPretty, const char *macRaw, const char *ssid) {
  if (!_tft) return;
  _cover = UiCover::All;
  _tft->fillScreen(UI_BG);
  _tft->setTextDatum(TC_DATUM);

  useFont(*_tft, FONT_UI_20, g_tftFont);
  _tft->setTextColor(UI_TEXT, UI_BG);
  _tft->drawString("Écran non déclaré", UI_SCREEN_W / 2, 26);

  useFont(*_tft, FONT_UI_16, g_tftFont);
  _tft->setTextColor(UI_TEXT_DIM, UI_BG);
  _tft->drawString("Créez l'équipement dans Jeedom", UI_SCREEN_W / 2, 56);

  // La MAC en grand : c'est toute la raison d'être de cet écran. Elle permet
  // de déclarer la carte sans brancher un câble série.
  _tft->fillRoundRect(24, 92, UI_SCREEN_W - 48, 54, UI_RADIUS, UI_SURFACE);
  useFont(*_tft, FONT_UI_30, g_tftFont);
  _tft->setTextColor(UI_ACCENT, UI_SURFACE);
  // La MAC est la SEULE information utile de cet écran. En 30 px elle mesure
  // 263 px pour une boîte de 272 : quatre pixels de marge de chaque côté, et
  // rien ne garantissait qu'un autre format tienne. On la borne.
  char mac[24];
  uiFitLabel(*_tft, macPretty ? macPretty : "??", 264, mac, sizeof(mac));
  _tft->drawString(mac, UI_SCREEN_W / 2, 105);

  useFont(*_tft, FONT_UI_16, g_tftFont);
  _tft->setTextColor(UI_TEXT_DIM, UI_BG);
  _tft->drawString(macRaw ? macRaw : "", UI_SCREEN_W / 2, 158);

  // Réseau associé + version : les deux informations qu'on cherche justement
  // quand on déclare un écran neuf, et qu'on ne peut lire nulle part ailleurs
  // sans câble série. Le NOM du réseau : jusqu'en v2.1 on affichait ici le
  // libellé d'état « WiFi OK », qui n'apprenait rien.
  char foot[64], fit[68];
  snprintf(foot, sizeof(foot), "%s%s%s", (ssid && ssid[0]) ? ssid : "hors ligne",
           _fwVersion[0] ? "  ·  " : "", _fwVersion);
  // Un SSID de 32 caractères déborderait des 320 px : on le borne.
  uiFitLabel(*_tft, foot, UI_SCREEN_W - 16, fit, sizeof(fit));
  _tft->drawString(fit, UI_SCREEN_W / 2, 196);
  _tft->setTextDatum(TL_DATUM);
  _shownMinute = -1;
}

void ButtonGrid::drawOtaScreen(const char *fromVersion, const char *toVersion) {
  if (!_tft) return;
  _cover = UiCover::All;
  _tft->fillScreen(UI_BG);
  _tft->setTextDatum(TC_DATUM);

  useFont(*_tft, FONT_UI_30, g_tftFont);
  _tft->setTextColor(UI_TEXT, UI_BG);
  _tft->drawString("Mise à jour", UI_SCREEN_W / 2, 40);

  useFont(*_tft, FONT_UI_20, g_tftFont);
  char buf[48];
  // ⚠️ Pas de « → » : U+2192 n'est PAS dans les 140 glyphes des polices VLW,
  // et TFT_eSPI dessine alors un rectangle vide. Des mots se lisent mieux de
  // loin qu'une flèche de 16 px, et ne dépendent d'aucun glyphe exotique.
  snprintf(buf, sizeof(buf), "de %s vers %s", fromVersion ? fromVersion : "?",
           toVersion ? toVersion : "?");
  _tft->setTextColor(UI_TEXT_DIM, UI_BG);
  _tft->drawString(buf, UI_SCREEN_W / 2, 86);

  useFont(*_tft, FONT_UI_16, g_tftFont);
  _tft->drawString("Ne pas débrancher", UI_SCREEN_W / 2, 186);
  _tft->setTextDatum(TL_DATUM);

  // Gouttière de la barre de progression.
  _tft->fillRoundRect(40, 130, UI_SCREEN_W - 80, 20, 6, UI_SURFACE);
  _shownMinute = -1;
}

void ButtonGrid::drawOtaProgress(uint8_t pct) {
  if (!_tft) return;
  if (pct > 100) pct = 100;
  const int16_t w = UI_SCREEN_W - 80;
  const int16_t filled = (int16_t)((int32_t)w * pct / 100);
  if (filled > 0) _tft->fillRoundRect(40, 130, filled, 20, 6, UI_OK);

  useFont(*_tft, FONT_UI_20, g_tftFont);
  _tft->setTextDatum(TC_DATUM);
  _tft->setTextColor(UI_TEXT, UI_BG);
  char buf[8];
  snprintf(buf, sizeof(buf), "%u %%", pct);
  _tft->fillRect(120, 156, 80, 24, UI_BG);
  _tft->drawString(buf, UI_SCREEN_W / 2, 158);
  _tft->setTextDatum(TL_DATUM);
}

void ButtonGrid::drawOtaFailure(const char *reason) {
  if (!_tft) return;
  _cover = UiCover::All;
  _tft->fillScreen(UI_BG);
  _tft->setTextDatum(TC_DATUM);

  useFont(*_tft, FONT_UI_30, g_tftFont);
  _tft->setTextColor(UI_ERROR, UI_BG);
  _tft->drawString("Mise à jour", UI_SCREEN_W / 2, 52);
  _tft->drawString("interrompue", UI_SCREEN_W / 2, 92);

  useFont(*_tft, FONT_UI_20, g_tftFont);
  _tft->setTextColor(UI_TEXT_DIM, UI_BG);
  _tft->drawString(reason ? reason : "", UI_SCREEN_W / 2, 140);

  // Le message qui compte : la carte n'est pas cassée.
  useFont(*_tft, FONT_UI_16, g_tftFont);
  _tft->drawString("Le firmware actuel est intact", UI_SCREEN_W / 2, 184);
  _tft->setTextDatum(TL_DATUM);
  _shownMinute = -1;
}
