// Écrans pleins de la v2.2 : menu Réglages, portail de secours, calibration
// tactile, message et identification à distance, attente de redémarrage.
//
// Tous dessinent depuis la tâche d'affichage, seule à toucher au SPI. Tous
// sauf le message prennent l'écran entier (UiCover::All) : la grille cesse
// alors de se repeindre par-dessus, et ButtonGrid::uncover() la restaure.
#include "button_grid.h"
#include "fonts/fonts.h"
#include "touch_calib.h"
#include "ui_internal.h"

// Tampon d'une ligne de message : le texte fait au plus 160 octets
// (JEEDOM_CMD_TEXT_LEN), une ligne ne peut pas être plus longue.
static const size_t JEEDOM_TEXT_WRAP_BUF = 168;

// --- Géométrie commune ----------------------------------------------------------

// Rangée de boutons des écrans pleins, en bas : hauteur d'un doigt sur une
// dalle résistive, comme les zones du bandeau de navigation.
static const int16_t SCR_BTN_Y = 192;
static const int16_t SCR_BTN_H = 44;
static const int16_t SCR_BTN_GAP = 3;
static const int16_t SCR_LINE_Y0 = 36;   // première ligne d'information
static const int16_t SCR_LINE_H = 21;
static const int16_t SCR_VALUE_X = 100;  // colonne des valeurs

static void drawScreenButton(TFT_eSPI &t, int16_t x, int16_t w, const char *label,
                             uint16_t fg) {
  t.fillRoundRect(x, SCR_BTN_Y, w, SCR_BTN_H, UI_RADIUS, UI_SURFACE);
  uiUseTftFont(t, FONT_UI_16);
  t.setTextDatum(MC_DATUM);
  t.setTextColor(fg, UI_SURFACE);
  char buf[32];
  uiFitLabel(t, label, (int16_t)(w - 8), buf, sizeof(buf));
  t.drawString(buf, x + w / 2, SCR_BTN_Y + SCR_BTN_H / 2);
  t.setTextDatum(TL_DATUM);
}

void ButtonGrid::drawScreenHeader(const char *title, bool back) {
  _cover = UiCover::All;
  _shownMinute = -1;
  _statusLeft = -1;  // le bandeau de la grille sera entièrement retracé
  _tft->fillScreen(UI_BG);
  _tft->fillRect(0, 0, UI_SCREEN_W, UI_BANNER_H - 1, UI_BANNER_BG);
  _tft->drawFastHLine(0, UI_BANNER_H - 1, UI_SCREEN_W, UI_HAIRLINE);
  const int16_t cy = (UI_BANNER_H - 1) / 2;
  int16_t tx = 8;
  if (back) {
    // Même geste que sur la grille : le bandeau entier ramène en arrière.
    _tft->fillTriangle(8, cy, 17, cy - 8, 17, cy + 8, UI_ACCENT);
    tx = 25;
  }
  uiUseTftFont(*_tft, FONT_UI_16);
  _tft->setTextDatum(ML_DATUM);
  _tft->setTextColor(UI_TEXT, UI_BANNER_BG);
  _tft->drawString(title, tx, cy);
  _tft->setTextDatum(TL_DATUM);
}

// --- Réglages -------------------------------------------------------------------

static void formatUptime(uint32_t s, char *out, size_t len) {
  const uint32_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;
  if (d) {
    snprintf(out, len, "%lu j %02lu h %02lu", (unsigned long)d, (unsigned long)h,
             (unsigned long)m);
  } else {
    snprintf(out, len, "%lu h %02lu min %02lu s", (unsigned long)h, (unsigned long)m,
             (unsigned long)(s % 60));
  }
}

void ButtonGrid::drawSettingsInfo(const SettingsInfo &s) {
  if (!_tft) return;
  uiUseTftFont(*_tft, FONT_UI_16);
  char v[64], fit[72];
  for (uint8_t i = 0; i < 7; i++) {
    const char *label = "";
    switch (i) {
      case 0: label = "MAC"; snprintf(v, sizeof(v), "%s", s.mac); break;
      case 1: label = "IP"; snprintf(v, sizeof(v), "%s", s.wifiUp ? s.ip : "—"); break;
      case 2:
        label = "Wi-Fi";
        if (s.wifiUp) snprintf(v, sizeof(v), "%s · %d dBm", s.ssid, s.rssi);
        else snprintf(v, sizeof(v), "hors ligne (%s)", s.ssid);
        break;
      case 3: label = "Jeedom"; snprintf(v, sizeof(v), "%s · %s", s.host, s.api); break;
      case 4: label = "Firmware"; snprintf(v, sizeof(v), "%s (%s)", s.fw, s.partition); break;
      case 5: label = "En service"; formatUptime(s.uptimeS, v, sizeof(v)); break;
      default:
        label = "Mémoire";
        snprintf(v, sizeof(v), "%lu ko libres · bloc %lu ko", (unsigned long)(s.heap / 1024),
                 (unsigned long)(s.block / 1024));
        break;
    }
    const int16_t y = SCR_LINE_Y0 + i * SCR_LINE_H;
    // Effacement LIGNE par ligne, juste avant de la retracer : les valeurs
    // bougent toutes les 2 s (heap, durée), un effacement global clignoterait.
    _tft->fillRect(0, y, UI_SCREEN_W, SCR_LINE_H, UI_BG);
    _tft->setTextColor(UI_TEXT_DIM, UI_BG);
    _tft->drawString(label, 10, y + 2);
    _tft->setTextColor(UI_TEXT, UI_BG);
    uiFitLabel(*_tft, v, (int16_t)(UI_SCREEN_W - SCR_VALUE_X - 8), fit, sizeof(fit));
    _tft->drawString(fit, SCR_VALUE_X, y + 2);
  }
}

void ButtonGrid::drawSettings(const SettingsInfo &s) {
  if (!_tft) return;
  drawScreenHeader("Réglages", true);
  drawSettingsInfo(s);
  const int16_t w = (UI_SCREEN_W - 2 * 4 - 2 * SCR_BTN_GAP) / 3;
  drawScreenButton(*_tft, 4, w, "Calibrer", UI_TEXT);
  drawScreenButton(*_tft, 4 + w + SCR_BTN_GAP, w, s.portalOpen ? "Portail" : "Wi-Fi",
                   UI_TEXT);
  // Redémarrer demande DEUX appuis : un seul, frôlé par erreur, couperait
  // l'écran — et, sur un firmware pas encore validé, ramènerait la version
  // précédente.
  drawScreenButton(*_tft, 4 + 2 * (w + SCR_BTN_GAP), w,
                   s.confirmReboot ? "Confirmer ?" : "Redémarrer",
                   s.confirmReboot ? UI_ERROR : UI_TEXT);
}

ScreenHit ButtonGrid::settingsHitTest(int16_t x, int16_t y) const {
  if (y < UI_BANNER_H) return ScreenHit::Back;
  if (y < SCR_BTN_Y - 6) return ScreenHit::None;
  const int16_t w = (UI_SCREEN_W - 2 * 4 - 2 * SCR_BTN_GAP) / 3;
  const int16_t col = (x - 4) / (w + SCR_BTN_GAP);
  switch (col) {
    case 0:  return ScreenHit::Calibrate;
    case 1:  return ScreenHit::Portal;
    case 2:  return ScreenHit::Reboot;
    default: return ScreenHit::None;
  }
}

// --- Portail de secours ---------------------------------------------------------

void ButtonGrid::drawPortalStatus(const PortalInfo &p) {
  if (!_tft) return;
  const int16_t y = 166;
  _tft->fillRect(0, y, UI_SCREEN_W, 22, UI_BG);
  uiUseTftFont(*_tft, FONT_UI_16);
  _tft->setTextDatum(TC_DATUM);
  char line[96], fit[100];
  // L'issue d'un essai de réglages prime : c'est ce que l'utilisateur, son
  // téléphone à la main, attend de lire. Le mot de passe reste affiché.
  if (p.trial && p.trial[0]) {
    const bool failed = !strncmp(p.trial, "Échec", strlen("Échec"));
    _tft->setTextColor(failed ? UI_ERROR : UI_WARN, UI_BG);
    uiFitLabel(*_tft, p.trial, UI_SCREEN_W - 16, fit, sizeof(fit));
    _tft->drawString(fit, UI_SCREEN_W / 2, y + 2);
    _tft->setTextDatum(TL_DATUM);
    return;
  }
  if (p.staUp) {
    snprintf(line, sizeof(line), "Connecté à %s", p.targetSsid);
  } else {
    snprintf(line, sizeof(line), "Recherche de « %s »…", p.targetSsid);
  }
  if (p.clients) {
    const size_t n = strlen(line);
    snprintf(line + n, sizeof(line) - n, " · %u appareil%s", p.clients,
             p.clients > 1 ? "s" : "");
  }
  _tft->setTextColor(p.staUp ? UI_OK : UI_TEXT_DIM, UI_BG);
  uiFitLabel(*_tft, line, UI_SCREEN_W - 16, fit, sizeof(fit));
  _tft->drawString(fit, UI_SCREEN_W / 2, y + 2);
  _tft->setTextDatum(TL_DATUM);
}

void ButtonGrid::drawPortal(const PortalInfo &p) {
  if (!_tft) return;
  drawScreenHeader("Portail Wi-Fi", true);
  _tft->setTextDatum(TC_DATUM);

  uiUseTftFont(*_tft, FONT_UI_16);
  _tft->setTextColor(UI_TEXT_DIM, UI_BG);
  _tft->drawString("Rejoindre le réseau", UI_SCREEN_W / 2, 36);
  uiUseTftFont(*_tft, FONT_UI_20);
  _tft->setTextColor(UI_ACCENT, UI_BG);
  _tft->drawString(p.apName, UI_SCREEN_W / 2, 56);

  uiUseTftFont(*_tft, FONT_UI_16);
  _tft->setTextColor(UI_TEXT_DIM, UI_BG);
  _tft->drawString("mot de passe", UI_SCREEN_W / 2, 88);
  // Le mot de passe n'existe QU'ICI : ni journal série, ni page web. Il faut
  // être devant l'écran pour rejoindre le point d'accès.
  uiUseTftFont(*_tft, FONT_UI_30);
  _tft->setTextColor(UI_TEXT, UI_BG);
  _tft->drawString(p.apPass, UI_SCREEN_W / 2, 106);

  uiUseTftFont(*_tft, FONT_UI_16);
  _tft->setTextColor(UI_TEXT_DIM, UI_BG);
  char url[40];
  snprintf(url, sizeof(url), "puis ouvrir http://%s", p.apIp);
  _tft->drawString(url, UI_SCREEN_W / 2, 144);
  _tft->setTextDatum(TL_DATUM);

  drawPortalStatus(p);
  drawScreenButton(*_tft, 4, UI_SCREEN_W - 8, "Fermer le portail", UI_TEXT);
}

ScreenHit ButtonGrid::portalHitTest(int16_t x, int16_t y) const {
  (void)x;
  if (y < UI_BANNER_H) return ScreenHit::Back;
  if (y >= SCR_BTN_Y - 6) return ScreenHit::ClosePortal;
  return ScreenHit::None;
}

// --- Calibration ----------------------------------------------------------------

void ButtonGrid::drawCalibTarget(uint8_t i, uint8_t n) {
  if (!_tft) return;
  _cover = UiCover::All;
  _tft->fillScreen(UI_BG);
  _tft->setTextDatum(TC_DATUM);
  uiUseTftFont(*_tft, FONT_UI_20);
  _tft->setTextColor(UI_TEXT, UI_BG);
  _tft->drawString("Calibration tactile", UI_SCREEN_W / 2, 74);
  uiUseTftFont(*_tft, FONT_UI_16);
  char buf[48];
  if (i < n) {
    snprintf(buf, sizeof(buf), "Touchez le centre de la cible %u/%u", i + 1, n);
  } else {
    snprintf(buf, sizeof(buf), "Contrôle : touchez la croix");
  }
  _tft->setTextColor(UI_TEXT_DIM, UI_BG);
  // Le texte du contrôle est AU-DESSUS de la croix centrale, pas dessus.
  _tft->drawString(buf, UI_SCREEN_W / 2, i < n ? 108 : 60);
  _tft->drawString(i < n ? "Sans toucher 60 s : abandon" : "Sans toucher 15 s : abandon",
                   UI_SCREEN_W / 2, i < n ? 132 : 160);
  _tft->setTextDatum(TL_DATUM);

  int16_t x, y;
  touchCalTargetPos(i, UI_SCREEN_W, UI_SCREEN_H, x, y);
  _tft->drawCircle(x, y, 10, UI_WARN);
  _tft->drawCircle(x, y, 2, UI_WARN);
  _tft->drawFastHLine(x - 14, y, 29, UI_WARN);
  _tft->drawFastVLine(x, y - 14, 29, UI_WARN);
}

void ButtonGrid::drawCalibOutcome(bool saved, const char *title, const char *detail) {
  if (!_tft) return;
  _cover = UiCover::All;
  _tft->fillScreen(UI_BG);
  _tft->setTextDatum(TC_DATUM);
  uiUseTftFont(*_tft, FONT_UI_20);
  _tft->setTextColor(saved ? UI_OK : UI_WARN, UI_BG);
  char fit[64];
  uiFitLabel(*_tft, title ? title : "", UI_SCREEN_W - 16, fit, sizeof(fit));
  _tft->drawString(fit, UI_SCREEN_W / 2, 90);
  uiUseTftFont(*_tft, FONT_UI_16);
  _tft->setTextColor(UI_TEXT_DIM, UI_BG);
  uiFitLabel(*_tft, detail ? detail : "", UI_SCREEN_W - 16, fit, sizeof(fit));
  _tft->drawString(fit, UI_SCREEN_W / 2, 126);
  _tft->setTextDatum(TL_DATUM);
}

// --- Message --------------------------------------------------------------------

// Découpe `text` en lignes de `maxW` px au plus, aux espaces, et appelle
// `emit` pour chacune (début, longueur en octets). Un mot plus large qu'une
// ligne est coupé sur une frontière de caractère UTF-8. Renvoie le nombre de
// lignes ; un `emit` qui ne fait rien sert au simple comptage, pour centrer.
template <typename F>
static uint8_t wrapText(TFT_eSPI &t, const char *text, int16_t maxW, uint8_t maxLines,
                        F emit) {
  char buf[JEEDOM_TEXT_WRAP_BUF];
  uint8_t lines = 0;
  const char *p = text;
  while (*p && lines < maxLines) {
    while (*p == ' ' || *p == '\n') p++;
    if (!*p) break;
    size_t best = 0;  // longueur de la plus longue ligne qui tient
    size_t i = 0;
    for (;;) {
      // Fin du mot suivant.
      size_t j = i;
      while (p[j] == ' ') j++;
      while (p[j] && p[j] != ' ' && p[j] != '\n') j++;
      if (j >= sizeof(buf)) break;
      memcpy(buf, p, j);
      buf[j] = '\0';
      if (t.textWidth(buf) > maxW) break;
      best = j;
      if (!p[j] || p[j] == '\n') break;
      i = j;
    }
    if (best == 0) {
      // Un seul mot trop long : on le coupe caractère par caractère.
      size_t j = 0;
      while (p[j] && p[j] != ' ' && p[j] != '\n' && j + 4 < sizeof(buf)) {
        size_t k = j + 1;
        while (((uint8_t)p[k] & 0xC0) == 0x80) k++;
        memcpy(buf, p, k);
        buf[k] = '\0';
        if (t.textWidth(buf) > maxW && j > 0) break;
        j = k;
      }
      best = j ? j : 1;
    }
    emit(lines, p, best);
    lines++;
    p += best;
  }
  return lines;
}

void ButtonGrid::drawMessage(const char *text) {
  if (!_tft) return;
  // Superposition : le bandeau reste vivant (heure, anomalies), la grille
  // dessous se fige jusqu'à la fermeture.
  _cover = UiCover::Grid;
  const int16_t x = 10, y = UI_GRID_TOP + 4, w = UI_SCREEN_W - 20,
                h = UI_SCREEN_H - y - 6;
  _tft->fillRoundRect(x, y, w, h, UI_RADIUS, UI_SURFACE);
  _tft->drawRoundRect(x, y, w, h, UI_RADIUS, UI_ACCENT);

  const int16_t maxW = w - 24;
  const int16_t areaH = h - 34;  // place réservée à l'invite du bas
  // Police 20 px si le message tient en 5 lignes, sinon 16 px.
  const uint8_t *font = FONT_UI_20;
  int16_t lineH = 25;
  uiUseTftFont(*_tft, font);
  auto none = [](uint8_t, const char *, size_t) {};
  uint8_t n = wrapText(*_tft, text ? text : "", maxW, 8, none);
  if (n * lineH > areaH) {
    font = FONT_UI_16;
    lineH = 20;
    uiUseTftFont(*_tft, font);
    n = wrapText(*_tft, text ? text : "", maxW, 8, none);
  }
  const uint8_t maxLines = (uint8_t)(areaH / lineH);
  if (n > maxLines) n = maxLines;

  _tft->setTextDatum(TC_DATUM);
  _tft->setTextColor(UI_TEXT, UI_SURFACE);
  const int16_t top = y + 8 + (areaH - n * lineH) / 2;
  TFT_eSPI &t = *_tft;
  wrapText(t, text ? text : "", maxW, maxLines,
           [&](uint8_t i, const char *start, size_t len) {
             char line[JEEDOM_TEXT_WRAP_BUF];
             if (len >= sizeof(line)) len = sizeof(line) - 1;
             memcpy(line, start, len);
             line[len] = '\0';
             t.drawString(line, UI_SCREEN_W / 2, top + i * lineH);
           });

  uiUseTftFont(*_tft, FONT_UI_16);
  _tft->setTextColor(UI_TEXT_DIM, UI_SURFACE);
  _tft->drawString("Touchez pour fermer", UI_SCREEN_W / 2, y + h - 26);
  _tft->setTextDatum(TL_DATUM);
}

// --- Identification ---------------------------------------------------------------

void ButtonGrid::drawIdentify(const char *name, const char *mac, const char *ip,
                              const char *fw) {
  if (!_tft) return;
  _cover = UiCover::All;
  _tft->fillScreen(UI_BG);
  _tft->setTextDatum(TC_DATUM);
  uiUseTftFont(*_tft, FONT_UI_16);
  _tft->setTextColor(UI_TEXT_DIM, UI_BG);
  _tft->drawString("Identification", UI_SCREEN_W / 2, 22);

  char buf[64];
  uiUseTftFont(*_tft, FONT_UI_30);
  _tft->setTextColor(UI_ACCENT, UI_BG);
  uiFitLabel(*_tft, name, UI_SCREEN_W - 16, buf, sizeof(buf));
  _tft->drawString(buf, UI_SCREEN_W / 2, 56);

  uiUseTftFont(*_tft, FONT_UI_20);
  _tft->setTextColor(UI_TEXT, UI_BG);
  uiFitLabel(*_tft, mac, UI_SCREEN_W - 16, buf, sizeof(buf));
  _tft->drawString(buf, UI_SCREEN_W / 2, 112);
  uiFitLabel(*_tft, ip, UI_SCREEN_W - 16, buf, sizeof(buf));
  _tft->drawString(buf, UI_SCREEN_W / 2, 144);

  uiUseTftFont(*_tft, FONT_UI_16);
  _tft->setTextColor(UI_TEXT_DIM, UI_BG);
  char foot[64];
  snprintf(foot, sizeof(foot), "firmware %s · touchez pour fermer", fw);
  uiFitLabel(*_tft, foot, UI_SCREEN_W - 16, buf, sizeof(buf));
  _tft->drawString(buf, UI_SCREEN_W / 2, 200);
  _tft->setTextDatum(TL_DATUM);
}

// --- Attente de redémarrage -------------------------------------------------------

void ButtonGrid::drawNotice(const char *title, const char *detail) {
  if (!_tft) return;
  _cover = UiCover::All;
  _tft->fillScreen(UI_BG);
  _tft->setTextDatum(TC_DATUM);
  uiUseTftFont(*_tft, FONT_UI_30);
  _tft->setTextColor(UI_TEXT, UI_BG);
  _tft->drawString(title, UI_SCREEN_W / 2, 84);
  uiUseTftFont(*_tft, FONT_UI_16);
  _tft->setTextColor(UI_TEXT_DIM, UI_BG);
  char buf[64];
  uiFitLabel(*_tft, detail ? detail : "", UI_SCREEN_W - 16, buf, sizeof(buf));
  _tft->drawString(buf, UI_SCREEN_W / 2, 134);
  _tft->setTextDatum(TL_DATUM);
}
