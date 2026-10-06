# Architecture du firmware GlowScreen32

Firmware de la carte ESP32-2432S028 (« Cheap Yellow Display »). Il affiche une **grille
paginée** de boutons décrite par le plugin Jeedom `glowscreen32` et notifie les appuis.

Le contrat d'échange est figé dans [`api-contract.md`](api-contract.md) (**v3.0**) : ce
document ne décrit que le **côté carte**. Version du binaire : `GLOWSCREEN_FW_VERSION`
= **2.3.1** (`src/fw_version.h:14`).

> **Corrections 2.3.1 (revue complète)** — détail dans « Corrections 2.3.1 » plus bas :
> échéances robustes au-delà de 24,8 jours, aucun `press` sur une mise en page sue périmée,
> tâches réseau déplacées sur le cœur 1, HTTP/1.0, validation ENROLL et preuve 5 élargie,
> `reboot` refusé avant validation, version visée notée après `Update.end()`, portail durci,
> calibration contrôlée au centre, tuiles `view` lisibles en 3×3, tampons en caractères
> (cache NVS v05).

> **Nouveautés 2.3.0 (schéma 3)** — tuiles `view` (valeur + ton, sans action), `values` au
> `ping`, écran en lecture seule (`ui.readonly`, erreur `read_only`), cache NVS v04 (v05 en
> 2.3.1). Détail dans « Schéma 3 » plus bas. En service sur la carte, OTA validé.

> **Nouveautés 2.2.0** — résumées ici, détaillées dans les sections « Tâches », « Configuration
> locale » et « Attente longue » plus bas : réseau sorti de la tâche d'affichage (trois
> tâches, double tampon `Layout`), attente longue du `ping` (`wait`/`rev`), commandes à
> distance (`cmd`), diagnostics au `ping`, Wi-Fi / Jeedom / calibration en **NVS**, menu
> local (appui 5 s sur le bandeau), portail Wi-Fi de secours, calibration tactile à
> l'écran. **Rien de tout cela n'a encore tourné sur la carte** — voir « Points ouverts ».

## Un seul binaire pour tout le parc

Rien n'identifie un écran en particulier dans le firmware. Au démarrage la carte lit sa
propre MAC (`WiFi.macAddress()`), la normalise en minuscules sans séparateur
(`246f28123456`) et s'en sert comme paramètre `device`. Le nom affiché dans le bandeau vient
du champ `name` renvoyé par `layout`, donc de Jeedom. On flashe le même `firmware.bin`
partout ; seule la déclaration dans Jeedom diffère.

`WifiMgr::deviceId()` est la **seule** source d'identité. Aucune MAC n'apparaît dans
`secrets.h` ni dans une constante — uniquement en commentaire, à titre d'exemple.

Depuis la v2.2, la configuration propre à une carte — Wi-Fi, hôte Jeedom, clé API,
**calibration tactile** — vit en **NVS** : `secrets.h` et `touch_calib.h` ne fournissent plus
que les **valeurs d'usine** du premier démarrage. La dernière exception au « binaire unique »
(la calibration compilée) a disparu. Voir « Configuration locale ».

## Vue d'ensemble

```
      ┌───────────────────────────────────────────────────────┐
Wi-Fi ┤ net/wifi_mgr        connexion non bloquante, MAC, RSSI │
      └──────────────────────────┬────────────────────────────┘
                                 │ état
      ┌───────────────────────────────────────────────────────┐
HTTP ─┤ net/jeedom_client   layout / press / ping / firmware   │
      │                     négociation de schéma, flux borné  │
      └──┬───────────────┬──────────────┬─────────────────────┬┘
         │ Layout        │ BannerInfo   │ FirmwareInfo        │
  ┌──────▼──────────┐    │      ┌───────▼──────────┐          │
  │ store/layout_   │    │      │ ota/ota_updater  │          │
  │ store — NVS     │    │      │ SHA-256 + repli  │          │
  └──────┬──────────┘    │      └───────┬──────────┘          │
         │               │              │                     │
         └───────────────┴──────┬───────┴─────────────────────┘
                                ▼
               ┌──────────────────────────────────┐
               │ ui/button_grid                   │
               │ bandeau haut, grille paginée,    │
               │ bandeau bas (retour + pastilles),│
               │ enrôlement, écrans OTA           │
               │   ├─ ui/theme.h   géométrie      │
               │   ├─ ui/fonts/    3 polices VLW  │
               │   └─ ui/icons     40 pictogrammes│
               └──────────────┬───────────────────┘
                              │ hitTest
               ┌──────────────▼───────────────┐
               │ ui/touch_calib               │
               │ brut XPT2046 -> pixels       │
               └──────────────────────────────┘
```

`main.cpp` orchestre le tout avec une machine à états ; aucun module n'en connaît un autre
en dehors du modèle partagé `model/layout.h`. Depuis la v2.2, les appels HTTP de
`jeedom_client` et `ota_updater` ne s'exécutent plus dans `main.cpp` mais dans les tâches de
`net/net_tasks` — voir « Tâches et files ».

## Fichiers

| Fichier | Rôle |
|---|---|
| `src/main.cpp` | **tâche d'affichage** : machine à états BOOT → WIFI → LAYOUT → RUN, modes d'écran, gestes tactiles, rétroéclairage, commandes à distance, calibration — **aucun appel HTTP** |
| `src/net/net_tasks.{h,cpp}` | tâches `net` et `ping`, files de demandes et de résultats, double tampon `Layout`, attente longue (v2.2) |
| `src/net/portal.{h,cpp}` | portail Wi-Fi de secours : point d'accès, DNS captif, page web (v2.2) |
| `src/store/device_config.{h,cpp}` | Wi-Fi, hôte, clé API, calibration en NVS (v2.2) |
| `src/ui/screens.cpp` | écrans pleins v2.2 : Réglages, portail, calibration, message, identification |
| `src/ui/ui_internal.h` | outils de tracé partagés par `button_grid.cpp` et `screens.cpp` |
| `src/secrets.h` | SSID, mot de passe, hôte et clé API **d'usine** (1er démarrage) — **non versionné** |
| `src/secrets.h.example` | gabarit à copier en `secrets.h` |
| `src/model/layout.h` | `struct Layout` / `LayoutPage` / `LayoutButton`, taille fixe, sans allocation |
| `src/net/wifi_mgr.{h,cpp}` | association Wi-Fi non bloquante + reconnexion à backoff |
| `src/net/jeedom_client.{h,cpp}` | les quatre actions du contrat, négociation de schéma, parsing ArduinoJson 7 |
| `src/store/layout_store.{h,cpp}` | cache NVS de la dernière mise en page |
| `src/ui/button_grid.{h,cpp}` | bandeaux + grille paginée + sprite par tuile + écrans pleins |
| `src/ui/theme.h` | **charte et géométrie** : palette, tailles de tuile, gestes, rétroéclairage |
| `src/ui/icons.{h,cpp}` | 40 identifiants d'icônes, 39 bitmaps 24×24 en 1 bit |
| `src/ui/fonts/fonts.h` | déclaration des trois polices VLW |
| `src/ui/fonts/font_ui_{16,20,30}.cpp` | **polices générées — ne jamais éditer à la main** |
| `src/ui/touch_calib.h` | bornes **d'usine** du tactile, calcul de calibration partagé, conversion brut → pixels |
| `src/tools/touch_calib.cpp` | outil de calibration autonome (env `calib`) |
| `src/ota/ota_updater.{h,cpp}` | téléchargement, vérification SHA-256, filet de sécurité |
| `src/fw_version.h` | `GLOWSCREEN_FW_VERSION` — **à incrémenter à chaque binaire publié** |
| `src/fw_version.cpp` | marqueur `GLOWSCREEN32-FW:<version>` gravé dans le `.bin` |
| `src/cyd_pins.h` | brochage de la carte (inchangé) |

---

# Modèle de données — les deux règles qui comptent

`struct Layout` (`src/model/layout.h:78`) est un bloc de taille fixe, sans aucune allocation
dynamique : la carte n'a pas de PSRAM et ce bloc est écrit tel quel en NVS.

Sa taille a été **multipliée par cinq** en v2.0 :

| | v1.4 | v2.0 |
|---|---|---|
| boutons | 6 | **32** |
| pages | — | **4** |
| `sizeof(Layout)` | 316 o | **1 608 o** |

> Mesure : `xtensa-esp32-elf-nm -S .pio/build/cyd/firmware.elf | grep ' layout$'` donne
> `00000648`, soit 1 608 octets. C'est aussi la taille du blob NVS, affichée au démarrage
> par `LayoutStore::begin()` (`src/store/layout_store.cpp:72`).

## Règle 1 — toute instance de `Layout` est statique ou globale

**La tâche Arduino n'a que 8 ko de pile.** Un `Layout` local en consomme 1 608 octets, soit
un cinquième, et le chemin de sauvegarde en empile naturellement plusieurs :

```
tryFetchLayout()            la mise en page fraîche      1 608 o
  └─ api.fetchLayout()         sous HTTPClient + WiFiClient, déjà gourmands
  └─ store.save(layout)
       └─ load(current)      le cache relu pour comparaison  1 608 o
```

Trois instances simultanées feraient près de 5 ko de pile **sous HTTPClient**, qui a déjà
consommé sa part. D'où :

- `s_layoutA` et `s_layoutB` sont des **statiques** de `main.cpp` (le double tampon, voir
  « Tâches et files ») ;
- `s_loadScratch` et `s_saveScratch` sont des **statiques de module**
  (`src/store/layout_store.cpp:23-24`) ;
- `JeedomClient::fetchLayout()` remplit `out` **en place**, sans copie locale
  (`src/net/jeedom_client.cpp`, commentaire en tête de la fonction).

En v1.4, avec 316 octets, la question ne se posait pas. C'est exactement le genre de
régression qui ne se voit pas à la compilation et qui se manifeste par un redémarrage
aléatoire sous charge réseau.

## Règle 2 — aucun champ volatil dans `Layout`

`info` (la valeur du bandeau) et l'heure changent **à chaque ping**, soit toutes les 30
secondes par défaut. `LayoutStore::save()` compare le contenu avant d'écrire : les mettre
dans `Layout` ferait réécrire la NVS deux mille fois par jour, pour une information que le
cache n'a même pas vocation à conserver.

Ils vivent donc dans `struct BannerInfo` (`src/net/jeedom_client.h`), qui ne transite que
jusqu'à `ButtonGrid` :

```c
struct BannerInfo {
  char     info[JEEDOM_INFO_LEN];  // chaîne déjà formatée par le plugin
  uint32_t time;                   // epoch UTC
  int32_t  tzoffset;               // secondes, DST comprise
  bool     hasTime;
};
```

Même raisonnement pour l'**état des boutons** : `LayoutButton::state` est présent dans la
structure (il faut bien le dessiner) mais **exclu de la comparaison** par
`sameExceptState()` (`src/store/layout_store.cpp:34`). Une lampe allumée depuis
l'application Jeedom ou un interrupteur mural ne doit pas user la flash.

## Champs

```c
struct LayoutButton {
  int32_t  id;        // rang GLOBAL, opaque (contrat v1.3, étendu v2.0)
  char     label[32]; // 24 caractères du contrat, en UTF-8 (« é » = 2 octets)
  uint16_t color;     // déjà converti en RGB565
  int8_t   state;     // -1 inconnu / 0 / 1 — VOLATIL
  uint8_t  mode;      // ButtonMode
  uint8_t  icon;      // IconId — résolu au parsing, plus une chaîne
  uint8_t  page;      // 0..3
  uint8_t  slot;      // 0..cols*rows-1
  uint8_t  target;    // page à ouvrir, UNIQUEMENT si mode == BTN_MODE_NAV
};
```

`LAYOUT_LABEL_LEN` vaut **49** (2.3.1 ; 32 avant) parce que le contrat borne en **caractères**, pas en
octets. `layoutCopyUtf8()` (`src/model/layout.h:108`) tronque sur une frontière de
caractère : un octet de continuation orphelin s'afficherait en parasite, TFT_eSPI décodant
l'UTF-8 dès qu'une police lissée est chargée.

### `id` est opaque, et global à l'écran

`LayoutButton::id` est le **rang du bouton sur l'ensemble des pages**, pas un rang par page
et surtout pas un identifiant de commande Jeedom. Conséquences tenues dans le code :

- **jamais affiché** à l'écran (seulement dans les traces série, préfixé `#`) ;
- **jamais trié**, jamais utilisé comme index de stockage ;
- comparé uniquement **par égalité**, pour détecter un changement de mise en page ;
- le cache NVS et `_pending[]` sont indexés par le **rang dans le tableau**, pas par `id`.

La seule exception est volontaire et vient du contrat : `states` est **indexé par l'`id`
global**, donc `applyStates()` y accède par `info.states[b.id]` après avoir vérifié les
bornes (`src/main.cpp`, fonction `applyStates`). S'appuyer sur l'`id` plutôt que sur le rang
de stockage rend la lecture insensible à un futur changement d'ordre des pages.

S'il manque dans la réponse, on retombe sur le rang — sans lui prêter davantage de sens.

### Les trois modes

| `mode` | Rendu | Réseau |
|---|---|---|
| `action` | tuile neutre, pas de pastille | `press` |
| `toggle` | **tuile entièrement colorée quand `state = 1`** | `press` |
| `nav` | chevron discret à droite | **aucun — jamais de `press`** |

Champ absent → `action`, le choix le plus prudent : pas de pastille d'état potentiellement
trompeuse. En schéma 1, un `nav` reçu est rabattu sur `action` (il n'y a pas de pages à
ouvrir), `src/net/jeedom_client.cpp`, `parseFlatButtons()`.

> **Ce qui a changé sur le rendu de `toggle`.** La v1.4 se contentait d'assombrir la couleur
> du bouton à 27 % quand il était éteint. Une nuance de gris ne se lit pas à trois mètres.
> En v2.0 c'est l'inverse : la tuile **éteinte est neutre** (`UI_SURFACE`) et la tuile
> **allumée prend toute sa couleur**. L'identité du bouton survit grâce à un **capuchon
> coloré de 3 px** en haut de la tuile, sinon une tuile éteinte serait grise et anonyme
> (`src/ui/button_grid.cpp:225-239`).

---

# Cache NVS

`Preferences`, espace de noms `glowscreen`, clés `layout` (blob de 1 608 octets) et `magic`.

```c
static const uint32_t STORE_MAGIC = 0x47533205;  // "GS2" + version 05
//   v02 : champ `mode` dans LayoutButton (contrat v1.3)
//   v03 : pages, grille, icône numérique, 32 boutons (contrat v2.0)
//   v04 : mode `view`, `readonly`, `id` du schéma 3 (contrat v3.0)
//   v05 : libellé, titre, nom sur 49 o (24 caractères accentués) — 2.3.1
```

v05 change la taille du blob (1 608 → **2 332 o**) : le contrôle `len == sizeof(Layout)`
suffirait, le magic suit quand même la règle.

⚠️ **v04 ne change pas la taille** (1 608 o : `readonly` tient dans le remplissage) : c'est
le **seul** `STORE_MAGIC` qui sépare les deux formats. Un cache v04 relu par une 2.2.0 après
retour arrière, ou un cache v03 relu par la 2.3.0, aurait des `id` d'un autre schéma : un
appui dans les secondes suivantes jouerait le bouton d'à côté. Chacun rejette le cache de
l'autre (« cache présent mais incompatible … ignoré ») et attend son premier `layout`.

**À incrémenter à chaque modification de `struct Layout`.** Le contrôle
`len == sizeof(Layout)` rattrape un changement de taille, mais **pas** un changement de
champs à taille constante — un cache v02 relu comme du v03 afficherait n'importe quoi.
Un cache incompatible est ignoré avec une trace explicite, pas en silence
(`src/store/layout_store.cpp:106-111`).

`load()` valide en plus les bornes du contrat avant d'accepter le blob (`count ≤ 32`,
`1 ≤ pageCount ≤ 4`, `3 ≤ cols ≤ 4`, `2 ≤ rows ≤ 3`) : un blob corrompu ne doit pas
produire un index hors tableau.

Le cache porte le champ `device`. À la relecture, `load()` compare cette MAC à celle de la
carte : **un cache appartenant à un autre écran est effacé, pas affiché** (carte reflashée,
NVS clonée).

`save()` relit d'abord le cache et n'écrit que si la **structure** diffère (voir
`sameExceptState`). Aucune écriture NVS n'a lieu sur un `press`, sur un `states` de `ping`,
ni sur une mise à jour du bandeau.

---

# Client Jeedom

- Point d'entrée : `http://<JEEDOM_HOST>/plugins/glowscreen32/core/php/api.php`
  (chemin **corrigé en v1.1** : la convention des plugins du serveur est `core/php/`).
- Authentification par **en-tête HTTP** `X-GLOWSCREEN32-APIKEY`, jamais en query string :
  une URL finit en clair dans les journaux d'Apache, à raison d'un appel toutes les 30 s.

## Négociation de schéma (v2.0)

La carte annonce ce qu'elle sait lire, à **chaque** requête
(`src/net/jeedom_client.cpp`, `request()`) :

```
X-GLOWSCREEN32-SCHEMA: 3      // LAYOUT_SCHEMA (src/model/layout.h) — 2 jusqu'en 2.2.x
```

⚠️ Sauf `press`, qui annonce le schéma **dans lequel son `id` a été reçu**
(`Layout::schema`) : voir « Schéma 3 ».

Un plugin v1.4 ignore cet en-tête et répond en schéma 1 — **que ce client sait toujours
lire**. C'est ce qui permet de déployer les deux moitiés dans n'importe quel ordre sans
immobiliser le parc.

Le schéma réellement servi est lu dans la réponse (`doc["schema"] | 1`), plafonné à
`LAYOUT_SCHEMA`, mémorisé dans `Layout::schema` et exposé par `serverSchema()`. Deux
lectures cohabitent :

| Réponse | Lecture | Effet |
|---|---|---|
| `pages` présent et non vide | `parsePages()` | pages, `slot`, `nav` |
| sinon | `parseFlatButtons()` | tout sur la page 0, `slot` = rang |

Défauts appliqués côté carte (`fetchLayout`) : grille **3×3**, mais **3×2 si `grid` est
absent et que le schéma vaut 1** — c'est la disposition historique, et une vieille réponse
ne doit pas changer d'allure sous prétexte qu'un firmware neuf la lit.

## Plafond de taille — sur les octets RÉELLEMENT LUS

```c
static const size_t JEEDOM_JSON_MAX = 8192;   // src/net/jeedom_client.cpp:24
```

> **Le garde-fou de la v1.4 ne gardait rien.** Il testait `http.getSize() > JEEDOM_JSON_MAX`,
> or `getSize()` vaut **−1** quand le serveur répond en `Transfer-Encoding: chunked` (pas de
> `Content-Length`). `-1 > 8192` est faux : la désérialisation devenait **non bornée**,
> précisément dans le cas où le plafond aurait servi.

En v2.0 le test sur `getSize()` subsiste, mais comme **chemin rapide uniquement** : quand
`Content-Length` est présent et trop grand, on refuse sans même ouvrir le flux. La vraie
borne est `BoundedStream` (`src/net/jeedom_client.cpp:40`), un `Stream` qui décompte les
octets lus et se comporte comme une fin de fichier au-delà de la limite : ArduinoJson
s'arrête et signale `IncompleteInput`, traduit en `ErrParse`. `exhausted()` permet de
distinguer dans le journal « réponse tronquée au plafond » de « JSON illisible ».

`BoundedStream` surcharge aussi `readBytes()` : ArduinoJson lit par blocs, et la version
héritée de `Stream` retomberait sur `timedRead()` → `read()`, un appel virtuel par octet.

Le plus gros `layout` légitime (4 pages, 32 boutons) pèse ~4,8 ko ; 8 ko laissent de la
marge sans menacer les 320 ko de RAM interne.

**`NestingLimit` passe de 6 à 8.** `{pages:[{buttons:[{…}]}]}` occupe 5 niveaux : la v1.4
n'avait plus qu'un cran de marge, on en garde trois.

## Timeouts et tentatives — par action

| Action | Timeout | Tentatives | Pire cas | Pourquoi |
|---|---|---|---|---|
| `layout` | 5 s | 3 | ≈ 15,5 s | rare, et on veut qu'il aboutisse |
| `press` | **2,5 s** | **1** | 2,5 s | l'utilisateur vient d'appuyer, il veut savoir vite ; s'acharner retient les appuis suivants dans la file |
| `ping` | 5 s (**`wait` + 5 s** en attente longue) | 1 | 5 s / 30 s | périodique, il est sa propre reprise |
| `firmware` | 5 s | 1 | 5 s | horaire, idem |

Constantes dans `jeedom_client.h` (`JEEDOM_TIMEOUT_MS`, `JEEDOM_ATTEMPTS`,
`JEEDOM_PRESS_TIMEOUT_MS`, `JEEDOM_PRESS_ATTEMPTS`, `JEEDOM_RETRY_DELAY_MS` = 250 ms).

Une **erreur applicative** (`ok:false` + champ `error`) n'est **jamais** réessayée : elle ne
se corrigera pas en 250 ms. Seuls les échecs de transport le sont. Chaque code `error` du
contrat a son `JeedomResult` et son libellé français court pour le bandeau.

Les 3 tentatives de `layout` sont enchaînées par la **tâche réseau**, pas par le client :
entre deux essais elle sert les `press` en attente. Depuis la v2.2, la tâche d'affichage
n'attend **jamais** le réseau, et l'appui est déjà confirmé à l'écran avant que la demande ne
parte.

## `press` : optimisme, puis vérité

1. le retour visuel est peint **au contact du doigt**, donc bien avant l'appel ;
2. l'état renvoyé est appliqué en **optimiste**, le bouton est marqué « en attente » — sa
   pastille est dessinée **en creux** (anneau, `src/ui/button_grid.cpp:248`) ;
3. **sans attente longue**, le ping suivant est avancé à 2 s (`PRESS_CONFIRM_DELAY_MS`, une
   simple demande `pingPoke(2000)`) ; **avec**, rien à faire : le ping retenu revient de
   lui-même dès que l'état change ;
4. la valeur du `states` suivant est **adoptée sans condition**, même si elle contredit
   l'optimisme.

Rien de tout cela n'est écrit en NVS. Un `layout` frais lève lui aussi tous les « en
attente » — filet de sécurité pour un plugin qui ne renverrait pas encore `states`.

## Rafraîchissement des états

`applyStates()` ne repeint que les pastilles qui ont réellement changé : **la grille n'est
pas redessinée**, et `drawButton()` sort immédiatement pour un bouton dont la `page` n'est
pas affichée — un bouton d'une autre page est mis à jour **dans le modèle**, sans le
moindre tracé.

- Taille de `states` ≠ nombre de boutons connus → tableau **ignoré**, `layout` complet forcé.
  C'est la règle qui a rattrapé la v1.2, conservée telle quelle.
- Plus de 32 entrées → tableau rejeté et journalisé côté client (`ping()`), en posant
  `stateCount = 33` pour que la comparaison de taille échoue à coup sûr.
- Champ `states` **absent** (plugin resté en v1.1) → repli : `layout` complet tous les
  `LAYOUT_RESYNC_PINGS` (10) pings. Ce compteur ne sert jamais face à un plugin à jour.
- Les boutons `nav` sont sautés : ils occupent une case de `states`, toujours à `null`.

### Cadence du `ping` — adaptative depuis la v2.1

| Situation | Intervalle |
|---|---|
| écran allumé | `poll` (30 s par défaut) |
| écran atténué | `poll × UI_IDLE_POLL_FACTOR`, soit **2 × poll** |
| **au réveil** | **immédiat** — `backlightWake()` appelle `pingPoke(0)`, une simple demande à la tâche ping |
| **attente longue** (v2.2) | relance **dès la réponse** (≥ 200 ms) — voir « Attente longue » |

Atténué, plus personne ne regarde : rafraîchir des pastilles que nul ne lit coûte une requête
PHP par écran, toutes les 30 secondes, toute la nuit. Le ping immédiat au réveil est la
**contrepartie indispensable** — sans lui, espacer reviendrait à échanger du trafic inutile
contre de l'information périmée au seul instant où quelqu'un la regarde.

⚠️ `UI_IDLE_POLL_FACTOR` (`theme.h`) a un **jumeau côté plugin**, `glowscreen32::IDLE_POLL_FACTOR`,
et le seuil « hors ligne » du plugin vaut `3 × ce facteur × poll`. Augmenter l'un sans l'autre
ferait déclarer **tout le parc hors ligne chaque nuit**, alors que chaque carte fonctionne
parfaitement — une alerte qui crie au loup toutes les nuits est une alerte qu'on finit par
ignorer, y compris le jour où un écran meurt pour de bon. Le contrat plafonne à 2.

### `rssi` — remonté au plugin depuis la v2.1

`ping()` joint le niveau Wi-Fi mesuré, en dBm, quand la liaison est établie :
`…&action=ping&device=…&rssi=-64`.

- Hors liaison, la sentinelle `JEEDOM_RSSI_NONE` (= `+1`, hors des bornes du contrat) : le
  paramètre n'est **pas envoyé**. `WiFi.RSSI()` ne veut rien dire sans association, et le
  plugin doit recevoir « pas de mesure », pas une valeur inventée.
- Les bornes `−120..0` sont vérifiées **des deux côtés**. Inutile de faire voyager une valeur
  qui se fera refuser, et le plugin ne doit pas dépendre de la discipline de la carte.
- **Le numéro de schéma ne bouge pas.** Un paramètre de *requête* qu'un serveur ne connaît
  pas, il l'ignore : une carte v2.1 face à un plugin v2.0 est servie exactement comme avant,
  et aucun champ de réponse ne change. Rien à négocier, donc rien à casser.

## Troncature : journalisée, jamais absorbée

Le contrat v2.0 l'impose des deux côtés. Trois messages distincts dans le parsing :

| Situation | Trace |
|---|---|
| plus de 32 boutons / plus de 4 pages | `⚠️ mise en page tronquée (max 4 pages, 32 boutons)` |
| `slot` hors de la grille annoncée | `⚠️ bouton "X" en case N hors grille 3x3 : invisible` |
| `page` invalide sur un `nav` | `⚠️ bouton "X" : cible de navigation N invalide` |

Un `slot` hors grille est rangé à `LAYOUT_MAX_SLOTS` (12) : le bouton existe, occupe une
case de `states`, mais n'est **jamais dessiné**. Un `nav` de cible invalide pointe sur sa
propre page — sans effet, plutôt qu'un saut imprévisible. Une page qui se déclare sa propre
parente voit son `parent` rejeté : un chevron de retour qui ne remonte nulle part piégerait
l'utilisateur.

En v1.4, la troncature était **muette** : un écran affichait calmement 6 boutons sur 8 et
personne ne pouvait le savoir.

# Tâches et files (v2.2)

Jusqu'en v2.1, chaque appel HTTP s'exécutait dans la tâche qui lit le tactile : un `ping` lent
figeait l'écran jusqu'à 10 s, un `layout` en échec jusqu'à 30 s, et **tout appui fait pendant
ce temps était perdu** — y compris le premier appui après le réveil, puisque le `ping` de
réveil partait dans le même tour de boucle. Depuis la v2.2 (`src/net/net_tasks.cpp`) :

| Tâche | Cœur | Prio | Pile | Rôle |
|---|---|---|---|---|
| `loopTask` (Arduino) | 1 | 1 | 8 ko | tactile, dessin, machine à états, application des résultats — **aucun HTTP** |
| `net` | **1** | **1** | 8 ko | `press` (en priorité), `layout` (3 essais, `press` servis entre deux), contrôle `firmware`, téléchargement OTA, sonde d'essai d'identifiants |
| `ping` | **1** | 1 | 6 ko | le `ping`, retenu jusqu'à 30 s en attente longue, avec **sa propre** instance `JeedomClient` / `HTTPClient` |
| `portal` | **1** | 1 | 6 ko | créée à l'ouverture du portail, détruite à sa fermeture |

⚠️ **Tout sur le cœur 1, à la priorité de `loopTask`, depuis la 2.3.1** (cœur 0 de la 2.2.0 à
la 2.3.0). Sur le cœur 0, la tâche IDLE est surveillée par le chien de garde **avec panique**
(`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0`) : toute attente active de 5 s y fait redémarrer
la carte. C'est ce qui a produit le `rst=wdt` de la 2.2.1 (`BoundedStream::readBytes`
retombait sur `Stream::timedRead`, corrigé par une attente qui cède la main), et
`HTTPClient::handleHeaderResponse()` garde le même défaut (`readStringUntil` sur un en-tête
coupé entre deux segments), dans du code qu'on ne contrôle pas. **Choix : ôter au défaut sa
conséquence plutôt que réécrire un client HTTP.** Sur le cœur 1, IDLE n'est pas surveillée ;
à priorité égale, le temps partagé de FreeRTOS (tranches de 1 ms) laisse tourner
l'affichage même si une tâche réseau boucle — au pire un écran ralenti, jamais un
redémarrage. Une priorité supérieure à `loopTask` figerait l'UI pendant une telle boucle.
Le `press` passe toujours devant : `net` le sert avant tout autre travail, et le ping a sa
propre tâche. lwIP et le pilote Wi-Fi restent sur le cœur 0. Toutes les requêtes passent en
**HTTP/1.0** (`useHTTP10`) : pas de `chunked`, le corps est lu brut.

Files (FreeRTOS, toutes non bloquantes côté UI) :

| File | Sens | Taille | Contenu |
|---|---|---|---|
| `qPress` | UI → net | 8 | `{index, id}` — **servie avant tout le reste** |
| `qJob` | UI → net | 4 | `Layout`, `Firmware`, `OtaApply` |
| `qResult` | net → UI | 8 × 96 o | `NetResult` : `LayoutDone`, `PressDone`, `FirmwareDone`, `OtaProgress`, `OtaDone` |
| `qPing` | ping → UI | 1 × 392 o | `PingResult` ; la tâche ping **attend** que l'UI ait consommé (aucune commande ne se perd) |
| `qFreeLayout` | UI → net | 1 | le tampon `Layout` dont dispose la tâche réseau |

**Double tampon `Layout`.** Deux instances statiques, `s_layoutA` et `s_layoutB`, chacune à
un seul propriétaire. La tâche réseau parse dans le sien, écrit le cache NVS (`store.save`,
hors UI), puis **cède** le pointeur dans `LayoutDone` ; l'UI l'adopte (`cur = fresh`,
`grid.setLayout()` ou `grid.adoptLayout()`) et rend l'ancien par `netReturnLayout()`. Rien
n'est copié, rien n'est partagé en écriture. Le cache NVS n'est lu par l'UI qu'**avant** le
lancement des tâches ; ensuite seule la tâche réseau touche à `LayoutStore` et à ses deux
tampons de travail.

Un `press` porte son rang de stockage **et** son `id` : à la réponse, l'UI vérifie que le
bouton porte toujours le même `id` à ce rang (la mise en page a pu changer entre-temps).

`layout` en échec : son backoff (2 s → 60 s) n'est **pas** remis à zéro par un ping réussi
(`scheduleLayoutSoon()` ne rapproche pas une échéance déjà prévue).

**Mesure des piles.** `netLogStacks()` journalise 15 s après le démarrage puis toutes les
10 min la marge minimale (`uxTaskGetStackHighWaterMark`, en octets sur ESP32) de chaque
tâche, plus heap libre, heap minimale et plus gros bloc :

```
[pile] marge min : ui 5123 o, net 3012/8192 o, ping 2911/6144 o | heap … (min …), bloc …
```

Les tailles de pile ci-dessus sont des **estimations** à resserrer sur ces mesures réelles.

## Attente longue, commandes, diagnostics (contrat v2.2)

**Capacités.** `features` (`wait`, `cmd`) et `rev` sont lus dans `layout` et dans `ping`
(`parseExtras()`). La carte n'utilise une capacité **que si elle est annoncée** : sans
`features.wait`, elle reste exactement en v2.1 (cadence `poll`, 2 × `poll` atténué, ping
immédiat au réveil, ping avancé à 2 s après un `press` en attente).

**`rev` part sur chaque `ping`** dès qu'on en connaît une (reçue de `layout` ou du ping
précédent), attente longue ou non : le plugin ne livre une commande qu'à une requête qui la
porte. Une `rev` de plus de 16 caractères est **ignorée**, pas tronquée — tronquée, elle
paraîtrait toujours « différente » et le serveur répondrait aussitôt.

**Attente longue** (si `features.wait > 0` **et** `rev` connue) : `wait = min(features.wait,
poll)`, délai HTTP `wait + 5 s` (connexion toujours à 5 s), relance 200 ms après la réponse,
backoff 2 s → 60 s sur erreur. Garde-fou **sur le temps seul** : une réponse en moins d'1 s
n'a pas été retenue, quelle qu'en soit la cause (y compris une `rev` qui changerait à chaque
réponse) ; on relance alors à 2 s au lieu de 200 ms. Une réponse `ping` **sans** `features` coupe l'attente longue (plugin
redescendu en v2.1). Plafond technique : `wait` ≤ 60 s, car `HTTPClient::setTimeout()` ne
prend qu'un `uint16_t` en ms.

> 2.3.0 : +5,0 ko de RAM statique pour les `TileValues` des deux `LayoutBundle` (2 × 1 344 o)
> et les deux tampons `PingResult` (2 × 1 740 o, en `.data`) ; l'essai d'identifiants
> alloue son `PingInfo` sur le tas le temps de l'essai.

**Commandes** (`cmd`, lue seulement si `features.cmd` est annoncé) : dédoublonnage sur le
dernier `seq` exécuté (en RAM), bornes réappliquées à la lecture (`identify` 1-120 s défaut 10,
`message` ≤ 64 caractères UTF-8 et 1-600 s défaut 30, `page` 0-3), verbe inconnu ignoré et
journalisé. La commande est traitée **quel que soit l'état** : le serveur l'a déjà retirée de
sa file.

| `do` | Effet |
|---|---|
| `reboot` | écran « Redémarrage », redémarrage 1 s plus tard (refusé pendant un OTA) |
| `identify` | plein écran nom + MAC + IP, rétroéclairage qui clignote, LED RVB qui tourne ; un toucher l'arrête |
| `message` | encadré par-dessus la grille (`UiCover::Grid`, le bandeau reste vivant) ; un toucher le ferme **sans actionner le bouton dessous** |
| `page` | réveille et ouvre la page |
| `calibrate` | calibration à l'écran, abandon à 60 s sans toucher |
| `ota` | avance le contrôle firmware (toujours soumis aux verrous du serveur, refusé si le firmware courant n'est pas validé) |
| `wifi` | essai de 60 s (voir « Essai d'identifiants ») ; succès → NVS ; refusé pendant un OTA |

`identify`, `message`, `page` et `calibrate` sont ignorés (et journalisés) pendant une
calibration, un OTA ou une attente de redémarrage.

**Essai d'identifiants** (commande `wifi` et portail). Rien n'est écrit en NVS avant deux
preuves : (1) une association **nouvelle** sur le SSID visé — un événement
`ARDUINO_EVENT_WIFI_STA_GOT_IP` survenu après le début de l'essai, car juste après
`disconnect()`/`begin()` `WiFi.status()` peut encore rendre `WL_CONNECTED` ; (2) une réponse
de l'API (`ok` ou `unknown_device`) à un ping émis **après** cette association par un client
jetable portant l'hôte et la clé essayés (`netRequestProbe`). Sans les deux en 60 s : retour
aux anciens identifiants, rien n'est écrit. La même règle d'association nouvelle
s'applique à toute tentative de `WifiMgr` (`_attemptIpSeq`).

**Diagnostics** joints à chaque ping : `up` (`esp_timer`, 64 bits, pas de retour à zéro à
49 j), `rst` (`esp_reset_reason()` ramené à `poweron|sw|panic|wdt|brownout|ext|other`),
`heap`, `blk` (`heap_caps_get_largest_free_block`), `ip`, `ssid` (encodé RFC 3986).

## Configuration locale : NVS, menu, portail (contrat v2.2)

`src/store/device_config.{h,cpp}`, espace NVS **`glowcfg`** — distinct du cache
`glowscreen`, que `LayoutStore::clear()` vide en entier :

| Clé | Contenu | Usine |
|---|---|---|
| `wifi` | SSID + mot de passe, **un seul blob** : une écriture ne peut pas laisser un SSID neuf avec l'ancien mot de passe | `WIFI_SSID`, `WIFI_PASS` |
| `host`, `apikey` | Jeedom | `JEEDOM_HOST`, `JEEDOM_APIKEY` |
| `cal` | `TouchCal` (8 o) | `TOUCH_CAL_FACTORY` |

Au démarrage, toute clé absente ou illisible reçoit sa valeur d'usine, **écrite** : elle ne
servira plus. Une carte passée de 2.1.1 à 2.2.0 par OTA retrouve donc exactement ses
identifiants compilés. Le mot de passe et la clé ne sont jamais journalisés. Les tâches
réseau **copient** hôte et clé à leur lancement : une modification par le portail prend
effet au redémarrage qui suit l'enregistrement.

**Wi-Fi** (`wifi_mgr.cpp`) : nom d'hôte `glowscreen-<6 hex>` posé **avant** `WiFi.mode()`,
`WIFI_ALL_CHANNEL_SCAN` + `WIFI_CONNECT_AP_BY_SIGNAL` (le point d'accès le plus fort, pas le
premier entendu), et **`setAutoReconnect(false)`** : jusqu'en v2.1 la reconnexion de la pile
et le `disconnect()`/`begin()` de `startAttempt()` se marchaient dessus. La MAC est lue dans
l'eFuse (`esp_read_mac(ESP_MAC_WIFI_STA)`), identique à `WiFi.macAddress()`.

**Menu Réglages** — appui **maintenu 5 s, doigt immobile**, sur le bandeau haut (sur l'écran
d'enrôlement : n'importe où). L'appui bref garde son sens (retour à la page parente). Écran :
MAC, IP, Wi-Fi + RSSI, hôte + état API, firmware + partition, durée de service, heap et plus
gros bloc (rafraîchis toutes les 2 s) ; boutons **Calibrer**, **Wi-Fi** (portail),
**Redémarrer** (deux appuis en 3 s) ; le bandeau ramène à l'accueil ; fermeture seule après
60 s sans toucher. Les écrans pleins n'acceptent un appui qu'après un **vrai relâchement** :
sans cela, le doigt qui vient de tenir 5 s le bandeau refermerait le menu en se levant.

**Portail de secours** (`net/portal.cpp`) — ouvert automatiquement après **5 min sans Wi-Fi**
(seulement sur l'écran d'accueil), ou depuis le menu. Point d'accès `GlowScreen-<6 hex>` en
AP+STA, mot de passe de 8 caractères tiré par `esp_random()` (alphabet sans 0/O/1/l) à chaque
ouverture, **affiché seulement à l'écran**. DNS captif + `WebServer` dans une tâche dédiée,
tout alloué à l'ouverture (`PortalCtx`) et rendu à la fermeture. Page : liste des réseaux
balayés (les plus forts, sans doublon), saisie libre, mot de passe et clé jamais réaffichés
(vides = inchangés ; un mot de passe vide pour un **autre** réseau = réseau ouvert), hôte
prérempli. Le formulaire n'écrit **rien** : il passe les valeurs à l'UI, qui lance un essai
d'identifiants (voir plus haut) ; succès → NVS (Wi-Fi + Jeedom) → « Redémarrage » 2,5 s plus
tard ; échec → anciens réglages, message à l'écran et état affiché en tête de la page. La station continue d'essayer
son réseau (essais espacés à 60 s tant qu'un appareil est associé au point d'accès) ; un
portail **automatique** se referme dès qu'elle l'a retrouvé, un portail ouvert depuis le menu
reste jusqu'au bouton « Fermer le portail », ou se referme seul après 20 min sans appareil
associé. Toucher le bandeau de l'écran « Portail Wi-Fi » (la flèche ◀, pas de bouton dédié)
ramène à la grille, portail ouvert ; tant que le Wi-Fi manque, le bandeau affiche alors
« Portail Wi-Fi », et le bouton « Wi-Fi » du menu devient « Portail ».

## Schéma 3 — tuiles `view`, lecture seule (2.3.0, contrat v3.0)

**Négociation.** La carte annonce `X-GLOWSCREEN32-SCHEMA: 3` (`LAYOUT_SCHEMA`). Un plugin
plus ancien répond en 2 ou en 1 : lu comme avant. ⚠️ **Un `press` annonce le schéma de son
`id`** (`Layout::schema`), pas 3 : les `id` d'un même bouton diffèrent d'un schéma à
l'autre (le schéma 2 retire les tuiles `view` et renumérote). Un `layout` qui change de
schéma, même à version égale, est traité comme un changement de structure (redessin complet).

**Modèle.** `BTN_MODE_VIEW` = 3. `value`/`tone` sont **volatils** : ils vivent dans
`TileValues` (32 × 42 o, indexé comme `Layout::buttons`), **hors de `Layout`**, donc hors
NVS. Le double tampon échange désormais une `LayoutBundle` = `Layout` + `TileValues` ; seul
`layout` part en NVS. Au démarrage depuis le cache, une tuile `view` affiche `—` jusqu'au
premier `ping`. `value` est recopiée à 16 caractères UTF-8 au plus (40 o), `tone` inconnu =
`neutral`. Un `mode: "view"` est reconnu dans tout schéma : jamais traité comme un bouton.

**Rendu** (`paintViewTile`, même sprite que les autres tuiles) : capuchon de 3 px et fond
sombre dans la couleur du ton, libellé en haut (avec l'icône 24 px **seulement** sur une
tuile d'au moins 80 × 90 px — 2.3.1 : en 3×3 elle repoussait la valeur en 16 px), **valeur**
centrée dans la plus grande des trois polices qui tient en largeur et en hauteur
(30 → 20 → 16 px ; 20 px en 3×3), `…` en dernier recours, `—` si `null`. Palette fixe dans
`theme.h` : `UI_TONE_OK` (vert), `UI_TONE_WARN` (orange), `UI_TONE_ALERT` (rouge) — les
couleurs d'état du bandeau ; `neutral` garde la couleur de la tuile. Tous les textes sont
contrastés à **≥ 3:1** (`readableOn()` éclaircit le ton s'il le faut, sinon
`contrast565()`) : mesuré ok 3,69, warn 3,71, alert 3,08, libellés et `—` ≥ 4,27. Chaque essai de police
recharge les métriques du sprite : une tuile `view` n'est repeinte que quand **sa** valeur
change.

**Toucher.** `hitTest()` ne renvoie **rien** sur une tuile `view` (ni retour visuel, ni
`press`), ni sur une tuile non-`nav` d'un écran `readonly` ; `handleButton()` le revérifie.

**`values` du ping.** Même règle que `states` : longueur ≠ nombre de tuiles → ignoré,
`layout` forcé ; plus de 32 entrées → rejeté. Indexé par l'`id` global ; seules les tuiles
`view` dont la valeur ou le ton change sont repeintes, et aucune d'une page non affichée.

**`read_only`** (403) : `JeedomResult::ErrReadOnly`, permanent, libellé « Lecture seule ».

**Tailles.** Pire cas mesuré (32 tuiles `view`, libellés et titres de 24 lettres accentuées,
valeurs de 16, JSON compact non échappé comme l'émet `api.php`) : `layout` **7 204 o**,
`ping` 2 266 o — sous les 8 192 o, avec ~1 ko de marge sur le `layout` extrême. Un plugin
qui échapperait l'UTF-8 en `\uXXXX` porterait le `layout` à 12,8 ko : refusé (tronqué au
plafond, journalisé). `PingResult` passe à 1 740 o : il ne transite plus **par valeur** dans
une file mais par échange de pointeur entre deux tampons statiques (`s_pingBufA/B`), comme
la mise en page ; la pile de la tâche ping ne grossit pas.

---

# Interface

Écran 320×240 en rotation 1 (paysage). Toute la géométrie et la palette tiennent dans
`src/ui/theme.h` — un seul endroit à modifier.

## Disposition

```
 0 ┌──────────────────────────────────────────────┐
   │ Salon                     21.4 °C  ·  18:42  │  bandeau haut, 28 px
28 ├──────────────────────────────────────────────┤
30 │  ┌────────┐  ┌────────┐  ┌────────┐          │
   │  │ tuile  │  │        │  │        │          │  grille, 188 px de haut
   │  └────────┘  └────────┘  └────────┘          │  marge 4, gouttière 5
   │  ┌────────┐  ┌────────┐  ┌────────┐          │
   │  └────────┘  └────────┘  └────────┘          │
218├──────────────────────────────────────────────┤
   │  ◀            ● ○ ○ ○                 2.0.0  │  bandeau bas, 20 px
240└──────────────────────────────────────────────┘
```

Palette sombre par construction : un écran mural allumé en permanence doit s'effacer quand
on ne le regarde pas. Le fond n'est pas un noir pur mais un bleu très sombre
(`UI_BG` = `#0B0E14`) — sur cette dalle, le noir absolu fait ressortir les fuites du
rétroéclairage et donne un rendu sale.

## Grille paginée

La grille n'est plus figée en 3×2. `cols` ∈ {3, 4}, `rows` ∈ {2, 3}, **défaut 3×3**,
jusqu'à **4 pages** et **32 boutons** par écran. Les tuiles sont calculées par
`uiCellW()` / `uiCellH()` (`src/ui/theme.h:51-58`) :

| Grille | Tuile | Sprite alloué |
|---|---|---|
| **3×3** (défaut) | 100 × 59 | 11 802 o |
| 3×2 (schéma 1) | 100 × 91 | 18 202 o |
| 4×3 | 74 × 59 | 8 734 o |
| 4×2 | 74 × 91 | 13 470 o |

Quatre chemins de navigation, tous **entièrement locaux** — donc instantanés, et
**fonctionnels hors réseau**, sur le seul cache NVS :

1. **bouton de mode `nav`** → `goToPage(b.target)` ;
2. **chevron de retour** en bas à gauche, dessiné seulement si la page a un parent ;
3. **pastilles de page**, au centre du bandeau bas — cliquables, avec une cible élargie de
   14 px (une pastille fait 8 px, un doigt bien davantage) ;
4. **bandeau haut cliquable** : `hitTest()` y renvoie `Back`. Contrairement à la v1.4, ce
   n'est plus une zone morte — c'est la cible la plus facile à viser de loin.

Plus le **balayage horizontal**, activé par écran depuis Jeedom (`ui.swipe`).

Un changement de page ne fait **pas** de `fillScreen` : chaque case est repeinte, y compris
les vides (`goToPage`, `src/ui/button_grid.cpp:176`). C'est ce qui évite le flash noir de
~60 ms à chaque navigation.

Une seule page → **aucune pastille** n'est dessinée : elle n'apprendrait rien et donnerait
l'impression qu'il y a autre chose à voir.

## Sprite par tuile — dimensionné à la case exacte

Les tuiles sont composées hors écran puis poussées d'un bloc : c'est ce qui supprime le
clignotement. `ensureSprite()` (`src/ui/button_grid.cpp:122`) alloue un sprite de la taille
**exacte** d'une tuile de la grille courante, et le réalloue si la géométrie change.

**Deux raisons, et aucune n'est cosmétique.**

1. **Un sprite plus grand que la tuile serait un piège.** `pushSprite(x, y)` pousse le
   sprite **entier** : les rangées excédentaires écraseraient la gouttière et le haut de la
   tuile suivante. Dimensionner au plus juste évite le problème à la source. Sur une grille
   3×3, cela économise en prime 6 400 octets par rapport au pire cas 3×2.
2. **Un sprite plein écran est hors de portée.** 320 × 240 × 2 = **153 602 octets** d'un
   seul tenant (TFT_eSPI alloue `w*h+1` pixels, `Sprite.cpp:174`), alors que la carte n'a
   **aucune PSRAM** — TFT_eSPI ne l'emprunte que si `psramFound()` — et que le plus gros
   bloc contigu libre tombe à 100–120 ko une fois la pile Wi-Fi levée. `createSprite()`
   renverrait `nullptr` **en silence**.

Si l'allocation échoue malgré tout (heap fragmentée), `paintTile()` **retombe sur un tracé
direct à l'écran** : moins joli, un clignotement par tuile, mais parfaitement fonctionnel.
Un écran dégradé vaut mieux qu'un écran noir. L'échec est journalisé avec la heap libre.

`UI_SPRITE_W`/`UI_SPRITE_H` (100 × 91) restent dans `theme.h` à titre documentaire : ils
donnent le **pire cas** d'allocation, pas la valeur utilisée.

⚠️ Le sprite est poussé **sans couleur transparente**. Utiliser `UI_BG` comme transparent
serait un piège : `contrast565()` renvoie exactement `UI_BG` pour le texte sombre des tuiles
claires, dont les pixels seraient alors percés — un libellé invisible sur fond jaune ou
cyan. Le sprite est pré-rempli en `UI_BG`, les coins arrondis retombent donc sur la bonne
couleur de fond.

## Polices VLW accentuées

**Les accents sont réglés.** `uiAsciiFold()` a disparu : plus aucune translittération, les
libellés partent en UTF-8 de Jeedom jusqu'à l'écran.

Trois polices **Inter** (SIL Open Font License 1.1) sont embarquées en flash au format VLW
de TFT_eSPI :

| Police | Taille | Graisse | Interligne | Usage |
|---|---|---|---|---|
| `FONT_UI_16` | **18 240 o** | wght 500 | 20 px | bandeaux, textes secondaires, petites tuiles |
| `FONT_UI_20` | **26 233 o** | wght 600 | 25 px | libellés des grandes tuiles |
| `FONT_UI_30` | **50 788 o** | wght 600 | 37 px | titres, enrôlement, écrans OTA |

**140 glyphes** chacune : ASCII 0x20–0x7E, plus le répertoire français complet
(`À Â Ä Ç È É Ê Ë Î Ï Ô Ö Ù Û Ü Ÿ` et leurs minuscules, `Œ œ`) et la ponctuation utile
(`« » ° · – — ’ … € ² ³`). Total **95 261 octets de flash**. Tout autre point de code est
dessiné par TFT_eSPI comme un rectangle vide.

Le 16 px est en graisse 500 et les deux autres en 600 : à 16 px, une graisse plus lourde se
boucherait.

> **`SMOOTH_FONT` était DÉJÀ actif** dans `platformio.ini` bien avant la v2.0. Les VLW n'ont
> demandé **aucun changement de build** — contrairement à ce qu'annonçait la version
> précédente de ce document, qui présentait les accents comme un chantier à ouvrir.

### Une seule police chargée à la fois, d'où la répartition

`tft.loadFont()` **libère la précédente** : TFT_eSPI ne garde qu'une police lissée chargée
par instance, avec ses tables de métriques en RAM — **12 octets par glyphe** en 7
allocations séparées (`Smooth_font.cpp:172-178`), soit **1 680 octets** pour nos 140
glyphes. En changer à chaque tuile fragmenterait la heap pour rien.

Le firmware suit donc ce qui est chargé sur **chaque instance** — `g_tftFont` pour l'écran,
`g_sprFont` pour le sprite — et ne recharge que si nécessaire (`useFont()`,
`src/ui/button_grid.cpp:18`). Comme l'écran et le sprite sont **deux instances distinctes**,
ils portent deux polices simultanément :

- l'**écran** garde `FONT_UI_16` (bandeaux) ;
- le **sprite** garde la police des libellés de la grille courante.

En régime normal, **il n'y a aucun rechargement**. Les deux seules exceptions, toutes deux
sans conséquence :

- les écrans pleins (enrôlement, OTA) chargent le 20 et le 30 px sur l'écran ; le
  `drawBanner()` suivant remet le 16 px ;
- sur une grille à 3 rangées la tuile fait 59 px de haut et le 20 px n'y tient pas (voir
  ci-dessous) : la première tuile dessinée charge le 16 px sur le sprite, et plus rien ne
  bouge. Un libellé trop large pour le 20 px sur une grille 3×2 provoque le même repli pour
  cette tuile-là.

### Pourquoi le 20 px est réservé aux grandes tuiles

Les hauteurs sont **comptées, pas approchées** (`src/ui/button_grid.cpp:256-266`). Sur une
grille 3×3 la tuile fait 59 px :

```
capuchon 3 + marge 3 + icône 24 + gouttière 3 + ligne 20 + marge 6 = 59
```

Une police de 20 px a un interligne **réel** de 25 px : elle déborderait de 4 px sous la
tuile et rognerait les jambages. Le seuil est `h >= 80` (`bigTile`), franchi par les
grilles à 2 rangées seulement. Les grandes tuiles gagnent aussi une **icône doublée** (48 px
au lieu de 24), par simple recopiage de pixels — pas de lissage, c'est justement ce qui
garde les traits nets.

Filet de sécurité : quelle que soit la grille, `textY` est plafonné à `oy + h - 2 - lineH`.
Mieux vaut coller le libellé au bas de la tuile que le laisser déborder.

`fitLabel()` tronque avec « … » sur une frontière UTF-8 quand le libellé ne tient toujours
pas.

⚠️ `setTextColor(fg, base)` est appelé avec le fond **réel** sous le texte : c'est lui que
TFT_eSPI utilise pour fusionner l'anti-crénelage. Un fond faux donnerait un liseré autour de
chaque glyphe.

### Génération

Les `.cpp` sont produits par `gen_vlw.py` (freetype-py) et relus par `check_vlw.py`, qui
rejoue le parsing de `TFT_eSPI::loadMetrics()` et vérifie
`taille_fichier == 24 + 28 * gCount + Σ(largeur × hauteur)`.
**Ne jamais éditer les tableaux à la main.**

## Icônes

`src/ui/icons.{h,cpp}` : **40 identifiants** (`ICON_NONE` + 39 glyphes), bitmaps 24×24 en
**1 bit**, 72 octets chacun, soit **2 808 octets** de flash.

Format : rangée par rangée, bit de poids fort = pixel de gauche, 3 octets par rangée — exactement
ce qu'attend `TFT_eSPI::drawBitmap()`. Un pixel à 1 se dessine dans la couleur d'avant-plan,
un pixel à 0 est transparent. Aucun anti-crénelage : c'est ce qui permet de doubler le
glyphe sans le rendre flou.

Le vocabulaire est **fermé** : le contrat transporte un nom, `iconFromName()` le résout
**une fois au parsing** vers un `uint8_t`, et la chaîne n'est jamais conservée. Un nom
inconnu vaut `ICON_NONE` et la tuile se rabat sur son seul libellé — jamais un carré vide.

> **Gain mesurable.** En v1.4, `icon` était parsé, stocké sur **12 octets** par bouton, et
> **dessiné nulle part** : de la donnée morte recopiée en NVS. En v2.0 le champ coûte
> **1 octet** et sert enfin à quelque chose. Sur 32 boutons, 352 octets de blob économisés.

⚠️ **L'ordre de l'`enum IconId` est celui du contrat** : il est écrit tel quel en NVS. Ne
jamais insérer au milieu ni réordonner — seulement ajouter avant `ICON_COUNT`, et de concert
avec le plugin Jeedom. Sinon un cache existant afficherait les mauvaises icônes.

L'icône n'est dessinée que si la tuile fait **au moins 46 px** de haut ; en dessous, le
libellé seul.

## Bandeau haut — ce qui s'affiche, et ce qui ne s'affiche pas

`drawStatus()` (`src/ui/button_grid.cpp:376`) repeint **uniquement la moitié droite** du
bandeau (à partir de x = 150). Repeindre le bandeau entier ferait clignoter le titre trente
fois par heure pour rien.

| Situation | Moitié droite |
|---|---|
| tout va bien | `info`, un point médian, puis l'heure si `ui.clock` |
| Wi-Fi perdu ou API en erreur | **l'anomalie**, à la place de `info` et de l'heure |

> **Pourquoi « API OK » a disparu.** Un voyant qui affiche « OK » 99,9 % du temps n'informe
> personne : il occupe la seule zone de l'écran qui pourrait porter une information utile, et
> l'œil cesse de le lire — donc il ne remarque pas non plus le jour où il passe au rouge. Un
> bandeau qui n'affiche **que** les anomalies se fait remarquer quand il en affiche une.
> L'heure et la température, elles, servent tous les jours.

Le RSSI en clair (« −84 dBm ») apparaît dès que le signal passe sous `UI_RSSI_WEAK_DBM` =
**−75 dBm**, **que le reste aille bien ou non** — c'est une alerte *précoce*, et la
subordonner à un état dégradé la rendrait muette au moment précis où elle sert : une liaison
faible mais encore fonctionnelle. Elle est aussi placée **la plus à gauche** et n'est jamais
abandonnée au profit d'`info` (`button_grid.cpp:581`). Il n'y a plus de barres de signal : le
chiffre se lit mieux et coûte moins de pixels.

> ⚠️ Ce paragraphe décrivait jusqu'ici l'inverse — « n'apparaît que si le bandeau est déjà en
> mode dégradé **et** le signal est faible ». C'était un **défaut réel de la v1.4, corrigé
> depuis** ; la description, elle, était restée. Le code est la référence, et il est d'accord
> avec le contrat. Corrigé ici pour que personne ne « répare » le code vers la doc.

**Depuis la v2.1, le niveau est aussi remonté à Jeedom** en paramètre `rssi` du `ping` (voir
plus bas), où il alimente une commande d'information. Le bandeau reste utile — il n'a besoin
de personne — mais il faut se tenir devant l'écran pour le lire, ce qui est exactement ce
qu'on ne fait pas devant un panneau mural au fond d'un couloir.

`info` est une chaîne **déjà formatée par le plugin**. La carte ne l'interprète pas, ne
calcule rien, et ne connaît même pas la notion de température — c'est ce qui permet d'y
mettre une humidité, une puissance ou un niveau de cuve sans toucher au firmware.

## Horloge

`setClock(epochUtc, tzOffsetSec)` recale l'heure à chaque `ping` ; entre deux pings elle est
égrenée localement depuis `millis()`. **Pas de NTP** : le serveur est déjà la référence de
temps, et une carte sans accès Internet doit continuer à donner l'heure.

`tickClock()` est appelé à chaque tour de boucle en `RUN` mais **ne repeint que si la minute
affichée a changé** (`_shownMinute`). Les écrans pleins remettent `_shownMinute` à −1 pour
forcer le prochain tracé.

## Bandeau bas

Chevron de retour à gauche (seulement si la page a un parent), pastilles de page au centre,
et la **version du firmware** à droite, très discrète. Sur un parc, c'est la seule façon de
savoir ce que tourne un écran sans câble série ni accès à Jeedom.

---

# Tactile

## L'action part au RELÂCHEMENT

Contrairement à la v1.4, qui déclenchait dès le **front descendant**, `pollTouch()`
(`src/main.cpp`) exécute au **relâchement**. Le retour visuel, lui, reste **immédiat** —
la règle du contrat est respectée.

```
contact        -> anti-rebond 250 ms, réveil éventuel, hitTest,
                  setPressed()  ← retour visuel IMMÉDIAT
doigt qui bouge-> si le point sort de la tuile : setPressed(-1), appui ANNULÉ
relâchement    -> balayage ? sinon dérive ≤ 18 px ? -> handleTap()
```

Ce découplage apporte deux choses qu'on ne peut pas avoir autrement :

- **un doigt qui glisse hors de la tuile annule l'appui** au lieu de l'exécuter. Sur du
  matériel réel, un appui de travers ouvrait le portail ;
- **le balayage devient possible** : il faut connaître le point d'arrivée pour le distinguer
  d'un appui.

Une dérive supérieure à `UI_TAP_SLOP` (18 px) annule l'appui même si le point d'arrivée est
resté dans la tuile.

## Balayage

Activé par écran (`ui.swipe`), et seulement s'il y a plus d'une page. Les seuils
(`src/ui/theme.h:83-92`) sont volontairement exigeants :

| Seuil | Valeur | Raison |
|---|---|---|
| `UI_SWIPE_MIN_DX` | 55 px | sur une dalle résistive, un appui appuyé dérive de quelques pixels |
| `UI_SWIPE_MAX_DY` | 40 px | au-delà, ce n'est plus horizontal |
| `UI_SWIPE_MAX_MS` | 800 ms | au-delà, c'est un appui traînant |

Il faut en plus `|dx| > |dy|`. Un balayage **n'actionne rien** : il change de page et rend
la main.

## Calibration — en NVS, à l'écran (v2.2)

`src/ui/touch_calib.h` ne contient plus que des **valeurs d'usine** et le calcul partagé :

```c
TOUCH_RAW_X_MIN / _MAX     bornes brutes d'usine sur l'axe X   (200 / 3700)
TOUCH_RAW_Y_MIN / _MAX     idem sur l'axe Y                    (240 / 3800)
struct TouchCal            bornes EN SERVICE (min > max = axe inversé, map() s'en charge)
touchCalPlausible()        chaque borne dans 0..4095, étendue >= 1500 par axe
touchCalFromSamples()      4 relevés -> bornes extrapolées aux bords (outil ET firmware)
TOUCH_SWAP_XY              orientation, toujours compilée
TOUCH_Z_MIN                pression minimale (400) — plancher de la bibliothèque
```

Les bornes en service viennent de `deviceConfig.cal()` (NVS), lues par `touchRawToScreen()`
à chaque échantillon. Elles sont **revérifiées à la lecture** : un blob absent, corrompu ou
aberrant est remplacé par l'usine — sinon un écran intouchable ne pourrait plus être
recalibré sans câble.

Calibration à l'écran : **Réglages > Calibrer**, ou commande `calibrate` depuis Jeedom.
Quatre cibles à 20 px des bords ; sous chaque doigt, les lectures brutes des 40 premières
ms sont écartées puis moyennées ; un appui de moins de 4 lectures ne compte pas ; la cible
suivante n'est armée qu'après un vrai relâchement (150 ms sans contact). À la fin,
`touchCalFromSamples()` refuse des relevés incohérents (deux cibles d'un même côté écartées
de plus de 600 points bruts, ~50 px). **Abandon après 60 s sans toucher** : rien n'est écrit,
les anciennes valeurs restent. Le résultat s'affiche 2,5 s puis l'écran revient à l'accueil.

L'outil de banc reste disponible :

```bash
pio run -e calib -t upload
pio device monitor -e calib
```

L'outil affiche les mêmes quatre cibles, utilise le même `touchCalFromSamples()` et imprime
les bornes ; la vérification (point rouge sous le doigt) se fait avec les bornes **qu'il
vient de mesurer**. Un appui long (> 2 s) relance la séquence. Recopier ses valeurs dans
`touch_calib.h` ne change que les valeurs d'usine des cartes **neuves**.

---

# Rétroéclairage

IO21 est piloté par logiciel, en **PWM via LEDC** (canal 0, 5 kHz, 8 bits,
`src/ui/theme.h:76-81`). Un panneau mural à pleine luminosité toute la nuit est une
nuisance : après **1 minute** sans activité (`UI_BL_IDLE_MS`), la luminosité descend de 255
à 24 par paliers, et remonte de la même façon au réveil.

**Le premier toucher sur un écran atténué ne fait que le réveiller** — il ne déclenche rien
(`touchWokeScreen`, `src/main.cpp`). Sans cela, chercher l'écran dans le noir allumerait une
lampe.

L'atténuation est suspendue quand un doigt est posé, pendant l'enrôlement (un écran qui
affiche une MAC à recopier ne doit pas s'éteindre sous le nez de l'utilisateur), pendant un
message, une identification, une calibration, un OTA. Passer en `ENROLL` **réveille** l'écran
(jusqu'en v2.1 il pouvait y rester atténué, et le toucher n'y était même pas lu).

> ⚠️ **`backlightInit()` doit être appelé APRÈS `tft.init()`.** TFT_eSPI est configuré avec
> `-DTFT_BL=21` et pilote lui-même cette broche par `digitalWrite` à l'initialisation :
> attacher le LEDC avant se ferait écraser, et l'écran resterait à pleine luminosité (ou
> éteint) sans que rien ne le signale. L'ordre est explicite dans `setup()`.

Corollaire déjà connu du matériel : **un écran noir est normal quand le firmware ne tourne
pas.** Ce n'est pas une carte morte.

---

# Machine à états

| État | Rôle | Sorties |
|---|---|---|
| `BOOT` | init écran/tactile/LEDC, relecture du cache NVS, affichage immédiat des boutons | → `WIFI` |
| `WIFI` | attend l'association ; si un cache existe, l'écran reste tactile **et navigable** | → `LAYOUT` |
| `LAYOUT` | `action=layout` ; backoff 2 s → 60 s en cas d'échec | → `RUN`, → `ENROLL` |
| `ENROLL` | écran pas encore déclaré dans Jeedom (`unknown_device`) : MAC affichée en grand, `layout` rejoué toutes les **10 s** | → `RUN` dès que Jeedom répond |
| `RUN` | tactile + ping (tâche `ping`) + horloge + contrôle OTA horaire | → `WIFI` (perte réseau), → `LAYOUT` (version changée), → `ENROLL` (équipement supprimé) |

Ces états ne font plus que **décider et demander** : `stepStateMachine()` dépose un `layout`
ou un contrôle `firmware` dans une file et revient aussitôt. Un `layout` en vol est marqué
(`layoutInFlight`) pour ne jamais être demandé deux fois.

Par-dessus l'état réseau, un **mode d'écran** (`UiMode`) : `Home` (grille, ou enrôlement en
`ENROLL`, avec éventuellement un message par-dessus), `Settings`, `Portal`, `Calib`,
`Identify`, `Ota`, `Notice`. Les écrans pleins passent la grille en `UiCover::All` : elle
continue de mettre son modèle à jour (états, page, bandeau) mais **ne dessine plus rien**
jusqu'à `uncover()`. C'est ce qui corrige le bandeau Wi-Fi qui se repeignait par-dessus
l'écran d'enrôlement.

Règles de dégradation :

- **Perte de Wi-Fi en `RUN`** → retour en `WIFI` **sans effacer l'écran** ; le bandeau passe
  en « Hors ligne ». Les boutons restent affichés, l'écran reste tactile, **la navigation
  entre pages continue de fonctionner** et les appuis réseau échouent immédiatement (le
  client refuse d'appeler sans liaison, pas de blocage).
- **Serveur injoignable ou en erreur** avec un cache présent → on passe quand même en `RUN` ;
  le bandeau affiche la cause (`Injoignable`, `Clé API`, `Écran ?`…).
- **Erreurs non transitoires** (`jeedomResultIsPermanent()`) : `bad_apikey`, clé absente,
  **`bad_request`** et `firmware_unavailable`. Un 400 est un défaut de programmation d'un des
  deux côtés, pas un incident réseau : il est journalisé explicitement
  (`BAD_REQUEST sur <url> -- défaut de programmation`) et **jamais réessayé en backoff**.
  Sur `press` et `ping` on passe simplement à autre chose ; sur `layout` la tentative
  suivante est repoussée à 60 s au lieu d'escalader.
- **Clé API absente** (`JEEDOM_APIKEY` resté à sa valeur de gabarit) : `jeedomApikeyConfigured()`
  la reconnaît et **aucune requête n'est émise** ; le bandeau affiche « Sans clé » en
  permanence au lieu d'enchaîner des 401 silencieux.

Tous les comparateurs de temps sont écrits `(int32_t)(millis() - deadline) >= 0` : la
soustraction reste juste au passage de `millis()` à zéro, ce qu'un `millis() > deadline`
ne ferait pas. Pour « tout de suite », on pose `millis()`, jamais `0`.

## Redessin sélectif après un `layout`

`onLayoutDone()` ne redessine tout que si la **structure** a changé (`version`, `count`,
`pageCount`, `cols`, `rows`). Sinon il ne repeint que les cases dont l'état, l'`id`, le
`mode` ou le marqueur « en attente » ont bougé. Un `layout` rejoué à l'identique ne produit
donc **aucun tracé**. Depuis la v2.2, un changement de structure **garde la page courante**
si elle existe encore (`setLayout(cur, keepPage)`) ; seule la sortie d'enrôlement ramène à
l'accueil.

## Enrôlement d'un écran neuf

Quand `layout` (ou `ping`) répond `unknown_device`, la carte bascule en `ENROLL` :

```
            Écran non déclaré
      Créez l'équipement dans Jeedom
        ┌───────────────────────┐
        │   24:6f:28:12:34:56   │   <- FONT_UI_30, encadré, en bleu accent
        └───────────────────────┘
              246f28123456            <- forme à recopier dans Jeedom
                  2.0.0
```

Aucun câble série n'est nécessaire pour mettre un écran supplémentaire en service : on
flashe, on branche, on lit la MAC à l'écran, on crée l'eqLogic dans Jeedom — la carte se
configure seule au `layout` suivant, **sans redémarrage**.

---

# Mise à jour par le réseau (OTA)

## Version compilée

`src/fw_version.h` porte une constante unique, `GLOWSCREEN_FW_VERSION`, **source unique de
vérité**. Elle est :

- envoyée en paramètre `fw` à chaque `action=firmware`, ce qui permet à Jeedom d'afficher la
  version qui tourne sur chaque écran du parc ;
- affichée **en permanence dans le bandeau bas**, à droite, en petit et en gris — c'est la
  première chose qu'on demande quand un écran se comporte bizarrement ;
- rappelée sur l'écran d'enrôlement, sous la MAC.

⚠️ **À incrémenter à chaque binaire publié.** Sinon le plugin ne voit pas la différence et
l'écran reste indéfiniment sur l'ancienne version.

## Marqueur de version dans le binaire

**Le descripteur `esp_app_desc_t` de l'image n'est pas le nôtre.** À l'offset 0x20 du `.bin`
(magic `0xABCD5432`), les champs viennent des bibliothèques Arduino précompilées du framework :

```
version      @0x10  "esp-idf: v4.4.7 38eeba213a"
project_name @0x30  "arduino-lib-builder"
idf_ver      @0x70  "v4.4.7-dirty"
```

Avec `framework = arduino` sous PlatformIO, ces champs ne sont pas sous notre contrôle. Ils
sont **identiques pour tous nos binaires** : un plugin qui s'en servirait pour détecter une
nouvelle version ne déclencherait **jamais** d'OTA, et la panne serait silencieuse.

On grave donc notre propre marqueur, dérivé de `GLOWSCREEN_FW_VERSION` :

```
GLOWSCREEN32-FW:<version>\0
```

Défini dans **`src/fw_version.cpp`** (et seulement *déclaré* dans le `.h` : le définir dans
l'en-tête en produirait une occurrence par unité de compilation, rendant l'extraction
ambiguë).

**Deux protections, contre deux risques distincts :**

| Risque | Parade |
|---|---|
| le compilateur élimine un symbole qu'il croit inutilisé | `__attribute__((used))` |
| l'éditeur de liens supprime une section non référencée (`-fdata-sections` + `--gc-sections`, tous deux actifs ici) | une **référence réelle à l'exécution** : `setup()` affiche `GLOW_FW_TAG` sur le port série |

`used` seul ne suffirait pas : il agit sur le compilateur, pas sur `--gc-sections`. La
référence dans `setup()` n'est pas un artifice — c'est aussi la trace qui identifie un
binaire depuis la console série.

### Vérification après compilation

À faire à chaque publication de binaire. Doit renvoyer **exactement 1** :

```bash
grep -a -o "GLOWSCREEN32-FW:" .pio/build/cyd/firmware.bin | wc -l
```

Pour voir la valeur et son offset (`strings` peut être indisponible selon l'état des outils
Xcode) :

```bash
python3 -c "
d=open('.pio/build/cyd/firmware.bin','rb').read()
o=d.find(b'GLOWSCREEN32-FW:')
print(hex(o), d[o:d.find(b'\0',o)].decode())"
```

Relevé du 2026-09-22 sur `firmware.bin` (1 129 280 o) : **1 seule occurrence**, offset
`0x00132A`, valeur `GLOWSCREEN32-FW:2.0.0`. L'environnement `calib` ne contient pas le
marqueur, ce qui est normal : ce n'est pas un firmware applicatif et il ne doit jamais partir
en OTA.

⚠️ **Côté plugin, chercher le préfixe complet `GLOWSCREEN32-FW:`**, pas seulement
`GLOWSCREEN32` : le binaire contient aussi la chaîne `X-GLOWSCREEN32-APIKEY` (l'en-tête
d'authentification). Avec le préfixe complet, aucune ambiguïté.

## Partitions

`min_spiffs.csv` est déjà une table OTA, rien à changer :

```
otadata  0x00E000  0x002000   quelle partition amorcer + état de chacune
app0     0x010000  0x1E0000   ota_0
app1     0x1F0000  0x1E0000   ota_1
```

Le firmware fait ~1,12 Mo pour 1,97 Mo par partition : il reste **843 383 octets** de marge
dans chacune (voir « Empreinte »).

## Déroulé d'une mise à jour

1. **Contrôle** — `action=firmware&fw=<version>` une fois par heure
   (`OTA_CHECK_PERIOD_MS`), plus un premier appel une minute après le démarrage
   (`OTA_FIRST_CHECK_MS`). `update: false` couvre aussi bien « à jour » que « OTA désactivé
   côté serveur » : la carte ne fait pas la différence et n'a aucun moyen de passer outre.
2. **Garde-fous avant de démarrer** (`otaMayStart()`, vérifiés avant le contrôle **et** à
   sa réponse) — état `RUN`, Wi-Fi connecté (pas en plein essai `wifi`), clé API configurée,
   **aucun doigt posé**, **aucun `press` en vol**, écran d'accueil sans message ni menu,
   portail fermé, et **firmware déjà validé**. Annonce reçue mais écran occupé : nouvel essai
   dans 1 min. La commande `ota` ne fait qu'avancer ce contrôle.
3. **Annonce incomplète refusée** — `url` vide ou `sha256` de longueur ≠ 64 : on ne
   télécharge pas un binaire qu'on ne saura pas vérifier.
4. **Téléchargement** — dans la **tâche réseau** ; flux HTTP lu par blocs de 1 ko, écrits au
   fil de l'eau dans la partition inactive via `Update`, et simultanément passés dans un
   contexte SHA-256. La tâche poste sa progression (`OtaProgress`, sans attendre : un
   pourcentage sauté est sans importance) ; l'UI dessine la barre. L'attente longue est
   **suspendue** (`pingSuspend`). ⚠️ `vTaskDelay(1)` après chaque bloc : sur le cœur 0, la
   tâche IDLE est surveillée **avec panique** — un flux qui débite sans trou la priverait de
   CPU 5 s et ferait redémarrer la carte en pleine écriture.
5. **Vérification** — l'empreinte est comparée **avant `Update.end()`**, car c'est `end()` qui
   bascule la partition d'amorçage. Empreinte fausse → `Update.abort()`, la partition
   d'amorçage n'est jamais touchée.
6. **Redémarrage** sur la nouvelle partition.

Le tampon de 1 ko est en BSS, pas sur la pile : la tâche réseau n'a que 8 ko et la carte pas
de PSRAM. Une allocation dynamique qui échouerait *pendant* une mise à jour serait le pire
moment possible.

## En cas d'échec

Tous les cas (`otaOutcomeLabel()`) — HTTP, taille annoncée ≠ reçue, partition inutilisable,
écriture flash, flux interrompu, empreinte fausse, image refusée — aboutissent au même
comportement : **le firmware actuel est conservé, intact**. La cause s'affiche quatre
secondes avec le message « Le firmware actuel est intact », puis la grille revient et l'écran
reprend son fonctionnement normal. Le contrôle suivant est repoussé de 6 h
(`OTA_RETRY_AFTER_FAIL_MS`).

`firmware_unavailable` (404, le plugin annonce une mise à jour dont le fichier a disparu) est
classé **erreur permanente** : même report de 6 h, pas de backoff serré.

**Un OTA raté n'immobilise jamais l'écran.**

## Retour arrière automatique — le point critique

Sans filet, un firmware qui ne sait plus joindre le réseau transforme un bug logiciel en panne
matérielle : il faut décrocher l'écran du mur et le rebrancher en USB.

Le bootloader précompilé d'arduino-esp32 active `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
(vérifié dans `tools/sdk/esp32/sdkconfig`). `ota_updater.cpp` porte un `#warning` qui se
déclenchera si une future version du framework le désactivait — sinon la régression ne se
découvrirait que le jour où on en a besoin.

Conséquence : une image installée par OTA démarre dans l'état `ESP_OTA_IMG_PENDING_VERIFY`.
Tant qu'elle ne s'est pas déclarée saine, **le prochain redémarrage la rejette** et le
bootloader repart sur la partition précédente.

```
initArduino() -> verifyRollbackLater() renvoie true   (ota_updater.cpp, depuis 2.1.1)
setup()
  ├─ otaPendingVerify() ?  ── non ──> firmware déjà validé, rien à faire
  └─ oui : compte à rebours de 10 min (OTA_VERIFY_TIMEOUT_MS)
           ⚠️ AUCUN esp_ota_mark_app_valid_* ici

loop() -> updateFirmwareVerification()
  ├─ écran initialisé  ET  Wi-Fi connecté  ET  API a répondu
  │  ET un ping réussi rendu par la TÂCHE ping
  │  ET (attente longue annoncée => un ping retenu terminé normalement)
  │  ET une réponse JSON bien formée à action=firmware (contrôle seul ;
  │     firmware_unavailable et annonce incomplète comptent, bad_apikey non)
  │  -- OU, en ENROLL : deux unknown_device espacées d'au moins 1 min (2.3.1) --
  │  ET >= 120 s de fonctionnement
  │      └─> esp_ota_mark_app_valid_cancel_rollback()  -> firmware adopté
  │          + otaJournalClearTarget()
  └─ délai dépassé (10 min)
         └─> esp_ota_mark_app_invalid_rollback_and_reboot()  -> version précédente
```

> **Durci après revue de sûreté (2.2.0).** La première écriture de la 2.2.0 validait sur un
> seul `layout` réussi : la tâche ping, l'attente longue et la lecture de `cmd` n'avaient
> pas encore tourné. Un défaut dans l'une d'elles, découvert après validation, aurait donné
> une carte murale qui redémarre en boucle sans retour arrière possible. Chaque chemin doit
> désormais avoir tourné au moins une fois. Le **contrôle** firmware part donc **avant**
> validation (`otaMayCheck()`, 60 s après le démarrage, réessayé toutes les 30 s en cas
> d'échec) ; une mise à jour proposée à ce moment n'est **jamais installée**. La trace
> d'échec détaille chaque preuve manquante.

**Ne jamais marquer valide depuis `setup()`** : ce serait désactiver le filet tout en croyant
l'avoir. C'est l'erreur classique, et elle ne se voit pas — tout fonctionne, jusqu'au jour où
rien ne fonctionne.

### Ce qui compte comme « l'API a répondu »

`Ok` **ou** `unknown_device`, quelle que soit la tâche qui a obtenu la réponse : les
résultats remontent par les files et sont comptés par `noteApiAnswer()` **dans la tâche
d'affichage**. Les deux prouvent que la chaîne entière fonctionne : réseau,
authentification par en-tête, parsing JSON — le plugin a validé la clé avant d'aller chercher
l'équipement. `bad_apikey` ne compte **pas** : si une nouvelle version cassait l'en-tête
d'authentification, on veut précisément qu'elle soit rejetée.

Inclure `unknown_device` n'est pas un détail : sans cela, **un écran neuf pas encore déclaré
dans Jeedom reviendrait en arrière indéfiniment**, ce qui serait absurde.

### Sur le délai de 10 minutes

Porté de 5 à 10 min avec les preuves supplémentaires (contrôle firmware à 60 s, ping retenu
jusqu'à 30 s, 120 s minimum). Généreux à dessein. Sur la liaison faible constatée sur site (−88 dBm), l'association Wi-Fi
puis le premier `layout` peuvent demander plusieurs tentatives. Un délai trop court ferait
revenir en arrière sur un firmware parfaitement sain. Trop long, on laisse un écran
inutilisable plus longtemps que nécessaire. Le paramètre est `OTA_VERIFY_TIMEOUT_MS` dans
`main.cpp`.

⚠️ **Tout redémarrage avant validation ramène la version précédente** — y compris
« Redémarrer » du menu, la commande `reboot` et l'enregistrement du portail. C'est le filet,
il n'a pas d'exception ; `scheduleReboot()` le signale sur le port série.

### Mémoire des versions rejetées (espace NVS `glowota`)

Sans elle, une version rejetée par retour arrière serait reproposée par le serveur,
retéléchargée et réinstallée en boucle. `ota_updater.cpp` :

| Moment | Action |
|---|---|
| après `Update.end(true)` réussi (2.3.1) | `otaJournalSetTarget(version)` dans `otaApply()` ; si la NVS refuse, l'amorçage est remis sur la partition courante et l'installation annulée. Noter la visée **avant** le téléchargement (2.2.0-2.3.0) faisait passer un téléchargement coupé suivi d'une coupure pour un rejet |
| au démarrage | `otaJournalBoot()` : visée ≠ courante **et** partition invalidée (`esp_ota_get_last_invalid_partition`) → la visée devient `rejected` ; visée ≠ courante sans partition invalidée → installation interrompue, visée effacée |
| à la validation | visée effacée |
| OTA en échec | visée effacée (rien d'installé) |
| à l'annonce | `otaJournalAllows()` : refuse `rejected` (trace « déjà REJETÉE ») ; une autre version efface `rejected` |

⚠️ Cette mémoire n'existe qu'à partir de la 2.2.0 : une 2.1.1 qui rejette la 2.2.0 ne s'en
souviendra pas. **Refermer le verrou de l'écran témoin dans Jeedom si la 2.2.0 revient en
arrière.**

### Pas d'OTA en chaîne

`otaMayStart()` refuse de démarrer une mise à jour tant que `fwPendingVerify` est vrai : on
écraserait la partition de repli qui nous sauve.

## Revenir en arrière à la main

Si tout a échoué — les deux partitions sont mauvaises, ou l'écran ne redémarre plus — il faut
passer par l'USB. **Le piège** : `pio run -t upload` écrit dans `app0` mais **ne touche pas à
`otadata`**. Si la carte avait basculé sur `app1`, elle continuera d'amorcer `app1` et le
flash n'aura aucun effet visible. Il faut effacer `otadata` :

```bash
# 1) Forcer l'amorçage sur app0 en effaçant otadata (0xE000, 8 ko)
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
    --chip esp32 --port /dev/cu.usbserial-130 erase_region 0xe000 0x2000

# 2) Flasher normalement
pio run -e cyd -t upload
```

Solution radicale si le doute persiste (efface **tout**, y compris le cache NVS de la mise en
page — il se reconstruit au premier `layout`) :

```bash
pio run -e cyd -t erase
pio run -e cyd -t upload
```

Le port `/dev/cu.usbserial-130` dépend du port USB physique ; `./scripts/detect-cyd.sh` le
retrouve.

---

# Environnements PlatformIO

| Env | Usage |
|---|---|
| `cyd` | firmware, CYD original (ILI9341) — **par défaut** |
| `cyd2usb` | variante ST7789 (micro-USB + USB-C) |
| `cyd_slow` | même firmware, upload à 115 200 bauds |
| `calib` | outil de calibration tactile seul |

`src/tools/` contient des programmes autonomes (leur propre `setup()`/`loop()`) : ils sont
exclus du firmware par `build_src_filter = +<*> -<tools/>` dans `[env]`, et l'env `calib`
inverse le filtre. Sans cela, deux `setup()` entreraient en conflit à l'édition de liens.

## Ce qui a été retiré du build en v2.0

Trois réglages payaient de la flash ou de la confusion sans rien rendre :

| Retiré | Pourquoi |
|---|---|
| `-DLOAD_FONT6`, `7`, `8` | polices bitmap **compilées sans jamais être utilisées** — **6,3 ko** de flash pour rien. L'interface est passée aux VLW ; `LOAD_GLCD`, `LOAD_FONT2` et `LOAD_FONT4` restent pour l'outil de calibration et les replis |
| `XPT2046_Bitbang_Slim` (lib_deps) | déclaré, mais **inclus par aucun fichier de `src/`**. Il ne redeviendra utile que si la carte SD doit cohabiter avec le tactile sur VSPI |
| `-DSPI_TOUCH_FREQUENCY` | **sans aucun effet** : ce réglage ne concerne que le pilote tactile interne de TFT_eSPI, que nous n'utilisons pas. `XPT2046_Touchscreen` fixe 2 MHz en dur dans sa propre `SPISettings` |

`-DSMOOTH_FONT=1` est **conservé** — et l'était déjà avant la v2.0.

---

# Empreinte (mesurée)

| Env | Flash | RAM statique |
|---|---|---|
| `cyd` 2.3.1 | **1 291 737 o / 1 966 080 (65,7 %)** | **69 024 o / 327 680 (21,1 %)** |
| `cyd` 2.3.0 | **1 288 085 o / 1 966 080 (65,5 %)** | **65 936 o / 327 680 (20,1 %)** |
| `cyd` 2.2.0 | 1 282 381 o (65,2 %) | 60 928 o (18,6 %) |
| `calib` | 320 805 o (16,3 %) | 22 468 o (6,9 %) |

Avant la 2.2 (2.1.1) : 1 209 397 o (61,5 %) et 56 504 o (17,2 %). Soit **+73 ko de flash**
(essentiellement `WebServer` + `DNSServer` pour le portail, puis les écrans et les tâches)
et **+3,0 ko de RAM statique** (files de résultats en `.data`, clients par tâche, tampons
`s_ping`/`s_pingOut`/`s_res`). Les quatre `Layout` résidents (2 du double tampon, 2 de
`LayoutStore`) passent de 4 × 1 608 o à **4 × 2 332 o** en 2.3.1 (tampons en caractères). `firmware.bin` : 1 288 960 o — il reste
**~680 ko par partition OTA**.

À l'exécution s'ajoutent, sur le tas : piles des tâches `net` (8 ko) et `ping` (6 ko), files
(~1,3 ko), et quand le portail est ouvert sa pile (6 ko) + `PortalCtx` (~1 ko) + `WebServer`.
Plus, comme avant : le sprite (8,7 à 18,2 ko), les métriques de police et la pile Wi-Fi.
Les valeurs réelles sont dans la trace `[pile]`.

```bash
pio run -e cyd        # compilation seule, ne flashe rien
```

---

# Corrections 2.3.1 (revue complète)

| Défaut | Correction |
|---|---|
| `nextLayoutTryMs` jamais rafraîchie après un `layout` réussi : passé 24,8 jours, la comparaison signée la voyait « future » et une nouvelle `version` bloquait l'écran en LAYOUT, appuis actifs avec des `id` périmés | rafraîchie au succès ; toutes les échéances passent par `due(deadline, maxAhead)`, qui tient une échéance plus lointaine que son plus long délai pour **périmée donc atteinte** ; les durées écoulées sont en non signé |
| appui possible sur une mise en page sue périmée | `layoutStale` (version annoncée ≠ affichée, `states`/`values` incohérents, `unknown_device`) : **aucun `press`** jusqu'au `layout` suivant, navigation conservée |
| `CredTrial::nextProbeMs` à 0 | posée à `millis()` au lancement, comparée par `due()` |
| attente active dans `HTTPClient` sur le cœur 0 | tâches sur le cœur 1, priorité de `loopTask` ; HTTP/1.0 |
| portail : clé envoyée à un hôte saisi par un visiteur | hôte changé ⇒ clé à ressaisir ; clé obligatoire si celle en service est invalide ; SSID non nettoyé, longueurs en octets, phrase de 64 = hexadécimale ; échec précis (Wi-Fi non associé / Jeedom injoignable / clé refusée / Jeedom en erreur) sur l'écran du portail **et** la page ; hôte + clé en **un** blob NVS (migration des clés 2.2.0) ; portail du menu refermé après 20 min sans client ; `portalRunning()` exclut la fermeture en cours |
| calibration acceptée sans contrôle | cible de contrôle au centre, écart > 20 px ou 15 s sans toucher ⇒ anciennes bornes ; messages courts |
| tuile `view` illisible en 3×3, contrastes 1,24 à 2,85:1 | pas d'icône sous 80 px de haut ni en 4 colonnes (valeur en 20 px en 3×3) ; textes ≥ 3:1 (`readableOn`, `contrast565`) — mesuré : ok 3,69, warn 3,71, alert 3,08, libellés ≥ 4,27 |
| ENROLL : validation impossible | deux `unknown_device` espacées ≥ 1 min remplacent les preuves 4-5 |
| preuve 5 trop stricte | toute réponse JSON bien formée (sauf `bad_apikey`) |
| `reboot` avant validation | refusé (il ferait rejeter une version saine) |
| sonde d'un essai précédent | numéro de génération dans `NetResult` |
| pastille « en attente » 25 s après un ping déjà plus récent | horodatage d'émission du `press` comparé au dernier ping appliqué |
| geste en cours quand l'écran change à distance | `cancelGesture()` : rien ne part au relâchement |
| tampons en octets | libellé/titre/nom 49 o, `info` 33 o ; troncature journalisée ; grille hors bornes journalisée ; `poll` hors [5, 3600] ⇒ **30** (règle du contrat) ; cache v05 |
| textes débordants | pied d'enrôlement, écran d'identification bornés |

# Points ouverts

1. **La 2.3.1 n'a pas encore tourné sur la carte** (la 2.3.0 y est en service, OTA validé).
   Liste de contrôle dans `ETAT-DU-PROJET.md`.
2. **Calibration** : réglée par la v2.2 (NVS + calibration à l'écran). `TOUCH_SWAP_XY` reste
   compilé — une dalle aux axes permutés demanderait encore un binaire.
3. **Liaison Wi-Fi faible sur site.** Relevés : un `press` perdu à **−88 dBm** ; un OTA mort
   à 2 % à **−92/−93 dBm**, passé en 8 s à −64 dBm. Le RSSI est désormais remonté à Jeedom à
   chaque ping (v2.1) avec les diagnostics (v2.2). ⚠️ Les `ping` passent dans les deux cas :
   **regarder le RSSI avant de soupçonner le code.**
4. **La clé API et la commande `wifi` circulent en clair sur le réseau** (HTTP). Hors
   périmètre, à reprendre avec TLS.
5. **`ping` ne dit pas quel écran il décrit.** Le contrat charge le plugin de refuser les
   doublons de MAC.
6. **`ButtonGrid::_wifiLabel` a été supprimé** (v2.2) : l'écran d'enrôlement affiche le nom du
   réseau, le menu Réglages le SSID et le RSSI.
