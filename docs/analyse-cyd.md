# Analyse du dépôt ESP32-Cheap-Yellow-Display

Source : https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display (MIT, sauf `OriginalDocumentation/`)

## Ce qu'est le dépôt

Ce n'est pas une librairie : c'est une **documentation + collection d'exemples** autour d'une
carte chinoise bon marché, la Sunton **ESP32-2432S028R** (ESP32-WROOM-32, écran 2.8" 320x240,
tactile résistif XPT2046, lecteur SD, LED RGB, LDR, ampli audio), surnommée "Cheap Yellow
Display" à cause de son PCB jaune.

### Arborescence

| Dossier / fichier | Rôle |
|---|---|
| `SETUP.md`, `PINS.md`, `TROUBLESHOOTING.md` | La valeur réelle du dépôt : procédure, brochage, dépannage |
| `DisplayConfig/User_Setup.h` | Config TFT_eSPI pour la CYD d'origine |
| `DisplayConfig/CYD2USB/User_Setup.h` | Config pour la révision à 2 ports USB (dalle ST7789) |
| `Examples/Basics/` | 8 exemples atomiques : écran, tactile, SD, backlight, LDR, LED, radio, boutons |
| `Examples/AlternativeLibraries/` | Adafruit_ILI9341, LovyanGFX, bb_spi_lcd |
| `Examples/LVGL8/`, `LVGL9/` | Intégration LVGL avec `lv_conf.h` fourni |
| `Examples/ESPHome/`, `ESP-IDF/`, `Micropython/`, `Rust/`, `NanoFramework/`, `openHASP/` | Mêmes démos portées sur d'autres stacks |
| `Examples/Projects/` | Projets complets (horloge, Tetris, slideshow, SquareLine…) |
| `Variants/` | Autres cartes "PCB jaune" qui **ne sont pas** des CYD |
| `Mods/`, `Hardware/`, `3dModels/` | Mods matériels, PCB annexes, boîtiers imprimables |
| `PROJECTS.md` | ~40 projets communautaires, la plupart avec flash web |

## Brochage (PINS.md)

| Fonction | GPIO |
|---|---|
| TFT (HSPI) | DC **2**, MISO **12**, MOSI **13**, SCLK **14**, CS **15**, BL **21**, RST relié au RST carte |
| Tactile XPT2046 | CLK **25**, MOSI **32**, CS **33**, IRQ **36**, MISO **39** |
| Carte SD (VSPI) | CS **5**, SCK **18**, MISO **19**, MOSI **23** |
| LED RGB (actif bas) | R **4**, V **16**, B **17** |
| LDR | **34** (0 = forte lumière) |
| Haut-parleur | **26** (DAC) |
| GPIO libres | **22**, **27**, **35** (35 = entrée seule, sans pull-up) |
| Connecteurs PicoBlade | P1 VIN/TX/RX/GND · P3 GND/IO35/IO22/IO21 · CN1 GND/IO22/IO27/3V3 (idéal I2C) |

## Contrainte architecturale importante

L'ESP32 n'expose que **deux bus SPI utilisables**, et l'écran, le tactile et la SD en veulent
un chacun. On ne peut donc pas faire tourner les trois en SPI matériel simultanément : il faut
basculer le tactile en SPI logiciel (`XPT2046_Bitbang_Slim`), comme le fait l'exemple `8-Buttons`.

## Deux révisions matérielles

| | CYD original | CYD 2 USB |
|---|---|---|
| Ports | 1 micro-USB | micro-USB + USB-C |
| Contrôleur dalle | ILI9341 (`ILI9341_2_DRIVER`) | ST7789 + `TFT_RGB_ORDER=TFT_BGR` + `TFT_INVERSION_OFF` |
| Symptôme si mauvaise config | — | couleurs inversées |

Le port USB-C de la révision 2 ne fonctionne **pas** avec un câble C↔C (résistances CC absentes
côté carte) : passer par un adaptateur ou un câble C→A.

## Pièges documentés

- **Écran noir** : le plus souvent `User_Setup.h` mal placé, ou IO21 (backlight) réutilisé pour autre chose.
- **`Wrong boot mode detected (0x13)`** : condensateur EN↔GND trop petit (0,1 µF). Remède matériel : le remplacer par 1–10 µF (C5 sur la révision Type-C).
- **Upload qui échoue** : baisser la vitesse à 115200.
- **LDR peu fiable** pour l'auto-luminosité (mod disponible chez `hexeguitar/ESP32_TFT_PIO`).
- **LVGL sous Arduino IDE** : `lv_conf.h` doit aller dans `libraries/`, pas dans le dossier LVGL ; ne pas installer `lv_examples`/`lv_arduino`.

## Choix retenus pour ce projet

On part sur **PlatformIO** plutôt que l'Arduino IDE : la config TFT_eSPI passe par les
`build_flags` du `platformio.ini`, ce qui évite d'aller patcher `User_Setup.h` dans le dossier
de la librairie (la manipulation la plus fragile de la procédure officielle) et rend le projet
reproductible.

## Compléments matériels (schéma officiel Sunton/Jingcai v0.1)

- **Module** ESP32-WROOM-32 (die ESP32-D0WD-V3), **4 MB de flash**, **pas de PSRAM**.
  C'est la contrainte la plus structurante : gros framebuffers et LVGL lourd sont exclus
  sans le mod PSRAM.
- **`TFT_RST` est câblé sur la ligne RST/EN de l'ESP32**, pas sur un GPIO — d'où `TFT_RST = -1`.
- **Backlight IO21** : grille d'un MOSFET N (AO3402), donc **actif haut**, et PWM/LEDC possible.
- **LDR sur IO34** : monté en parallèle de la branche basse d'un pont 1 MΩ/1 MΩ →
  **plus il y a de lumière, plus la valeur ADC baisse**.
- **Haut-parleur** : ampli SC8002B, sortie **BTL** sur P4 (JST PH 2.0 mm) —
  ⚠️ ne jamais relier une borne du HP à la masse. IO26 = DAC2 (IO25/DAC1 est pris par le tactile).
- **Deux régulateurs AMS1117-3.3 séparés** : un pour l'ESP32, un pour le TFT.
- **GPIO réellement libres : IO22, IO27, IO35** (ce dernier en entrée seule, sans pull-up).
  Soit trois seulement — plus IO21 si on sacrifie le rétroéclairage.
- **Connecteurs** : P1 = VIN/TX(IO1)/RX(IO3)/GND · P3 = IO21/IO22/IO35/GND · CN1 = 3V3/IO27/…/GND
  (`PINS.md` documente CN1 comme le connecteur I²C, SDA=27 SCL=22 ; le schéma v0.1 laisse la
  broche 3 non connectée — à vérifier à l'ohmmètre avant de s'appuyer dessus).
- **Suffixe de référence** : `R` = tactile **R**ésistif (XPT2046, la vraie CYD).
  `C` = tactile **C**apacitif (GT911, I²C, brochage différent).

### Divergence sur la révision 2 USB

`DisplayConfig/CYD2USB/User_Setup.h` déclare `ST7789_DRIVER`, alors que plusieurs sources
décrivent la dalle comme un ILI9341 dont les couleurs sont simplement inversées. Les deux
approches fonctionnent en pratique ; on suit le fichier du dépôt (env `cyd2usb`), l'alternative
étant de rester sur `ILI9341_2_DRIVER` et d'appeler `tft.invertDisplay(1)`.

Cette révision a aussi un **défaut de gamma**, corrigeable par :

```cpp
tft.writecommand(ILI9341_GAMMASET); tft.writedata(2); delay(120);
tft.writecommand(ILI9341_GAMMASET); tft.writedata(1);
```

Et un **défaut de conception sur son port USB-C** : pas de résistances CC 5.1 k, donc
**inopérant avec un câble C↔C**. Passer par le micro-USB, ou un câble/adaptateur C→A.
