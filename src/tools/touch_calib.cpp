// Outil de calibration du tactile XPT2046 — environnement PlatformIO `calib`.
//
//   pio run -e calib -t upload
//   pio device monitor -e calib
//
// L'écran affiche une cible dans chaque coin, l'une après l'autre. On touche
// la cible, l'outil mémorise les extrêmes, et affiche à la fin les quatre
// bornes mesurées.
//
// Depuis la v2.2 le firmware sait se calibrer LUI-MÊME, à l'écran, et range le
// résultat en NVS (Réglages > Calibrer). Cet outil ne sert plus qu'au banc :
// les bornes qu'il imprime ne deviennent que des valeurs d'usine si on les
// recopie dans src/ui/touch_calib.h. Les deux partagent le calcul
// (touchCalFromSamples) pour mesurer exactement la même chose.
//
// Un appui long (> 2 s) n'importe où recommence la séquence.

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#include "../cyd_pins.h"
#include "../ui/touch_calib.h"

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSpi = SPIClass(VSPI);
XPT2046_Touchscreen touch(CYD_TOUCH_CS, CYD_TOUCH_IRQ);

// Quatre cibles : haut-gauche, haut-droite, bas-droite, bas-gauche.
static const uint8_t TARGET_COUNT = TOUCH_CAL_TARGETS;

static int32_t sampleX[TARGET_COUNT], sampleY[TARGET_COUNT];
static uint8_t current = 0;
static bool finished = false;
// Bornes utilisées pour la vérification : celles d'usine tant qu'aucune
// mesure n'a abouti, puis celles qu'on vient de relever.
static TouchCal measured = TOUCH_CAL_FACTORY;

static void targetPos(uint8_t i, int16_t &x, int16_t &y) {
  touchCalTargetPos(i, tft.width(), tft.height(), x, y);
}

static void drawTarget(uint8_t i) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Calibration tactile", tft.width() / 2, tft.height() / 2 - 20, 4);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(String("Touchez la cible ") + (i + 1) + "/" + TARGET_COUNT,
                 tft.width() / 2, tft.height() / 2 + 10, 2);
  tft.setTextDatum(TL_DATUM);

  int16_t x, y;
  targetPos(i, x, y);
  tft.drawCircle(x, y, 10, TFT_YELLOW);
  tft.drawFastHLine(x - 14, y, 29, TFT_YELLOW);
  tft.drawFastVLine(x, y - 14, 29, TFT_YELLOW);
}

// Affiche et trace le résultat. Ces bornes ne sont plus que des VALEURS
// D'USINE (premier démarrage d'une carte neuve) : le firmware se calibre
// lui-même, à l'écran, et garde le résultat en NVS.
static void showResult() {
  TouchCal cal;
  const bool ok = touchCalFromSamples(sampleX, sampleY, tft.width(), tft.height(), cal);
  if (ok) measured = cal;
  const int32_t xMin = cal.xMin, xMax = cal.xMax, yMin = cal.yMin, yMax = cal.yMax;
  if (!ok) Serial.println("!!! releves incoherents : refaire la sequence (appui long)");

  Serial.println();
  Serial.println("--- Valeurs d'usine, a recopier dans src/ui/touch_calib.h ---");
  Serial.println("--- (cartes neuves seulement : le firmware se calibre a l'ecran) ---");
  Serial.printf("static const int16_t TOUCH_RAW_X_MIN = %ld;\n", (long)xMin);
  Serial.printf("static const int16_t TOUCH_RAW_X_MAX = %ld;\n", (long)xMax);
  Serial.printf("static const int16_t TOUCH_RAW_Y_MIN = %ld;\n", (long)yMin);
  Serial.printf("static const int16_t TOUCH_RAW_Y_MAX = %ld;\n", (long)yMax);
  Serial.printf("// axes inverses : X %s, Y %s\n",
                xMin > xMax ? "OUI" : "non", yMin > yMax ? "OUI" : "non");
  Serial.println("-------------------------------------------");

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("Valeurs relevees", 8, 6, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String("TOUCH_RAW_X_MIN = ") + xMin, 8, 42, 2);
  tft.drawString(String("TOUCH_RAW_X_MAX = ") + xMax, 8, 62, 2);
  tft.drawString(String("TOUCH_RAW_Y_MIN = ") + yMin, 8, 82, 2);
  tft.drawString(String("TOUCH_RAW_Y_MAX = ") + yMax, 8, 102, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Valeurs d'usine (touch_calib.h)", 8, 132, 2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Touchez pour verifier - appui long = relancer", 8, 160, 2);
}

// Mode vérification : un point rouge doit apparaître sous le doigt.
static void verify(const TS_Point &p) {
  int16_t sx, sy;
  touchRawToScreen(measured, p.x, p.y, tft.width(), tft.height(), sx, sy);
  tft.fillCircle(sx, sy, 3, TFT_RED);
  Serial.printf("brut x=%4d y=%4d z=%4d -> ecran %3d,%3d\n", p.x, p.y, p.z, sx, sy);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== Calibration tactile CYD ===");
  Serial.println("Touchez les quatre cibles, dans l'ordre affiche.");

  pinMode(CYD_TFT_BL, OUTPUT);
  digitalWrite(CYD_TFT_BL, HIGH);

  tft.init();
  tft.setRotation(1);

  touchSpi.begin(CYD_TOUCH_CLK, CYD_TOUCH_MISO, CYD_TOUCH_MOSI, CYD_TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(1);

  drawTarget(0);
}

void loop() {
  static uint32_t pressStartMs = 0;
  static bool down = false;

  if (!(touch.tirqTouched() && touch.touched())) {
    down = false;
    pressStartMs = 0;
    delay(10);
    return;
  }

  const TS_Point p = touch.getPoint();
  if (p.z < TOUCH_Z_MIN) return;

  if (!down) {
    down = true;
    pressStartMs = millis();

    if (!finished) {
      // Moyenne de quelques lectures : la dalle résistive est bruitée.
      int32_t sx = 0, sy = 0;
      uint8_t n = 0;
      for (uint8_t i = 0; i < 8 && touch.touched(); i++) {
        const TS_Point q = touch.getPoint();
        if (q.z < TOUCH_Z_MIN) continue;
        sx += q.x;
        sy += q.y;
        n++;
        delay(8);
      }
      if (n == 0) return;
      sampleX[current] = sx / n;
      sampleY[current] = sy / n;
      Serial.printf("cible %u : brut x=%ld y=%ld (%u lectures)\n",
                    current + 1, (long)sampleX[current], (long)sampleY[current], n);

      current++;
      if (current >= TARGET_COUNT) {
        finished = true;
        showResult();
      } else {
        delay(400);  // évite d'enchaîner deux cibles sur le même appui
        drawTarget(current);
      }
    } else {
      verify(p);
    }
  } else if (finished && pressStartMs && millis() - pressStartMs > 2000) {
    // Appui long : on recommence la séquence.
    pressStartMs = 0;
    finished = false;
    current = 0;
    drawTarget(0);
    delay(500);
  }

  delay(10);
}
