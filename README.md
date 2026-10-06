# GlowScreen32 — écrans tactiles ESP32 pilotés par Jeedom

GlowScreen32 transforme des cartes **ESP32-2432S028 « Cheap Yellow Display » (CYD)** en
télécommandes murales pour Jeedom. Chaque écran affiche des boutons et des tuiles configurés
dans Jeedom et déclenche des commandes ou des scénarios domotiques. Le projet est conçu pour
**un parc d'écrans** : le même binaire tourne sur toutes les cartes, et chaque carte se
reconnaît à sa propre adresse MAC.

## Les deux moitiés du projet

| | Où | Quoi |
|---|---|---|
| **Firmware** | **ce dépôt** | PlatformIO, Arduino, TFT_eSPI, ArduinoJson 7 |
| **Plugin Jeedom** `glowscreen32` | un **autre dépôt**, côté serveur Jeedom | équipements (eqLogic), endpoint HTTP `plugins/glowscreen32/core/php/api.php` |

**[`docs/api-contract.md`](docs/api-contract.md) (v3.0) est la pièce maîtresse.** Toute
évolution se décide là d'abord, puis se répercute des deux côtés. Une moitié ne diverge
jamais du contrat en silence.

---

## Matériel et contraintes

Carte validée le 2026-09-22 :

| Élément | Résultat |
|---|---|
| Puce | ESP32-D0WD-V3 rev 3.1, **4 Mo de flash, aucune PSRAM**, quartz 40 MHz |
| USB-série | CH340 (`1a86:7523`), port `/dev/cu.usbserial-XXXX` |
| Pilote macOS | Apple natif `AppleUSBCHCOM`, reset automatique RTS/DTR fonctionnel |
| Écran | ILI9341 320×240 (env `cyd`) ; révision USB-C en ST7789 (env `cyd2usb`) |
| Tactile | XPT2046 |
| LED RVB | 3 canaux, actif bas (sert à la commande `identify`) |
| LDR | lit 0 en lumière ambiante (normal : pont inversé) |

À garder en tête :

- **4 Mo de flash, aucune PSRAM** : pas de gros tampons, pas de framebuffer plein écran.
- **3 GPIO libres seulement** : IO22, IO27, IO35 (entrée seule).
- **IO21 = rétroéclairage, piloté par le firmware.** Un écran noir quand le firmware ne
  tourne pas (flash corrompu, démarrage en échec) est normal : la carte n'est pas morte.
- **Câble USB de données obligatoire.** Un câble de charge seule ne produit strictement aucun
  événement sur le bus, pas même un périphérique inconnu. C'est le piège qui a coûté le plus
  de temps. Sur la révision USB-C, un câble C↔C ne fonctionne pas : prendre un C→A.
- **Alimentation directe sur l'ordinateur.** Sur un hub USB passif partagé, le flash a déjà
  échoué en cours d'écriture faute de courant.
- `upload_speed` plafonné à **460800 bauds** : au-delà, le pilote Apple CH34x devient
  instable. Repli à 115200 : env `cyd_slow`.
- **Ne jamais installer le pilote WCH** sur macOS : il casse RTS/DTR sur Apple Silicon (voir
  [`docs/setup-macos.md`](docs/setup-macos.md)).

Le port `/dev/cu.usbserial-XXXX` dépend du **port USB physique** utilisé : il change si l'on
rebranche ailleurs. `./scripts/detect-cyd.sh` le retrouve.

---

## Premier démarrage (développeur)

### Prérequis

PlatformIO Core (`brew install platformio`) ; la plateforme `espressif32` et les
bibliothèques sont installées par `pio` au premier build. Détail et dépannage :
[`docs/setup-macos.md`](docs/setup-macos.md).

### Valeurs d'usine : `src/secrets.h`

```bash
cp src/secrets.h.example src/secrets.h
```

Y renseigner :

| Constante | Contenu | Exemple |
|---|---|---|
| `WIFI_SSID`, `WIFI_PASS` | réseau Wi-Fi | `"MonReseau"`, `"motdepasse"` |
| `JEEDOM_HOST` | IP ou nom du serveur Jeedom, **sans** `http://` ni chemin | `"192.168.1.10"` |
| `JEEDOM_APIKEY` | clé API du plugin (Jeedom → Réglages → Système → Configuration → onglet API, ligne GlowScreen32) | — |

**Ce ne sont que des valeurs d'usine.** La configuration réelle d'une carte vit en **NVS**
(espace `glowcfg`, `src/store/device_config.*`). Au démarrage, toute valeur absente ou
illisible en NVS reçoit sa valeur d'usine, qui y est **écrite** — et ne sert plus ensuite.
Concrètement, `secrets.h` ne compte que pour une **carte neuve** ou une carte dont la NVS a
été effacée. Changer `secrets.h` puis reflasher **ne modifie pas** une carte déjà
configurée : pour cela, voir [Mettre son Wi-Fi](#mettre-son-wi-fi-et-ladresse-de-jeedom).

Tant que la clé vaut sa valeur de gabarit (ou fait moins de 8 caractères), le firmware
n'émet aucune requête et le bandeau affiche « Sans clé ».

> ⚠️ **Ces valeurs d'usine sont compilées dans chaque binaire**, y compris ceux publiés par
> OTA et déposés sur le serveur Jeedom. Quiconque récupère un `firmware.bin` peut y lire le
> mot de passe Wi-Fi et la clé API d'usine. Ne jamais diffuser un binaire hors du foyer.

Aucune identité d'écran n'est à saisir : la carte se reconnaît à sa MAC.

### Compiler, flasher, observer

```bash
./scripts/detect-cyd.sh        # identifier la carte, son port et sa puce
pio run -e cyd                 # compiler (CYD 1 port micro-USB, ILI9341)
pio run -e cyd2usb             # compiler (révision micro-USB + USB-C, ST7789)
pio run -e cyd -t upload       # flasher
pio run -e cyd_slow -t upload  # repli à 115200 bauds (câble ou alimentation médiocres)
pio run -e calib -t upload     # outil de calibration du tactile, autonome (banc)
```

Le moniteur série de PlatformIO exige un terminal interactif ; en script, passer par
pyserial (115200 bauds) :

```bash
/opt/homebrew/Cellar/platformio/6.2.0/libexec/bin/python -c "
import serial; s=serial.Serial('/dev/cu.usbserial-XXXX',115200,timeout=1)
while True:
    l=s.readline()
    if l: print(l.decode('utf-8','replace'), end='')
"
```

Ni le mot de passe Wi-Fi ni la clé API ne sont écrits sur le port série.

### Piège : reflasher en USB après un OTA

`pio run -t upload` écrit dans `app0` mais ne touche pas à `otadata`. Une carte passée sur
`app1` par OTA continuera d'amorcer `app1` : le flash n'a **aucun effet visible**. Effacer
d'abord `otadata` :

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --port /dev/cu.usbserial-XXXX \
    erase_region 0xe000 0x2000
pio run -e cyd -t upload
```

`pio run -e cyd -t erase` efface tout, **y compris la NVS** : la carte repart alors sur les
valeurs d'usine de `secrets.h` et sa calibration d'usine. Détail : [`docs/ota.md`](docs/ota.md),
« Revenir en arrière à la main ».

---

## Mettre son Wi-Fi (et l'adresse de Jeedom)

Identifiants Wi-Fi, hôte Jeedom et clé API se changent **sans recompiler ni brancher de
câble**. Trois chemins.

### Quelle méthode quand

| Situation | Méthode |
|---|---|
| L'écran n'a plus de Wi-Fi (box remplacée, mot de passe changé, déménagement) | **A. Portail de secours** — automatique après 5 min |
| L'écran fonctionne, on veut changer de réseau, d'hôte Jeedom ou de clé, sur place | **A. Portail**, ouvert depuis le menu |
| L'écran fonctionne et le nouveau réseau est **déjà joignable** depuis l'écran | **B. Depuis Jeedom**, sans se déplacer |
| Carte neuve, ou NVS effacée | **C. Au flash**, par `secrets.h` |

Dans tous les cas, **rien n'est enregistré avant d'avoir été éprouvé** : la carte essaie
les nouveaux réglages **60 s au plus** (association Wi-Fi **et** réponse de l'API Jeedom).
En cas d'échec, elle revient d'elle-même aux anciens réglages et n'écrit rien.

### A. Portail de secours (sur place, avec un téléphone)

1. **Ouvrir le portail**, de l'une des deux façons :
   - **automatiquement** : sans Wi-Fi depuis **5 minutes**, l'écran ouvre seul le portail
     (seulement depuis l'écran d'accueil, jamais au milieu d'un menu ou d'une calibration) ;
   - **depuis le menu** : **appui maintenu 5 s, doigt immobile, sur le bandeau du haut**
     (sur l'écran « Écran non déclaré » : n'importe où) → écran **Réglages** → bouton
     **Wi-Fi**.
2. L'écran **Portail Wi-Fi** affiche :
   - « Rejoindre le réseau » **`GlowScreen-xxxxxx`** (les 6 derniers caractères de la MAC,
     en minuscules, par exemple `GlowScreen-123456`) ;
   - un **mot de passe** de 8 caractères, tiré au hasard à chaque ouverture et affiché
     **uniquement à l'écran** (ni journal série, ni page web) : il faut être devant ;
   - « puis ouvrir http://… » : l'adresse de la page, celle du point d'accès de la carte
     (`http://192.168.4.1`, adresse par défaut du point d'accès ESP32).
3. Sur le téléphone, **rejoindre `GlowScreen-xxxxxx`** avec ce mot de passe. La page s'ouvre
   en général d'elle-même (portail captif : toute adresse ramène au formulaire) ; sinon,
   ouvrir l'adresse affichée.
4. **Remplir le formulaire** :

   | Champ | Règle |
   |---|---|
   | **Réseau Wi-Fi** | liste des réseaux captés (les plus forts, sans doublon) ; réseau actuel présélectionné ; « Actualiser la liste » relance un balayage |
   | « ou saisir un autre nom » | prioritaire sur la liste (réseau masqué, par exemple) ; 32 octets au plus |
   | **Mot de passe Wi-Fi** | 8 à 63 octets, ou 64 chiffres hexadécimaux. **Vide** : inchangé si c'est le même réseau, **aucun** (réseau ouvert) pour un autre réseau |
   | **Hôte Jeedom** | prérempli ; IP ou nom, sans `http://` |
   | **Clé API du plugin** | **Vide** : inchangée. **Obligatoire si l'hôte change** |

   Le mot de passe Wi-Fi et la clé API en service ne sont **jamais réaffichés** dans la page.
5. **« Essayer et enregistrer »**. La page répond « Essai en cours (60 s au plus) ». Le point
   d'accès peut décrocher pendant l'essai : se reconnecter puis recharger la page.
6. **Issue**, affichée sur l'écran et en tête de la page :
   - **Succès** : « Réussi et enregistré : redémarrage » — Wi-Fi, hôte et clé sont écrits en
     NVS et l'écran redémarre ~2,5 s plus tard pour les appliquer. Variante, avec le même
     redémarrage : « Enregistré ; écran pas encore déclaré dans Jeedom » (la chaîne
     fonctionne, il reste à [enrôler l'écran](#enrôler-un-écran-neuf)).
   - **Échec** (anciens réglages rétablis, rien d'écrit, le mot de passe du portail reste
     affiché pour corriger) :
     - « Échec : Wi-Fi non associé (réseau ou mot de passe) »
     - « Échec : clé API refusée par Jeedom »
     - « Échec : clé API absente ou invalide »
     - « Échec : Jeedom répond en erreur »
     - « Échec : Jeedom injoignable à cet hôte »
   - Saisie refusée d'emblée (rien n'est essayé) : nom de réseau absent ou trop long, mot de
     passe Wi-Fi invalide, hôte invalide, clé invalide (8 à 99 caractères sans espace),
     « L'hôte change : ressaisir la clé API du plugin », « Aucune clé API valide en service :
     la saisir ». Pendant une mise à jour OTA ou un autre essai : « Refusé : un autre essai ou
     une mise à jour est en cours. »

> **Pourquoi la clé est à ressaisir quand l'hôte change.** Sinon, n'importe qui associé au
> portail pourrait indiquer l'adresse de son propre serveur et y recevoir la clé en service
> dès la première requête.

**Fermeture du portail** : un portail **automatique** se referme dès que la carte retrouve
son réseau habituel (elle continue de le chercher pendant tout ce temps). Un portail ouvert
**depuis le menu** reste ouvert jusqu'au bouton **« Fermer le portail »**, ou se referme seul
après **20 minutes sans appareil connecté**. Toucher le bandeau (flèche) masque l'écran du
portail sans le fermer ; tant que le Wi-Fi manque, le bandeau de la grille affiche alors
« Portail Wi-Fi » (au lieu de « Hors ligne »), et le bouton **Wi-Fi** du menu devient
**Portail** (il rouvre l'écran du portail).

### B. Depuis Jeedom (l'écran est encore joignable)

Depuis la **page de l'équipement** de l'écran dans Jeedom, l'action d'envoi des
identifiants Wi-Fi (réservée à l'administrateur) met en file la commande `wifi` (`ssid` ≤ 32,
`pass` ≤ 64). Elle n'existe **pas** comme commande Jeedom utilisable par un scénario. La
carte la reçoit à son `ping` suivant, puis :

1. quitte son réseau et essaie les nouveaux identifiants, **hôte et clé inchangés** ;
2. en cas de succès (associée **et** Jeedom a répondu dans les 60 s), écrit le Wi-Fi en NVS
   — sans redémarrer ;
3. sinon, revient d'elle-même à ses anciens identifiants.

La commande est ignorée pendant une mise à jour OTA ; une commande en file expire après
10 minutes (écran éteint, par exemple).

> ⚠️ **L'essai exige que le nouveau réseau réponde pendant ces 60 s.** On ne peut donc pas
> « préparer » un changement de mot de passe de la box en envoyant les nouveaux identifiants
> avant de les appliquer : la carte échouerait à s'associer et reviendrait aux anciens.
> Cette méthode convient quand le nouveau réseau existe déjà à côté de l'ancien (nouvelle
> box, nouveau SSID, période de transition), et qu'il permet de joindre Jeedom. Pour un
> simple changement de mot de passe sur le même SSID : changer la box, puis passer par le
> **portail de secours** (A), qui s'ouvre seul au bout de 5 minutes.

Le mot de passe transite en clair sur le réseau local (HTTP), comme la clé API : accepté
pour un parc domestique.

### C. Au flash, par `secrets.h` (carte neuve ou NVS effacée)

Renseigner `src/secrets.h` (voir [Premier démarrage](#premier-démarrage-développeur)) puis
flasher. Ces valeurs ne sont appliquées que si la NVS ne contient rien de valide : une carte
déjà configurée **les ignore**. Pour les imposer à une telle carte, effacer d'abord la NVS
(`pio run -e cyd -t erase`), ce qui efface aussi la calibration et le cache de mise en page.

> Après une mise à jour OTA, tant que le nouveau firmware n'est pas validé (2 à 10
> minutes), tout redémarrage — y compris celui qui suit un enregistrement par le portail —
> ramène la version précédente. Laisser l'écran se valider avant de le reconfigurer.

---

## Enrôler un écran neuf

1. Flasher le binaire commun, donner le Wi-Fi (`secrets.h` ou portail).
2. Une carte inconnue de Jeedom reçoit `unknown_device` et affiche **« Écran non déclaré —
   Créez l'équipement dans Jeedom »** avec sa **MAC en grand** (`24:6f:28:12:34:56`), la forme
   compacte en dessous (`246f28123456`), le réseau associé et la version du firmware.
3. Dans Jeedom : **Plugins → Organisation → GlowScreen32 → Ajouter**, nommer l'écran d'après
   la pièce (le nom s'affiche dans le bandeau), saisir la **MAC** (avec ou sans séparateurs,
   casse indifférente — un doublon est refusé), choisir les boutons, **Sauvegarder**.
4. La carte interroge le plugin toutes les 10 s et passe seule en mode normal dès qu'elle
   est reconnue, **sans redémarrage ni câble série**.

---

## Calibration du tactile

Les bornes du tactile vivent elles aussi en NVS. Pour recalibrer : **menu Réglages →
Calibrer** (ou commande `calibrate` depuis Jeedom). Quatre cibles dans les coins puis une
croix de contrôle au centre ; abandon après 60 s sans toucher (15 s pour la croix), et en
cas d'abandon ou de relevés incohérents, les anciennes valeurs sont conservées. L'outil de banc `pio run -e
calib` ne sert qu'à ajuster les valeurs **d'usine** de `src/ui/touch_calib.h`.

Le menu **Réglages** affiche aussi MAC, IP, Wi-Fi et RSSI, hôte et état de l'API, firmware et
partition, durée de service, mémoire ; le bouton **Redémarrer** demande deux appuis
(« Confirmer ? »). Le menu se referme seul après 60 s sans toucher.

---

## Mises à jour par le réseau (OTA)

Les écrans **tirent** : chacun demande au plugin s'il existe un firmware plus récent (une
première fois environ une minute après le démarrage, puis toutes les heures, ou
immédiatement sur la commande `ota`). En bref :

1. incrémenter `GLOWSCREEN_FW_VERSION` dans `src/fw_version.h` (indispensable, sinon le
   plugin ne voit pas la différence) ;
2. `pio run -e cyd` → `.pio/build/cyd/firmware.bin` ;
3. le déposer sur la page du plugin Jeedom (un binaire sans marqueur `GLOWSCREEN32-FW:` est
   refusé — l'outil `calib` ne peut donc jamais partir) ;
4. ouvrir le **verrou global** puis le **verrou d'un seul écran témoin** ; ouvrir les autres
   une fois le témoin revenu avec sa nouvelle version ; refermer les verrous ensuite.

**Retour arrière automatique** : un firmware fraîchement installé n'est adopté qu'après six
preuves — écran initialisé, Wi-Fi associé, API ayant répondu, `ping` réussi depuis la tâche
ping (en attente longue si le plugin l'annonce), réponse valide au contrôle firmware, 120 s de
fonctionnement continu. Sur un écran non déclaré, le ping et le contrôle firmware sont
remplacés par deux réponses `unknown_device` espacées d'au moins une minute. Faute de quoi,
au bout de 10 minutes, la carte rebascule seule sur la version précédente et ne réinstalle
plus la version rejetée. Tant que le firmware n'est pas validé, la commande `reboot` venue de
Jeedom est refusée. Procédure complète :
[`docs/ota.md`](docs/ota.md).

---

## Arborescence

```
platformio.ini                 envs cyd, cyd2usb, cyd_slow, calib ; config TFT_eSPI en build_flags
src/main.cpp                   tâche d'affichage : machine à états, menus, portail, calibration, OTA
src/cyd_pins.h                 brochage complet de la carte
src/fw_version.h               GLOWSCREEN_FW_VERSION (à incrémenter à chaque binaire publié)
src/fw_version.cpp             marqueur GLOWSCREEN32-FW:<version> gravé dans le .bin
src/secrets.h.example          gabarit des valeurs d'usine (copier en src/secrets.h)
src/model/layout.h             structures de la mise en page, taille fixe, sans allocation
src/net/wifi_mgr.{h,cpp}       Wi-Fi non bloquant, reconnexion, essai d'identifiants
src/net/jeedom_client.{h,cpp}  client HTTP du contrat (layout, press, ping, firmware)
src/net/net_tasks.{h,cpp}      tâches réseau et ping, files, attente longue
src/net/portal.{h,cpp}         portail Wi-Fi de secours : point d'accès, DNS captif, page web
src/store/device_config.{h,cpp}  Wi-Fi, hôte, clé API, calibration en NVS
src/store/layout_store.{h,cpp}   cache NVS de la dernière mise en page
src/ota/ota_updater.{h,cpp}    mise à jour par le réseau + retour arrière automatique
src/ui/button_grid.{h,cpp}     bandeaux, grille paginée, tuiles, écran d'enrôlement, OTA
src/ui/screens.cpp             écrans pleins : Réglages, portail, calibration, message, identification
src/ui/ui_internal.h           outils de tracé partagés
src/ui/theme.h                 palette, géométrie, gestes, rétroéclairage
src/ui/icons.{h,cpp}           pictogrammes (générés)
src/ui/fonts/fonts.h           déclaration des polices
src/ui/fonts/font_ui_{16,20,30}.cpp  polices VLW accentuées (générées, ne pas éditer)
src/ui/touch_calib.h           bornes tactiles d'usine et calcul de calibration
src/tools/touch_calib.cpp      outil de calibration autonome (env calib)
tools/assets/                  génération des polices et icônes (gen_vlw, gen_icons, check_vlw, roundtrip)
scripts/detect-cyd.sh          détection carte, port, puce
docs/                          documentation (ci-dessous)
```

## Documentation

| Fichier | Contenu |
|---|---|
| [`docs/api-contract.md`](docs/api-contract.md) | **le contrat d'API, à lire en premier** |
| [`docs/firmware.md`](docs/firmware.md) | architecture du firmware |
| [`docs/ota.md`](docs/ota.md) | publier et recevoir un firmware par le réseau |
| [`docs/plugin-jeedom.md`](docs/plugin-jeedom.md) | architecture du plugin Jeedom |
| [`docs/jeedom-reference.md`](docs/jeedom-reference.md) | référence Jeedom |
| [`docs/analyse-cyd.md`](docs/analyse-cyd.md) | analyse du projet CYD amont + schéma Sunton |
| [`docs/setup-macos.md`](docs/setup-macos.md) | toolchain, pilotes, dépannage macOS |
| [`docs/ETAT-DU-PROJET.md`](docs/ETAT-DU-PROJET.md) | où en est le projet, ce qui reste |
| [`tools/assets/README.md`](tools/assets/README.md) | régénérer polices et icônes |

## Ce qui n'est pas versionné

| Chemin | Pourquoi |
|---|---|
| `src/secrets.h` | identifiants Wi-Fi et clé API d'usine : seul le gabarit `secrets.h.example` est versionné |
| `*.local.md` | notes d'accès locales (serveurs, mots de passe) : elles restent sur la machine |
| `local/` | détails de l'installation réelle (accès, correspondance avec les exemples de la doc) |
| `*.bin`, `*.elf` | binaires : ils embarquent les valeurs d'usine de `secrets.h` en clair |
| `.pio/` | sorties de compilation et bibliothèques téléchargées, régénérées par `pio` |
| `.vscode/`, `__pycache__/`, `.DS_Store` | fichiers d'éditeur, de Python ou de macOS |

Les boutons déclenchent du **matériel réel** : lors des essais, n'actionner qu'un équipement
sans conséquence.

## Licence et remerciements

Le code de ce dépôt est publié sous licence **MIT** (voir [`LICENSE`](LICENSE)).

- Polices **Inter** (© The Inter Project Authors), converties en VLW : licence **SIL Open Font
  License 1.1**, texte complet dans [`src/ui/fonts/OFL.txt`](src/ui/fonts/OFL.txt).
- Le travail de départ sur la carte s'appuie sur
  [witnessmenow/ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)
  (MIT) — aucun code n'en est recopié.
- Bibliothèques téléchargées à la compilation, non incluses : TFT_eSPI (Bodmer), ArduinoJson
  (Benoît Blanchon, MIT), XPT2046_Touchscreen (Paul Stoffregen), cœur Arduino pour ESP32
  (Espressif).
- `docs/jeedom-reference.md` cite de courts extraits du cœur de Jeedom (GPL), à titre de
  référence.
