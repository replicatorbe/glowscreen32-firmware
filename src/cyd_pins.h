#pragma once
// Brochage ESP32-2432S028 (CYD). Source : PINS.md du dépôt witnessmenow.

// Écran ILI9341 / ST7789 — bus HSPI (déclaré via les build_flags TFT_eSPI)
#define CYD_TFT_BL   21   // rétroéclairage, ne rien brancher d'autre sur IO21

// Tactile XPT2046 — bus SPI distinct (VSPI)
#define CYD_TOUCH_CLK  25
#define CYD_TOUCH_MOSI 32
#define CYD_TOUCH_CS   33
#define CYD_TOUCH_IRQ  36
#define CYD_TOUCH_MISO 39

// Carte SD — VSPI également : incompatible avec le tactile sur le même bus,
// passer le tactile en bitbang (XPT2046_Bitbang_Slim) si les deux sont requis.
#define CYD_SD_CS   5
#define CYD_SD_SCK  18
#define CYD_SD_MISO 19
#define CYD_SD_MOSI 23

// LED RGB — logique inversée (LOW = allumé)
#define CYD_LED_R 4
#define CYD_LED_G 16
#define CYD_LED_B 17

// Divers
#define CYD_LDR     34  // photorésistance, 0 = forte lumière
#define CYD_SPEAKER 26  // sortie DAC vers l'ampli
