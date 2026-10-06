# Mise à jour du firmware par le réseau

Ce document explique comment **publier** un firmware depuis Jeedom et comment les écrans le
**reçoivent**. Il est écrit pour être relu le jour où quelque chose se passe mal.

## Le principe : les écrans tirent, Jeedom ne pousse pas

Aucune machine ne se connecte à un écran. C'est **l'écran qui demande**, comme il demande déjà
sa mise en page :

```
Écran  ──  action=firmware&device=<mac>&fw=1.4.0  ──▶  plugin Jeedom
       ◀──  { "update": true, "url": …, "sha256": … }  ──
       ──  GET <url>  ──────────────────────────────▶  fichier .bin
```

Ce choix n'est pas cosmétique. La carte est sur **192.168.20.x** alors que Jeedom est sur
**192.168.1.x** : les requêtes sortantes de la carte passent, mais joindre la carte depuis
l'extérieur supposerait connaître son adresse et traverser le routage en sens inverse. Le
modèle « la carte va chercher » évite le problème et fonctionne à l'identique avec 1 ou 10
écrans.

## Publier un firmware — la procédure

### 1. Incrémenter la version

Dans `src/fw_version.h` :

```c
#define GLOWSCREEN_FW_VERSION "1.4.0"
```

**C'est l'étape qu'on oublie.** Le plugin compare des chaînes de version : republier un
binaire sans changer ce numéro signifie qu'aucun écran ne le prendra, et on cherchera
longtemps pourquoi « la mise à jour ne part pas ».

La version est lue par le plugin **dans le binaire lui-même**, derrière le marqueur
`GLOWSCREEN32-FW:` gravé par `src/fw_version.cpp` — donc c'est bien cette constante qui
fait foi de bout en bout, pas le nom du fichier.

> ⚠️ **Surtout pas `esp_app_desc_t`.** Cette page l'a affirmé pendant toute la v1.4, et
> c'était précisément le piège que la v1.4 avait dû corriger. Avec `framework = arduino`,
> ce descripteur vient des bibliothèques Arduino précompilées et annonce
> « esp-idf: v4.4.7 … » pour **tous** nos binaires. La panne qui en découle est
> silencieuse : le dépôt réussit, la valeur ressemble à une version, les verrous
> fonctionnent, et le plugin compare indéfiniment une chaîne à elle-même en journalisant
> « rien de plus récent à proposer ». Aucun code d'erreur ne la signale. Voir
> `docs/api-contract.md`, section `action=firmware`.

### 2. Compiler

```bash
pio run -e cyd
# produit .pio/build/cyd/firmware.bin
```

### 3. Déposer sur Jeedom

> ⚠️ **Le `.bin` embarque en clair les valeurs d'usine de `src/secrets.h`** (Wi-Fi, clé API du
> plugin). Ne le diffuser nulle part ailleurs que sur le serveur Jeedom, qui ne le sert qu'avec
> un jeton à durée limitée (`action=fwfile`). Pour un parc tiers, compiler avec un `secrets.h`
> sans aucune vraie valeur : chaque écran se configure ensuite par son portail Wi-Fi.
> Les binaires ne sont jamais versionnés (`*.bin` dans `.gitignore`).

Page du plugin → cadre **« Firmware (mise à jour par le réseau) »** → *Parcourir* →
choisir `.pio/build/cyd/firmware.bin` → **Déposer**.

Le cadre se remplit sans recharger la page : version, taille, SHA-256, URL, date. Le fichier
est rangé dans `data/firmware/glowscreen32-<version>.bin`.

Contrôles effectués au dépôt :
- un fichier dont le premier octet n'est pas **`0xE9`** est refusé (ce n'est pas une image
  ESP32) ;
- la version est extraite du descripteur du binaire, pas du nom de fichier ;
- le SHA-256 et la taille sont calculés **sur le fichier écrit**, pas sur ce qui a été envoyé.

### 4. Ouvrir les verrous, un écran d'abord

**Les deux verrous sont fermés par défaut.** Il faut les deux pour qu'un écran reçoive quoi
que ce soit.

| Verrou | Où | Effet |
|---|---|---|
| **Global** `ota_enabled` | page du plugin | prend effet **immédiatement**, sans « Sauvegarder » — c'est un arrêt d'urgence |
| **Par écran** `ota_allowed` | onglet *Écran* de l'équipement | nécessite « Sauvegarder » |

Procédure recommandée :

1. Ouvrir le **verrou global**.
2. Ouvrir **un seul écran témoin** — de préférence celui que vous avez sous la main.
3. Attendre qu'il redémarre et **réapparaisse dans le tableau du parc avec sa nouvelle
   version**. C'est la preuve que le firmware fonctionne : il n'a pu s'annoncer que parce que
   l'écran, le Wi-Fi et l'API fonctionnent tous les trois.
4. Ouvrir les autres écrans.

Ouvrir le verrou d'un écran **ne le fait pas redessiner** : ce réglage n'entre pas dans la
signature de mise en page.

### 5. Refermer les verrous — à faire après chaque campagne

**Une fois tous les écrans à jour, referme les verrous.** Les laisser ouverts n'apporte rien et
expose le parc : le prochain binaire déposé partirait immédiatement, y compris un dépôt fait
par erreur.

| Verrou | Où le fermer | Effet |
|---|---|---|
| **Global** `ota_enabled` | *Plugins → GlowScreen32* (page du plugin) → décocher **« Autoriser les mises à jour par le réseau »** | **immédiat**, sans « Sauvegarder » |
| **Par écran** `ota_allowed` | *Plugins → GlowScreen32 → l'équipement* → onglet **Écran** → décocher **« Verrou de cet écran »** → **Sauvegarder** | après enregistrement |

Fermer le **verrou global** suffit à tout bloquer : c'est l'arrêt d'urgence, un seul clic. Les
verrous par écran peuvent rester tels quels entre deux campagnes, puisqu'ils ne servent à rien
tant que le global est fermé.

Pour vérifier l'état, le tableau du parc affiche la colonne **Verrou OTA** (le ET des deux).

En ligne de commande, si besoin :

```bash
sshpass -e ssh <utilisateur>@192.168.1.10 'printf "…\n" | sudo -S -p "" php -r "
require_once \"/var/www/html/core/php/core.inc.php\";
config::save(\"ota_enabled\", 0, \"glowscreen32\");
foreach (eqLogic::byType(\"glowscreen32\") as \$e) {
  \$e->setConfiguration(\"ota_allowed\", 0); \$e->save();
}
"'
```

### En cas de problème pendant une campagne : refermer le verrou global

Un seul clic, effet immédiat, aucun « Sauvegarder ». Les écrans qui n'ont pas encore migré
continuent d'interroger et reçoivent `update: false` — ils restent sur leur firmware actuel.

C'est tout l'intérêt d'avoir gardé la décision côté serveur : **la carte n'a aucun moyen de
passer outre.**

## Surveiller le parc

Le tableau de la page du plugin donne, pour chaque écran : nom, MAC, **dernier contact**,
**version du firmware**, **état du verrou OTA**, nombre de boutons et version de mise en page.

C'est avec cette vue qu'on pilote un déploiement progressif : on voit quel écran est passé en
1.4.0, lequel est resté en 1.3.0, et lequel ne répond plus.

> **Les journaux du plugin sont filtrés à « Error » par défaut sur cette installation.** Les
> décisions OTA sont journalisées en `info` : pour les voir, abaisser le niveau dans
> *Analyse → Journaux*. Sans cela le journal reste muet et on croit que rien ne se passe.

Le journal distingue les trois cas de blocage, là où l'API renvoie une réponse identique :

```
… bloqué par le verrou global du plugin (ota_enabled) et par le verrou de cet écran (ota_allowed)
… bloqué par le verrou de cet écran
… bloqué par le verrou global
```

## Deux détails d'installation à connaître

- **`.deployignore` exclut `data/firmware/*.bin`** : un firmware téléversé **ne disparaît pas**
  au prochain `deploy-plugin.sh`. Le motif est limité aux binaires pour que
  `data/firmware/.htaccess` reste déployé.
- **`data/firmware/.htaccess` rouvre les seuls `.bin`** face au `Deny from all` de `data/`.
  Sans lui, le fichier serait annoncé par l'API mais renverrait 403 au téléchargement — même
  piège que le `.htaccess` de `plugin_info/` qui bloquait l'icône du plugin.

## Côté carte — ce qui se passe à la réception

1. **Interrogation** : une minute après le démarrage, puis toutes les heures.
2. **Garde-fous avant de commencer** (`otaMayStart()`) — tous requis :
   état `RUN`, Wi-Fi connecté, clé API configurée, **aucun doigt posé sur l'écran**, aucun
   appui en cours de traitement, et **firmware courant déjà validé**.
   > Ce dernier point est important : lancer une mise à jour depuis un firmware non encore
   > confirmé écraserait la partition de repli, c'est-à-dire exactement le filet qui nous
   > sauve en cas de problème.
3. **Téléchargement** par blocs de 1 ko, écrits au fil de l'eau dans la partition inactive et
   passés simultanément dans un calcul SHA-256. Le tampon est en BSS, pas sur la pile (la
   tâche Arduino n'a que 8 ko, et une allocation qui échoue pendant une mise à jour serait le
   pire moment possible). La progression s'affiche à l'écran.
4. **Vérification de l'empreinte AVANT `Update.end()`** — c'est `end()` qui bascule la
   partition d'amorçage. Empreinte fausse → `Update.abort()`, la partition d'amorçage n'est
   jamais touchée.
5. **Redémarrage** sur la nouvelle partition.

### En cas d'échec

HTTP en erreur, taille incohérente, partition indisponible, écriture refusée, flux coupé,
empreinte fausse, image rejetée : **tous aboutissent au même comportement**. Le firmware
courant reste intact, la cause s'affiche 4 s à l'écran, l'écran revient à sa grille de boutons
et réessaiera dans 6 h.

**Un échec de mise à jour n'empêche jamais un écran de fonctionner normalement.**

## Le filet de sécurité : retour arrière automatique

Sans lui, un firmware défectueux transformerait un bug logiciel en panne matérielle : il
faudrait décrocher chaque écran et le rebrancher en USB.

Au démarrage sur un firmware fraîchement installé, la partition est marquée
`ESP_OTA_IMG_PENDING_VERIFY` et un compte à rebours de **10 minutes** démarre. Le firmware ne
se déclare sain **qu'une fois six preuves réunies** (contrat, « Validation renforcée ») :

1. l'écran s'est initialisé,
2. le Wi-Fi est connecté,
3. l'API a répondu au moins une fois,
4. un `ping` a réussi **depuis la tâche ping** — un ping en attente longue si le plugin
   annonce `features.wait`,
5. `action=firmware` a donné une réponse valide (contrôle seul : rien n'est installé avant
   la validation ; `firmware_unavailable` compte, c'est le chemin qui est prouvé),
6. la carte a tourné **120 s** sans interruption.

Alors seulement, `esp_ota_mark_app_valid_cancel_rollback()`. Sinon,
`esp_ota_mark_app_invalid_rollback_and_reboot()` : l'ESP32 **rebascule seul sur la version
précédente**, et cette version rejetée est **mémorisée en NVS** pour ne plus être réinstallée.
Un firmware qui ne sait plus joindre le réseau — ou plus être remplacé — se défait donc tout
seul.

Subtilités voulues :

- **`unknown_device` compte comme « l'API a répondu »**, au même titre qu'une réponse normale.
  Les deux prouvent toute la chaîne — réseau, authentification, analyse JSON — puisque le
  plugin a validé la clé avant de chercher l'équipement.
- **Écran non déclaré (ENROLL)** : les preuves 4 et 5 sont impossibles pour une MAC inconnue.
  Elles sont **remplacées** par deux réponses `unknown_device` espacées d'au moins une minute
  (plus les 120 s). Sans cela, **un écran neuf pas encore déclaré dans Jeedom reviendrait en
  arrière après chaque OTA**, et une version saine serait mémorisée comme rejetée.
- **`bad_apikey` ne compte pas.** Si une version cassait l'en-tête d'authentification, on veut
  précisément qu'elle soit rejetée.
- **La commande `reboot` est refusée tant que le firmware n'est pas validé** (comme pendant un
  OTA) : un redémarrage à ce stade déclencherait le retour arrière. Le bouton **Redémarrer**
  du menu, lui, n'est pas bloqué — il ramène alors la version précédente.
- Le délai est de **10 minutes** : sur une liaison faible (−88 dBm constatés sur site),
  l'association puis le premier `layout` peuvent demander plusieurs tentatives, et les preuves
  4 et 5 exigent un contrôle firmware (une minute après le démarrage) et un ping retenu
  (jusqu'à 30 s). Trop court, on reviendrait en arrière sur un firmware parfaitement sain.

> ⚠️ **Ne jamais marquer le firmware valide dans `setup()`.** Ce serait désactiver la
> protection tout en croyant l'avoir. C'est l'erreur classique de l'OTA sur ESP32.
> Vérification : `esp_ota_mark_app_valid` ne doit apparaître **nulle part** dans `main.cpp`.

## Revenir en arrière à la main — le piège

Si tout échoue et qu'il faut reflasher en USB :

```bash
pio run -e cyd -t upload     # ⚠️ NE SUFFIT PAS toujours
```

**`upload` écrit dans `app0` mais ne touche pas à `otadata`.** Si la carte avait basculé sur
`app1`, elle continuera d'amorcer `app1` : le flash n'aura **aucun effet visible** et on
croira la carte morte alors qu'elle exécute simplement l'autre partition.

La bonne procédure :

```bash
# Remettre à zéro le choix de partition, puis flasher
pio pkg exec -p tool-esptoolpy -- esptool.py --port /dev/cu.usbserial-XXX \
    erase_region 0xe000 0x2000
pio run -e cyd -t upload

# Ou, plus radical (efface aussi le cache NVS et la configuration) :
pio run -e cyd -t erase
pio run -e cyd -t upload
```

## Table de partitions

`min_spiffs.csv`, déjà compatible OTA sans modification :

| Partition | Rôle | Taille |
|---|---|---|
| `otadata` | quelle partition amorcer | 8 ko |
| `app0` / `ota_0` | emplacement A | 1,97 Mo |
| `app1` / `ota_1` | emplacement B | 1,97 Mo |

Firmware actuel : ~1,00 Mo. Il reste ~960 ko libres dans chaque emplacement.

## Validé en conditions réelles — 2026-09-22

Mise à jour 1.4.0 → 1.4.1 sur la carte `246f28123456`, binaire de 1 011 600 octets.

```
[61s] [ota] mise a jour annoncee : 1.4.1 (1011600 octets)
[61s] [ota] 0% (1024/1011600 octets)
[69s] [ota] 100% (1011600/1011600 octets)
[69s] [ota] 1011600 octets verifies, bascule sur la partition inactive
[70s] [boot] GLOWSCREEN32-FW:1.4.1 sur la partition app1
```

**8 secondes** pour 1 Mo, puis redémarrage sur `app1` et remontée de `fw=1.4.1` à Jeedom.
L'écran dont le verrou était resté fermé n'a **rien reçu** — le déploiement progressif
fonctionne.

### La qualité du lien radio est déterminante

Trois tentatives, code et serveur identiques, seule la position de la carte a changé :

| RSSI | Téléchargé | Résultat |
|---|---|---|
| −92 dBm | 18 979 o (1,9 %) | échec, `on reste sur app0` |
| −93 dBm | 26 207 o (2,6 %) | échec, `on reste sur app0` |
| **−64 dBm** | **1 011 600 o (100 %)** | **réussite en 8 s** |

Un `ping` de quelques octets passe encore à −92 dBm, **un transfert d'1 Mo n'a aucune chance**.
Avant de soupçonner le code ou le serveur, **regarder le RSSI affiché dans le bandeau**.

Pour isoler un doute côté serveur, télécharger le binaire depuis une autre machine :

```bash
curl -o /tmp/fw.bin -w "HTTP %{http_code}  %{size_download} o\n" \
  http://192.168.1.10/plugins/glowscreen32/data/firmware/glowscreen32-<version>.bin
shasum -a 256 /tmp/fw.bin   # doit correspondre au SHA-256 annoncé par l'API
```

Les deux échecs ont confirmé le filet de sécurité : abandon propre, partition d'amorçage
intacte, écran opérationnel dans la seconde. **Un échec de mise à jour ne casse rien.**
