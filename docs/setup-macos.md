# Setup macOS — ESP32-2432S028 (CYD)

Machine : macOS 26.6.2, Apple Silicon (arm64).

## Installé

| Outil | Version | Via |
|---|---|---|
| PlatformIO Core | 6.2.0 | `brew install platformio` |
| plateforme `espressif32` | 7.1.3 (Arduino core 3.x) | `pio pkg install -g -p espressif32` |
| arduino-cli | 1.5.1 | `brew install arduino-cli` |
| core `esp32:esp32` | 3.3.12 | `arduino-cli core install esp32:esp32` |
| TFT_eSPI | 2.5.43 | `lib_deps` du projet |
| XPT2046_Touchscreen | v1.4 (git) | `lib_deps` |
| ArduinoJson | 7.x | `lib_deps` |

arduino-cli est configuré avec l'index Espressif et sert de solution de repli pour compiler
les exemples du dépôt CYD tels quels ; le travail se fait sous PlatformIO.

## Pilote USB-série : rien à installer, et surtout rien de tiers

D'après le schéma officiel Sunton v0.1, **les deux révisions de la CYD portent un CH340C**
(U6, SOP-16, près des connecteurs USB). Le CP2102 qu'on voit parfois évoqué concerne d'autres
cartes ESP32 génériques, pas celle-ci.

macOS 26 embarque nativement le pilote correspondant — vérifié sur cette machine dans
`/System/Library/DriverExtensions/` :

- `com.apple.DriverKit-AppleUSBCHCOM.dext` → CH340/CH34x (`1a86:7523`, et `1a86:55d4` pour le CH9102)
- `com.apple.DriverKit-AppleUSBSLCOM.dext` → CP210x (`10c4:ea60`), au cas où

⚠️ **Ne pas installer le pilote WCH `CH34xVCPDriver`.** Sur Apple Silicon il ne gère pas
RTS/DTR en runtime (`Inappropriate ioctl for device`, issue WCH #50 toujours ouverte) : le
reset automatique d'esptool cesse de fonctionner et il faut manipuler BOOT/RST à la main.
Installé en parallèle du pilote Apple, il crée en plus des ports fantômes en conflit
(`cu.wchusbserial*` et `cu.usbserial-*` simultanément).

Port attendu dans `/dev` avec le pilote Apple : **`cu.usbserial-XXXX`** où XXXX est le
location ID — il change selon le port USB physique utilisé. Toujours utiliser `cu.` et jamais
`tty.`.

## Débit

Le pilote Apple CH34x devient instable au-delà de **460800 bauds** : c'est la valeur
`upload_speed` retenue dans `platformio.ini` (et non 921600). En cas d'échec, `env:cyd_slow`
retombe à 115200.

## Commandes

```bash
cd screenes32                # racine du dépôt

./scripts/detect-cyd.sh     # identifier la carte et son port
pio run -e cyd              # compiler (révision 1 port USB, ILI9341)
pio run -e cyd2usb          # compiler (révision USB-C, ST7789)
pio run -e cyd -t upload    # flasher
pio device monitor -b 115200
pio run -e cyd_slow -t upload   # repli à 115200 si l'upload à 460800 bauds échoue
```

## Si le port n'apparaît pas au branchement

1. Câble USB de **données**, pas un câble de charge seule — c'est la cause n°1.
2. Essayer un autre port / sans hub.
3. Sur la révision USB-C : un câble C↔C ne fonctionne pas, utiliser C→A.
4. En dernier recours, maintenir BOOT (IO0) pendant le branchement pour forcer le bootloader.
