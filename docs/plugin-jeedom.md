# Le plugin Jeedom GlowScreen32

Compte rendu de réalisation, 22 septembre 2026.
Mis à jour le même jour, deux fois : après un essai sur matériel réel, puis
après l'ajout de la mise à jour par le réseau — **plugin 1.2, contrat d'API
v1.4**.

Le plugin `glowscreen32` pilote des écrans tactiles ESP32 depuis Jeedom. Il met
en œuvre le contrat d'API **v1.4** (`docs/api-contract.md`), côté serveur ; le
firmware est écrit en face, par un autre agent.

- Dépôt de développement : `/home/<utilisateur>/dev/jeedom-plugin-glowscreen32` (git,
  branche `beta`, trois commits).
- Installation Jeedom : `/var/www/html/plugins/glowscreen32` — copie non
  versionnée, écrasée à chaque déploiement.

**Rien de ce plugin ne sort de ces deux dossiers.** Aucun fichier du cœur de
Jeedom, aucun autre plugin, aucune configuration d'Apache, aucune table de base
de données n'a été touché.

---

## 0. Ce que la version 1.2 apporte : l'OTA, sous double verrou

La mise à jour du firmware par le réseau. C'est la seule fonction du plugin qui
peut **casser durablement un écran à distance** : une image défectueuse écrite
dans la partition inactive, et il faut décrocher la carte du mur pour la
rebrancher en USB. Toute la conception vient de là.

### 0.1 Deux verrous qui ne font pas le même travail

| Verrou | Portée | Défaut |
|---|---|---|
| `ota_enabled` | **global au plugin** (`config::byKey`) | **fermé** |
| `ota_allowed` | **par écran** (configuration de l'eqLogic) | **fermé** |

**Les deux** doivent être ouverts pour qu'un écran reçoive `update: true`. Ce
n'est pas une ceinture et des bretelles :

- Le verrou **par écran** permet le **déploiement progressif**. On ouvre un seul
  écran témoin, on vérifie qu'il revient en ligne et qu'il fonctionne, puis on
  ouvre les autres. Sans lui, une mauvaise version part partout en même temps,
  et l'on découvre le défaut sur six murs au lieu d'un.
- Le verrou **global** permet d'**arrêter net** la propagation. Un firmware qui
  s'avère défectueux se coupe d'un seul interrupteur, sans rouvrir chaque
  équipement : les écrans restés en arrière continuent d'interroger et reçoivent
  `update: false`.

Une carte bloquée reçoit **exactement la même réponse** qu'une carte à jour —
`{"ok":true,"update":false}`. Elle n'a aucun moyen de faire la différence, donc
aucun moyen de passer outre. La décision est entièrement côté serveur.

### 0.2 Ce qui a été prévu avant d'avoir à le corriger

Trois pièges de cette installation étaient connus, et sont traités dans le
plugin plutôt que découverts en production :

1. **`deploy-plugin.sh` fait un `rsync --delete`.** Le binaire n'arrive jamais
   par le dépôt de développement — il est déposé par l'utilisateur, dans
   l'installation. Sans exclusion, le premier redéploiement venu l'efface, et
   les écrans se voient proposer une URL qui rend 404 au milieu de leur mise à
   jour. `.deployignore` exclut donc `data/firmware/*.bin` ; un fichier exclu
   n'est pas supprimé par `--delete` (il faudrait `--delete-excluded`).
   L'exclusion ne vise que les binaires, pour que `data/firmware/.htaccess`
   continue, lui, d'être déployé. Vérifié § 5.10.
2. **`data/.htaccess` porte `Deny from all`.** Le firmware est téléchargé par
   une *carte*, pas par un navigateur authentifié : `data/firmware/.htaccess`
   rouvre les seuls `.bin`, exactement comme `plugin_info/.htaccess` rouvre les
   seules images (§ 2.1). C'est la même panne, au même endroit, et elle avait
   déjà coûté l'icône du plugin en 1.1.
3. **Un fichier déposé finit écrit dans la flash d'une carte.** Le plugin refuse
   ce qui ne commence pas par l'octet `0xE9` : ce n'est alors pas une image
   d'application ESP32, et la déposer reviendrait à promettre à une carte de
   quoi ne plus redémarrer.

### 0.3 La version vient du binaire — et pas de celle qu'il annonce tout seul

La version affichée par Jeedom doit être **celle qui est dans le binaire**, et
non celle qu'un opérateur a retapée dans un formulaire : c'est la seule façon
que la comparaison avec ce que la carte annonce à `action=firmware` ait un sens.

#### Le descripteur ESP-IDF ne convient pas — et il a fallu un vrai binaire pour le voir

Une image ESP32 porte un descripteur normalisé à l'offset `0x20` (mot magique
`0xABCD5432`), avec un champ `version`. Le premier jet le lisait : c'est
standard, c'est documenté, et cela paraissait la bonne source.

Le contrôle sur un **vrai** binaire du projet — et non sur une image fabriquée
pour l'essai — a montré autre chose :

```
descripteur @ 0x20, magic 0xABCD5432
  version      @0x10 → « esp-idf: v4.4.7 38eeba213a »
  project_name @0x30 → « arduino-lib-builder »
  idf_ver      @0x70 → « v4.4.7-dirty »
```

Ce descripteur vient des bibliothèques Arduino **précompilées** du framework,
pas de notre code : avec `framework = arduino` sous PlatformIO, on ne le
maîtrise pas. Il est donc **identique dans tous nos binaires**.

La panne qui en découlait mérite d'être nommée, parce qu'elle est du genre le
plus désagréable : **silencieuse et durable**. Le dépôt réussissait. La version
annoncée — `esp-idfv4.4.738eeba213a` — avait la tête d'une version. Le fichier
se rangeait sous ce nom. Les verrous fonctionnaient. Et **aucun écran n'aurait
jamais reçu la moindre mise à jour**, puisque `version_compare()` aurait comparé
une chaîne à elle-même à chaque appel, pour toujours. Rien, nulle part, ne
l'aurait dit : ni un code d'erreur, ni une ligne de journal — le journal aurait
patiemment répété « rien de plus récent à proposer ».

> Le premier jet portait en outre une vraie faute d'offset — `0x30` compté
> depuis le début du descripteur au lieu de `0x10` — qui lisait `project_name`
> et rangeait le binaire sous `glowscreen32-glowscreen32.bin`. Corrigée sur le
> moment, elle n'a eu qu'un mérite : rendre visible que la source elle-même
> était mauvaise. Les deux champs se suivent et produisent tous deux une chaîne
> plausible, ce qui est exactement pourquoi ni l'une ni l'autre ne se serait vue
> à la relecture.

#### Un marqueur à nous

Le firmware grave donc sa propre chaîne (`src/fw_version.cpp`), protégée de
l'élimination par le compilateur (`__attribute__((used))`) **et** par l'éditeur
de liens (`--gc-sections` ignore `used` ; la garantie vient d'une référence
réelle à l'exécution) :

```
GLOWSCREEN32-FW:<version>\0
```

Le plugin la cherche dans le fichier téléversé, et lit jusqu'au premier octet
nul. Le préfixe est **cherché en entier** : le binaire contient aussi
`X-GLOWSCREEN32-APIKEY`, et une recherche sur `GLOWSCREEN32` seul pourrait en
extraire `-APIKEY` comme numéro de version.

```
0x00132A  GLOWSCREEN32-FW:1.4.1     ← le marqueur, une seule occurrence
0x0018BA  X-GLOWSCREEN32-APIKEY     ← l'en-tête d'authentification
```

Ce qui suit le préfixe n'est accepté que si un **octet nul le termine** dans la
fenêtre — c'est ce qui distingue un littéral C d'une suite d'octets qui
ressemblerait au préfixe —, qu'il ne contient que `[0-9A-Za-z._+-]`, et qu'il
tient en **31 caractères**. La valeur devient un nom de fichier et une URL, et
elle vient d'un binaire que l'utilisateur choisit : tout ce qui est accepté ici
doit rester inoffensif une fois recollé dans un chemin. À défaut, la recherche
**continue à l'occurrence suivante** plutôt que d'abandonner — un message de
diagnostic pourrait citer le préfixe sans être le marqueur.

Le fichier est parcouru par tranches d'un mégaoctet, avec un recouvrement égal à
la longueur du motif : un marqueur à cheval sur deux lectures est retrouvé au
tour suivant (vérifié § 5.11, sur les cinq positions autour de la frontière).

#### Sans marqueur, le dépôt est refusé

Pas de repli sur le nom du fichier, pas de version par défaut. Publier un
firmware dont on ne sait pas nommer la version, c'est ou bien ne jamais
déclencher l'OTA, ou bien le déclencher sur un binaire qui n'est pas le nôtre.
Le refus est explicite, et il couvre au passage un cas qu'on n'avait pas
cherché : l'environnement `calib` (l'outil de calibration tactile) ne porte pas
de marqueur, et **ne peut donc pas partir en OTA**.

Une version passée explicitement à `publishFirmware()` continue de primer :
c'est une porte de sortie pour un binaire produit autrement, et elle n'est
offerte nulle part dans l'interface.

Le **SHA-256 et la taille** sont calculés ici, sur le fichier réellement écrit,
et jamais repris d'une saisie : c'est l'empreinte que la carte vérifie **avant
de basculer** de partition.

---

## 0 bis. Ce que la version 1.1 avait corrigé

Deux défauts constatés en conditions réelles, sur le mur.

### Le bouton allumait, et n'éteignait jamais

Un bouton était lié à **une** commande. Le Shelly d'essai expose `#1560 Allumer`,
`#1561 Éteindre`, `#1562 Basculer` et `#1556 État` : le bouton était câblé sur
« Allumer », et il faisait donc exactement son travail. Le défaut n'était pas
dans le code, il était dans le modèle.

Un bouton porte désormais un **mode** :

| Mode | Ce qu'il fait | Ce qu'il faut remplir |
|---|---|---|
| `action` | joue toujours la même chose | une commande d'action, ou un scénario |
| `toggle` | lit l'état, puis joue la commande **inverse** | « Allumer », « Éteindre », et une commande d'état |

Un bouton `toggle` **sans commande d'état est refusé à la sauvegarde**, en
nommant l'emplacement fautif : sans état, rien ne permet de décider du sens, et
le bouton referait exactement ce que faisait la 1.0.

La commande « Basculer » de l'équipement est acceptée comme configuration
alternative, mais elle n'est jouée **qu'en secours** — quand l'état devient
illisible. Le choix explicite d'après l'état reste le chemin normal : une
commande « basculer » désynchronisée, parce qu'un relais a été actionné à la
main pendant que Jeedom ne regardait pas, inverse l'état affiché sur l'écran, et
l'écart ne se rattrape jamais. « Allumer quand c'est éteint » converge, lui,
quoi qu'il se soit passé entre-temps.

### `lastcontact` restait vide — la cause

`noteContact()` n'écrivait l'horodatage que dans la **commande d'information**
« Dernier contact ». La **configuration de l'équipement**, elle, n'a jamais rien
reçu : il n'y a jamais eu de `setConfiguration('lastcontact', …)` nulle part.
`getConfiguration('lastcontact')` rendait donc une chaîne vide sur un écran qui
dialoguait parfaitement — ce n'était pas une écriture qui échouait, c'était une
écriture qui n'existait pas.

Vérifié avant correction sur `eqLogic #453` : la commande portait bien
`2026-09-22 11:03:05`, et `getConfiguration('lastcontact')` rendait `''`.

C'est maintenant écrit **aux deux endroits**, et la configuration est la source
que le plugin consulte lui-même.

---

## 1. Architecture

### Le modèle

Un **eqLogic = un écran physique**, reconnu à l'adresse MAC de sa carte. Le parc
est multi-écrans par construction : autant d'équipements que d'écrans, chacun
avec sa MAC, ses boutons et **son propre compteur de version**. Le firmware est
identique sur toutes les cartes — une carte découvre son identité en lisant sa
propre MAC au démarrage.

Tout tient dans la configuration de l'eqLogic, il n'y a pas de table dédiée :

| Clé de configuration | Contenu |
|---|---|
| `mac` | adresse MAC normalisée : 12 caractères hexadécimaux minuscules |
| `poll` | intervalle conseillé à la carte, borné entre 5 et 3600 s |
| `version` | le compteur que la carte surveille |
| `layout_signature` | empreinte de la mise en page, qui décide si `version` bouge |
| `lastcontact` | horodatage du dernier appel reçu, à la minute — **ajouté en 1.1** |
| `ota_allowed` | le verrou OTA de cet écran, fermé par défaut — **ajouté en 1.2** |
| `fw` | la version annoncée par la carte, écrite seulement quand elle change — **ajouté en 1.2** |
| `buttons` | la liste des six emplacements |

Et, dans la configuration **du plugin** (`config::byKey(…, 'glowscreen32')`),
ajoutées en 1.2 : `ota_enabled`, le verrou global, et les métadonnées du
firmware déposé — `firmware_file`, `firmware_version`, `firmware_sha256`,
`firmware_size`, `firmware_date`. Elles vivent en base et le binaire sur le
disque : les deux peuvent diverger — un fichier effacé à la main — et c'est
`exists`, recalculé à chaque lecture, qui décide d'annoncer ou non une mise à
jour. Un firmware annoncé dont le fichier a disparu vaut `firmware_unavailable`,
pas une URL morte.

Le `logicalId` de l'eqLogic reçoit la MAC normalisée : c'est la clé de recherche
de l'API, et c'est un index plutôt qu'un parcours. `glowscreen32::byMac()` a
cependant un repli par balayage, pour les équipements restaurés sans logicalId —
sans lui, une carte parfaitement configurée se verrait répondre
`unknown_device` jusqu'à ce que quelqu'un pense à réenregistrer l'équipement.

Un bouton, tel qu'il est stocké en 1.1 :

```php
array(
    'label'    => 'Lampe',       // 24 caractères au plus, sans balise
    'color'    => '#9b59b6',      // normalisée, #abc accepté et développé
    'icon'     => 'bulb',         // texte libre en v1
    'mode'     => 'toggle',       // 'action' ou 'toggle'
    // --- mode action ---
    'target'   => '',             // '', 'cmd' ou 'scenario'
    'cmd'      => '',             // commande d'action à déclencher
    'scenario' => 0,              // id du scénario, si target = scenario
    // --- mode toggle ---
    'on'       => '#1560#',       // la commande qui allume
    'off'      => '#1561#',       // la commande qui éteint
    'toggle'   => '#1562#',       // « basculer » — secours, facultatif
    // --- commun ---
    'state'    => '#1556#',       // commande d'information portant l'état
)
```

Un bouton enregistré par la 1.0 n'a pas de clé `mode` : il vaut `action`, ce qui
est exactement ce qu'il faisait. **Aucune configuration existante ne change de
comportement à la mise à jour** ; il faut repasser volontairement un bouton en
interrupteur pour qu'il se mette à éteindre.

Le cœur convertit lui-même les champs `cmd`, `on`, `off`, `toggle` et `state`
entre `#[Objet][Équipement][Commande]#` à l'affichage et `#id#` à
l'enregistrement (`jeedom::toHumanReadable` / `fromHumanReadable`,
`core/ajax/eqLogic.ajax.php`) : un renommage ultérieur ne casse donc pas un
bouton. Les couleurs `#9b59b6` ne sont pas touchées par cette conversion, dont
l'expression régulière exige un dièse fermant.

### `id` est un rang — le changement structurant de la v1.3

Jusqu'en v1.2, `id` était l'**identifiant de commande Jeedom**, et la carte le
renvoyait à `press` pour dire quoi exécuter. C'était une erreur de conception :
la carte décidait de *quelle commande* déclencher, alors qu'elle n'a aucun moyen
de savoir laquelle est pertinente — et, en mode interrupteur, il n'y a même plus
*une* commande à désigner, il y en a deux et le choix dépend de l'état.

Désormais `id` est le **rang du bouton dans la mise en page**, de 0 à 5. La
carte le traite comme opaque, ne l'affiche jamais, le renvoie tel quel. C'est le
plugin qui résout la commande.

Bénéfice de sécurité, et il n'est pas théorique : la clé API est la même pour
tout le parc, et jusqu'en 1.0 une carte du couloir qui inventait un entier
pouvait nommer n'importe quelle commande d'action de l'installation. Le filtre
« parmi les boutons de cet écran-là » la protégeait déjà, mais désormais
l'espace des valeurs acceptées est **0 à 5**, et rien d'autre n'existe.

Un rang hors de la mise en page rend `unknown_button` (404) — y compris un rang
négatif, et y compris l'ancien identifiant de commande `1560`, vérifié.

La convention de l'**identifiant négatif pour les scénarios** (`-2` pour le
scénario 2), inventée en 1.0 faute de place dans le contrat, **disparaît** :
elle n'a plus d'objet puisqu'un bouton-scénario porte un rang comme les autres.
C'était la remarque n° 1 du § 7 de la version précédente de ce document ; elle
est close.

### Le compteur de version

Calculé dans `preSave()`, **avant** l'écriture, pour ne pas avoir à
réenregistrer l'équipement depuis `postSave()`. Il n'augmente que si l'empreinte
de la mise en page — nom, `poll`, boutons — a réellement changé : un
enregistrement qui ne touche qu'une catégorie ou un commentaire ne doit pas
faire redessiner tous les écrans de la maison alors qu'ils afficheraient la même
chose.

En 1.1, l'empreinte ne retient **que les champs qui comptent pour le mode du
bouton** : une commande « Éteindre » choisie puis le bouton repassé en mode
action ne fait pas redessiner la maison pour un champ que plus personne ne lit.
Conséquence attendue de ce changement de calcul : le **premier** enregistrement
après la mise à jour incrémente la version une fois, et une seule.

### L'état d'un bouton, et le tableau `states`

Deux sources, dans cet ordre :

1. la commande d'information **désignée explicitement** dans le champ d'état.
   C'est elle qui fait foi ;
2. à défaut, le lien que Jeedom pose lui-même entre une commande d'action et son
   état (champ `value` de la commande d'action), celui dont le cœur se sert pour
   allumer une tuile de dashboard. Ce n'est pas une heuristique : c'est une
   déclaration faite ailleurs dans Jeedom, par le plugin qui a créé la lampe.

En mode `toggle`, la source 1 est **obligatoire** : c'est elle qui décide du
sens, et la déduction ne suffit pas pour une décision qui allume ou éteint pour
de bon.

Une commande désignée explicitement mais devenue introuvable **ne retombe pas**
sur la déduction : l'utilisateur a dit ce qu'il voulait, afficher autre chose à
sa place serait pire que de n'afficher rien.

Seul un sous-type **binaire** donne un état. Une consigne de température ou un
volet à 40 % rend `null`, ce que le contrat prévoit.

`layout()`, `states()` et `press()` parcourent tous les trois `activeButtons()`,
le même filtre : c'est ce qui garantit que `states` du ping est dans le même
ordre que `buttons` du layout, **et** que le rang reçu par `press` désigne le
bouton que la carte a dessiné. Trois parcours séparés, même filtrés de la même
façon, finiraient un jour par diverger, et le symptôme serait un appui sur la
lampe qui ouvre le portail.

### Ce que `press` décide

```
état lu avant  →  commande jouée        →  état attendu
     1                « Éteindre »               0
     0                « Allumer »                1
    null              « Basculer » si elle existe, sinon « Allumer »
```

Les deux derniers cas journalisent un avertissement : jouer à l'aveugle est un
incident, même quand c'est le moins mauvais choix disponible.

`press` répond `{"ok":true,"id":0,"state":1,"pending":true}`. `state` est l'état
**attendu**, pas constaté ; `pending` vaut vrai tant que l'état relu ne confirme
pas ce qu'on annonce. La règle est unique pour les deux modes : en mode `action`
le plugin n'a rien à prédire — il rend ce qu'il lit — donc `pending` y vaut
toujours faux. Le firmware s'en sert pour un retour visuel optimiste immédiat ;
la source de vérité reste le `states` du `ping` suivant.

C'était la remarque n° 2 du § 7 de la version précédente de ce document ; elle
est close, et inscrite au contrat.

### Le dernier contact, et pourquoi à la minute

`noteContact()` écrit à deux endroits :

- la **configuration** de l'équipement, qui est la source consultée par le
  plugin lui-même (`getConfiguration('lastcontact')`, `lastContact()`,
  `contactAge()`, `isOnline()`, `overview()`) ;
- la **commande d'information** « Dernier contact », pour que l'écran reste un
  équipement ordinaire sur le dashboard et qu'un scénario puisse réagir à sa
  disparition.

L'écriture passe par `save(true)` : **écriture directe**, sans `preSave()` ni
`postSave()`. Un ping ne doit ni recalculer la signature de mise en page, ni
risquer de faire bouger le compteur de version, ni recréer les commandes, ni
écrire une ligne de journal. Il ne doit poser qu'une date.

Et il ne la pose **qu'une fois par minute**. Une carte interroge toutes les
trente secondes ; avec dix écrans, horodater chaque appel ferait six cents
écritures par heure pour une information dont personne ne lit la seconde. Un
**appui**, lui, est rare et intéressant : il est toujours écrit, et il
accompagne de toute façon l'écriture de « Dernier bouton ».

Vérifié : douze pings en seize secondes ne produisent aucune écriture, et la
carte réelle posée sur le mur en produit une toutes les minutes environ (§ 5).

`isOnline()` tolère **trois** intervalles de rafraîchissement, plus la
granularité : un ping perdu et un autre en retard ne doivent pas faire clignoter
« hors ligne » sur une installation saine.

### La décision d'OTA, et pourquoi elle est journalisée

`otaDecision()` examine **les deux verrous d'abord**, et **tous les deux** —
avant la comparaison de versions, avant l'existence du fichier. Un journal qui
ne nommerait que le premier verrou fermé ferait rouvrir l'un des deux, réessayer,
et recommencer.

L'ordre est donc : verrous → firmware déposé → version plus récente → fichier
présent. Chaque sortie laisse une ligne, avec la version annoncée, la réponse et
la cause :

```
[INFO] [Test GlowScreen] : OTA refusé — la carte annonce 1.3.0, bloqué par
       le verrou global du plugin (ota_enabled) et par le verrou de cet écran
       (ota_allowed). Réponse : update=false.
[INFO] [Test GlowScreen] : OTA accordé — la carte annonce 1.3.0, le firmware
       1.4.0 lui est proposé (1002288 octets, sha256 0fbb3369…).
```

C'est la seule chose qui réponde à « pourquoi cet écran-là ne se met pas à
jour ? » : la carte, elle, reçoit la même réponse que si elle était à jour, et
ne peut donc rien en dire.

La comparaison passe par `version_compare()` et non par une comparaison de
chaînes : `1.10.0` est postérieur à `1.9.0`, et lui est *inférieur* en ASCII.
Conséquence assumée : un binaire plus ancien que ce que la carte exécute n'est
jamais proposé. Redescendre de version demande donc de reflasher par USB — le
retour arrière automatique de l'ESP32 (partition précédente) reste le vrai filet.

### La version de la carte, retenue sans frais

`noteFirmware()` est appelée pour **toute** action qui porte un `fw`, y compris
un `ping`. Elle n'écrit que si la version a **changé** : une carte l'annonce à
chaque interrogation, et réécrire la même chaîne toutes les trente secondes
ferait le même gâchis que d'horodater chaque ping (§ 1, « Le dernier contact »).
Comme `noteContact()`, l'écriture passe par `save(true)` — directe, sans
`preSave()` ni `postSave()` : dire sa version ne doit ni recalculer la signature
de mise en page, ni faire bouger le compteur que la carte surveille.

Le verrou `ota_allowed` n'entre **pas** dans la signature de mise en page :
autoriser un écran à se mettre à jour ne change rien à ce qu'il affiche, et ne
doit donc pas le faire redessiner. Vérifié § 5.9 — la version reste à 7 des deux
côtés d'un enregistrement qui bascule le verrou.

### Le cloisonnement

`press` cherche le rang reçu **parmi les boutons de cet écran-là**, jamais dans
toute la base. Sans ce filtre, la clé API — qui est la même pour tout le parc —
laisserait un écran du couloir commander ce qui est configuré ailleurs. Depuis
la v1.3, le cloisonnement est même structurel : il n'y a plus d'espace de noms
commun à désigner, seulement six rangs locaux.

---

## 2. Arborescence

```
jeedom-plugin-glowscreen32/
├── plugin_info/
│   ├── info.json                    carte d'identité (id, catégorie organization)
│   ├── install.php                  crée la clé API à l'installation
│   ├── configuration.php            URL, clé, en-tête, et le tableau du parc
│   ├── glowscreen32_icon.png        dessinée par tools/make-icon.php
│   └── .htaccess                    Deny from all, SAUF les images (voir § 2.1)
├── core/
│   ├── class/
│   │   ├── glowscreen32.class.php   eqLogic + glowscreen32Cmd (obligatoire)
│   │   └── .htaccess                Deny from all
│   ├── php/
│   │   └── api.php                  LE point d'entrée des cartes — PAS de .htaccess ici
│   ├── ajax/
│   │   └── glowscreen32.ajax.php    contrôleur de la page (session admin)
│   └── i18n/
│       ├── en_US.json
│       └── .htaccess
├── desktop/
│   ├── php/glowscreen32.php         page de configuration
│   └── js/glowscreen32.js
├── docs/{fr_FR,en_US}/{index.md,changelog.md}
├── data/
│   ├── .htaccess                    Deny from all
│   └── firmware/
│       ├── .htaccess                rouvre les .bin, et EUX SEULS (§ 2.2)
│       └── glowscreen32-1.4.0.bin   déposé par l'utilisateur, JAMAIS dans git
├── tests/check-classes.php          contrôles par réflexion (non déployé)
├── tools/make-icon.php              (non déployé)
├── .github/workflows/work.yml       workflow réutilisable Jeedom
├── .gitignore, .deployignore, LICENSE (AGPL-3.0), README.md
```

Le binaire déposé ne vient pas du dépôt et n'y retourne pas : `.deployignore`
exclut `data/firmware/*.bin` du `rsync --delete`, et `.gitignore` le tiendrait de
toute façon hors de git. Le `.htaccess` du même dossier, lui, fait partie du
plugin et doit être déployé — d'où un motif qui ne vise que les binaires.

**Il n'y a pas de `.htaccess` dans `core/php/`**, et il ne doit jamais y en
avoir : il fermerait le point d'entrée des cartes. `tests/check-classes.php` le
vérifie à chaque exécution.

### 2.1 Un défaut trouvé en chemin : l'icône du plugin renvoyait 403

`plugin_info/.htaccess` portait `Deny from all` sans exception. Les quatorze
autres plugins de l'installation portent tous, à la ligne près :

```apache
Order allow,deny
<Files ~ "\.(jpg|jpeg|png|gif|pdf|txt|bmp)$">
   allow from all
</Files>
Deny from all
```

Sans cette exception, `glowscreen32_icon.png` est refusée par Apache : la
vignette du plugin ne s'affiche nulle part, et chaque affichage dépose une ligne
`AH01797 client denied by server configuration` dans `log/http.error` — quatre y
étaient déjà. L'exception est reprise telle quelle. Vérifié après correction :

```
icône du plugin                      HTTP 200
plugin_info/configuration.php        HTTP 403   (toujours refusé, c'est le but)
```

C'est une correction **dans le plugin** : aucun `.htaccess` hors du plugin n'a
été touché, et la configuration d'Apache n'a pas bougé.

### 2.2 Le même défaut, prévu cette fois : `data/firmware/.htaccess`

`data/.htaccess` porte `Deny from all`, sans exception. Le firmware est
téléchargé par une **carte**, qui n'est pas un client authentifié d'Apache : sans
exception, elle reçoit un 403 au milieu de sa mise à jour, et `log/http.error`
une ligne `AH01797 client denied by server configuration`.

`data/firmware/.htaccess` reprend donc la forme du § 2.1, mais pour les `.bin` :

```apache
Order allow,deny
<Files ~ "\.bin$">
   allow from all
</Files>
Deny from all
```

Vérifié après déploiement :

```
data/firmware/glowscreen32-1.4.0.bin   HTTP 200   (1 002 288 octets)
data/firmware/.htaccess                HTTP 403
data/.htaccess                         HTTP 403
plugin_info/configuration.php          HTTP 403
```

Le `.htaccess` d'un sous-dossier l'emporte bien sur celui du parent : l'accès
reste refusé partout ailleurs sous `data/`, et le binaire, lui, est servi. C'est
une correction **dans le plugin** — aucun `.htaccess` hors du plugin n'a été
touché, et la configuration d'Apache n'a pas bougé.

### Les deux pièges de `STRUCTURE-PLUGIN-JEEDOM.md`

Vérifiés par réflexion sur le source, à chaque `php tests/check-classes.php` :

- **aucune propriété sans souligné** dans les classes eqLogic et cmd —
  `DB::save()` les prendrait pour des colonnes SQL et la création d'un
  équipement échouerait sur « Unknown column ». Le plugin n'en déclare d'ailleurs
  aucune : tout l'état vit dans la configuration de l'eqLogic ;
- **aucune méthode `set` + clé de formulaire**, et en particulier pas de
  `setCmd()` — `utils::a2o()` l'appellerait à l'enregistrement. Les méthodes du
  plugin s'appellent `buttons()`, `layout()`, `states()`, `press()`,
  `pressToggle()`, `pressScenario()`, `noteContact()`, `lastContact()`,
  `activeButtons()`, `checkButtons()` : aucune ne commence par `set`.

Le contrôle a été **étendu en 1.1** à trois vérifications de plus, toutes du
même genre — des défauts qui ne se voient ni au `php -l`, ni à la relecture :

- `plugin_info/.htaccess` doit contenir `allow from all`, faute de quoi l'icône
  est de nouveau refusée ;
- le source doit contenir `MODE_TOGGLE`, `MODE_ACTION`, `pressToggle` et
  `checkButtons` : sans eux, le mode de bouton n'est plus mis en œuvre et le
  bouton se remet à n'allumer que ;
- `noteContact()` doit contenir `setConfiguration('lastcontact'`, faute de quoi
  `getConfiguration('lastcontact')` rendra de nouveau une chaîne vide.

Et **étendu de nouveau en 1.2**, à ce qui protège l'OTA. Ces quatre contrôles
visent des fautes qui laisseraient un plugin qui marche, qui passe le `php -l`,
et qui pousse un firmware sur tout le parc à la première occasion :

- `ota_enabled`, `ota_allowed`, `otaDecision`, `otaAllowed`, `otaEnabled`,
  `publishFirmware` et `noteFirmware` doivent exister dans le source ;
- **`otaDecision()` doit consulter les deux verrous**, `self::otaEnabled()` *et*
  `$this->otaAllowed()`, dans le corps de la même méthode — extrait par une
  expression régulière. C'est le contrôle qui compte : rendre l'un des deux
  facultatif ne se verrait nulle part ailleurs ;
- `.deployignore` doit contenir `data/firmware/*.bin`, sans quoi le firmware
  déposé disparaît au prochain déploiement ;
- `data/firmware/.htaccess` doit exister et contenir `allow from all` et `.bin`,
  sans quoi la carte reçoit un 403 au milieu de sa mise à jour ;
- **`ESP_APP_DESC_MAGIC`, `ESP_APP_DESC_OFFSET`, `ESP_APP_VERSION_OFFSET`,
  `ESP_APP_VERSION_LENGTH` et `imageVersion` ne doivent plus exister** dans le
  source, et `FIRMWARE_MARKER` / `markerVersion` doivent y être. C'est le
  contrôle qui empêche la panne du § 0.3 de revenir : lire de nouveau la version
  dans le descripteur ESP-IDF donnerait un plugin qui marche, qui passe tous les
  autres contrôles, et dont l'OTA ne se déclenche jamais. Le préfixe littéral
  `GLOWSCREEN32-FW:` est vérifié tel quel — il est gravé en face, dans
  `src/fw_version.cpp`, et les deux côtés doivent dire la même chose ;
- `publishFirmware()` doit appeler `markerVersion()` **et** vérifier
  `ESP_IMAGE_MAGIC` dans le corps de la même méthode : garder les fonctions sans
  les appeler laisserait passer n'importe quel fichier.

Le jeu d'essai (§ 5) enregistre les équipements **par le chemin de production** —
`jeedom::fromHumanReadable()` puis `utils::a2o()` puis `save()`, exactement comme
`core/ajax/eqLogic.ajax.php` — avec une clé `cmd` dans le formulaire. C'est ce
chemin, et lui seul, qui déclenche le piège `setCmd()`.

---

## 3. Déploiement

Le dépôt est la source de vérité ; `/var/www/html/plugins/glowscreen32` n'en est
qu'une copie, écrasée à chaque déploiement.

```bash
# Une seule fois, le dossier cible doit préexister
sudo install -d -o www-data -g www-data -m 775 /var/www/html/plugins/glowscreen32

# À chaque modification
sudo /home/<utilisateur>/dev/tools/deploy-plugin.sh /home/<utilisateur>/dev/jeedom-plugin-glowscreen32
```

`sudo` n'est pas facultatif : sans lui, les nouveaux fichiers ne reçoivent pas le
groupe `www-data`. **Une modification non déployée est invisible** — le
navigateur comme les cartes ne lisent que la copie.

Le déploiement exclut `tools/`, `tests/` (par `.deployignore`), `.git/`,
`.github/` et les documents de travail à la racine. Il préserve les journaux et
`core/config/common.config.php` côté Jeedom.

Contrôles avant déploiement :

```bash
cd /home/<utilisateur>/dev/jeedom-plugin-glowscreen32
for f in $(find . -name '*.php' -not -path './.git/*'); do php -l $f; done
php tests/check-classes.php
```

Les deux sont passés propres à chaque déploiement de la 1.1.

---

## 4. Configurer un écran

1. **Plugins → Organisation → GlowScreen32 → Ajouter un écran**, nommé d'après la
   pièce : ce nom s'affiche dans le bandeau de l'écran.
2. **Adresse MAC**, onglet Écran. Avec ou sans séparateurs, en majuscules ou en
   minuscules : `24:6F:28:12:34:56`, `24-6f-28-12-34-56` et `246f28123456`
   désignent la même carte. Un doublon est refusé en nommant l'écran qui porte
   déjà l'adresse.
3. **Onglet Boutons** : six emplacements, dans l'ordre de la grille 3×2.
   Libellé, couleur, icône, puis le **mode** :
   - **Action simple** — le champ « Déclenche » apparaît : une commande d'action
     choisie dans le sélecteur de Jeedom, ou un scénario. Le champ d'état, alors
     nommé « Pastille allumée si », reste facultatif.
   - **Interrupteur** — trois champs apparaissent à la place : « Allumer »,
     « Éteindre », et « Basculer » (facultatif, et documenté comme un secours).
     Le champ d'état devient « État (obligatoire) » : sans lui, la sauvegarde
     est refusée.

   Les champs de l'autre mode sont **masqués**, pas seulement grisés : un
   formulaire qui affiche « Allumer », « Éteindre » *et* « Déclenche » laisse
   croire qu'il faut tout remplir, et l'utilisateur remplit alors celui qui ne
   sert pas.

   Le sélecteur de commande ne propose que ce qui convient au champ : type
   *action* pour ce qui fait quelque chose, type *info* pour ce qui dit quelque
   chose. Un aperçu, à droite, montre l'écran tel que la carte le dessinera,
   avec **le rang de chaque tuile** et un `⇄` sur les interrupteurs.
4. **Sauvegarder.** La version augmente, la carte redessine à sa prochaine
   vérification.

Un emplacement sans cible n'est pas envoyé à la carte : il ne laisse pas de case
morte, les boutons suivants remontent — et les rangs se resserrent avec eux.

L'onglet Écran affiche aussi le **dernier contact** en clair, et la page du
plugin un tableau du parc, dès le premier écran (§ 4.1).

Le bouton **Voir ce que la carte reçoit** affiche la réponse `layout` exacte,
telle qu'elle est servie à l'instant — le seul moyen de s'apercevoir qu'un bouton
a disparu parce que sa commande a été supprimée, le formulaire, lui, n'ayant pas
changé d'apparence.

### 4.1 Le dernier contact dans la page

Affiché en clair, au format `22/09/2026 11:22:08 (il y a 54 s)`, à deux
endroits :

- **onglet Écran**, cadre « État », sous la version de la mise en page ;
- **tableau du parc**, en bas de la liste des écrans et dans la page de
  configuration du plugin, avec une étiquette **hors ligne** au-delà de trois
  intervalles de rafraîchissement sans nouvelle, et **jamais vu** pour un écran
  qui n'a jamais appelé.

Le tableau s'affiche désormais **dès le premier écran** : savoir si la carte
qu'on vient de flasher a parlé au plugin est la première question qu'on se pose,
et on se la pose avec un seul écran.

### 4.2 Déposer un firmware, et ouvrir les verrous

Tout se passe sur la **page du plugin**, dans le cadre « Firmware (mise à jour
par le réseau) », sous le tableau du parc — c'est là qu'on regarde les écrans
revenir en ligne, et un interrupteur d'arrêt d'urgence doit être sous la main de
celui qui regarde.

1. **Parcourir**, choisir le `.bin` produit par PlatformIO
   (`.pio/build/cyd/firmware.bin`), puis **Déposer**. Un binaire qui ne porte
   pas le marqueur `GLOWSCREEN32-FW:` est **refusé** — y compris l'outil de
   calibration, qui n'a donc aucun moyen de partir sur un écran. Le fichier
   part en
   `multipart/form-data` vers le contrôleur AJAX du plugin, qui exige une session
   d'administrateur — un `POST` sans session rend
   `401 - Accès non autorisé` (vérifié § 5.8).
2. Le cadre se remplit **sans recharger la page** : version, nom de fichier,
   taille, **SHA-256**, URL publique, date de dépôt. Un dépôt remplace le
   précédent — il n'y a jamais qu'un firmware proposé à la fois, et l'ancien
   binaire est effacé *après* que le nouveau est en place, jamais avant.
3. **Ouvrir le verrou global** : la case « Autoriser les mises à jour par le
   réseau ». Elle prend effet **tout de suite**, sans bouton « Sauvegarder » : un
   arrêt d'urgence qui demande une confirmation n'en est pas un. La colonne
   « Verrou OTA » du tableau du parc bascule d'un coup sur toutes les lignes —
   elle montre le ET des deux verrous, c'est-à-dire ce que la carte recevra, et
   non ce qui est coché quelque part.
4. **Ouvrir un seul écran témoin** : onglet *Écran* de l'équipement, cadre
   « Mise à jour par le réseau », case « Verrou de cet écran », puis
   *Sauvegarder*. Le même cadre affiche la version que la carte a annoncée.
5. Attendre que le tableau du parc montre cet écran **revenu en ligne avec sa
   nouvelle version**, puis ouvrir les suivants.

Si quelque chose tourne mal : **refermer le verrou global**. Les écrans restés
en arrière reçoivent `update: false` dès leur appel suivant.

Déposer ne déverrouille rien, et c'est la marche à suivre : on dépose d'abord,
on ouvre ensuite. Le journal note le dépôt en précisant l'état du verrou global
— « déposé […] Verrou global : fermé — aucun écran ne le recevra ».

### Où trouver la clé API

**Réglages → Système → Configuration → onglet API**, ligne *GlowScreen32*. Elle
est aussi affichée, avec l'URL et le nom de l'en-tête, dans le cadre « Ce qu'il
faut donner à la carte » de l'onglet Écran, et dans la page de configuration du
plugin.

La clé est **la même pour tout le parc** : le firmware étant identique sur toutes
les cartes, il n'y a qu'une clé à y inscrire. La changer impose de la reporter
dans chaque carte, qui n'a aucun moyen de la redemander.

Sur cette installation, à ce jour — **inchangée par la 1.1 comme par la 1.2** :

```
URL     http://192.168.1.10/plugins/glowscreen32/core/php/api.php
En-tête X-GLOWSCREEN32-APIKEY
Clé     <clé API du plugin — Jeedom > Plugins > GlowScreen32>
```

---

## 5. Vérification — réponses réelles

Écran d'essai : **Test GlowScreen** (`eqLogic #453`, MAC de référence
`24:6f:28:12:34:56`), la carte réellement posée sur le mur.

**Un seul équipement réel a été actionné : la lampe
extérieure de test (`eqLogic #84`, commandes 1556/1560/1561/1562).**
Aucun autre. La lampe extérieure de test a été laissée dans l'état où elle a été trouvée
(allumée).

### 5.1 La sauvegarde refuse ce qui ne peut pas fonctionner

Trois enregistrements par le chemin de production, trois refus :

```
Interrupteur SANS commande d'état :
  REFUSÉ — Bouton 1 « Lampe » : un interrupteur a besoin d'une commande d'état.
  Sans elle, rien ne permet de décider s'il faut allumer ou éteindre — désignez
  la commande d'information qui dit si l'équipement est allumé, ou repassez le
  bouton en « Action simple ».

Interrupteur avec « Allumer » seule :
  REFUSÉ — Bouton 1 « Lampe » : il manque la commande « Éteindre ». Avec une
  seule des deux, le bouton ne va que dans un sens — c'est exactement ce que le
  mode interrupteur corrige.

Commande d'ACTION glissée dans le champ d'état :
  REFUSÉ — Bouton 1 « Lampe » : la commande d'état doit être une commande
  d'information, celle qui DIT si l'équipement est allumé.
```

Le message nomme l'emplacement fautif : dans un formulaire de six lignes qui se
ressemblent toutes, c'est la seule information qui sert.

### 5.2 Le cycle allumer / éteindre — deux appuis sur le même bouton

Mise en page : un interrupteur au rang 0 (`#1560` / `#1561` / `#1562`, état
`#1556`), et une action simple au rang 1 (`#1560 Allumer`).

```
### layout
HTTP 200
{"ok":true,"device":"246f28123456","name":"Test GlowScreen","version":6,"poll":30,
 "buttons":[{"id":0,"label":"Lampe","color":"#9b59b6","icon":"bulb","mode":"toggle","state":1},
            {"id":1,"label":"Lampe ON","color":"#2d7ff9","icon":"bulb","mode":"action","state":1}]}

   lampe #1556 (État lampe test) = 1        ← allumée au départ

### press id=0   (1er appui)
HTTP 200
{"ok":true,"id":0,"state":0,"pending":true}    ← le plugin a joué « Éteindre »

   lampe #1556 = 0                             ← 3 s plus tard, elle est éteinte

### ping
HTTP 200
{"ok":true,"version":6,"time":1790068565,"states":[0,0]}

### press id=0   (2e appui, le MÊME bouton)
HTTP 200
{"ok":true,"id":0,"state":1,"pending":true}    ← le plugin a joué « Allumer »

   lampe #1556 = 1

### ping
HTTP 200
{"ok":true,"version":6,"time":1790068568,"states":[1,1]}

### press id=1   (le bouton en mode action)
HTTP 200
{"ok":true,"id":1,"state":1,"pending":false}   ← rien à prédire : pending faux

   lampe #1556 = 1
```

Le défaut de la 1.0 est corrigé : le même bouton allume puis éteint, et l'état
lu entre les deux le confirme.

`pending` vaut `true` sur les deux appuis en mode `toggle` : l'état relu juste
après l'exécution est encore l'ancien, le Shelly parlant par MQTT et remontant
sa nouvelle valeur une seconde plus tard. C'est précisément ce que le champ est
là pour dire. En mode `action` il vaut `false` : le plugin ne prédit rien, il
rend ce qu'il lit.

### 5.3 La configuration finale, et son journal

L'écran est laissé avec **un seul bouton, interrupteur sur la lampe extérieure de test** :

```
[{"label":"Lampe","color":"#9b59b6","icon":"bulb","mode":"toggle","target":"","cmd":"",
  "scenario":0,"on":"#1560#","off":"#1561#","toggle":"#1562#","state":"#1556#"}]

### layout
{"ok":true,"device":"246f28123456","name":"Test GlowScreen","version":7,"poll":30,
 "buttons":[{"id":0,"label":"Lampe","color":"#9b59b6","icon":"bulb","mode":"toggle","state":1}]}
```

Trois appuis successifs, le journal abaissé à *debug* le temps du contrôle :

```
   lampe #1556 = 0
### press id=0  →  {"ok":true,"id":0,"state":1,"pending":true}     lampe = 1
### press id=0  →  {"ok":true,"id":0,"state":0,"pending":true}     lampe = 0
### press id=0  →  {"ok":true,"id":0,"state":1,"pending":true}     lampe = 1
### press id=3  →  HTTP 404 {"ok":false,"error":"unknown_button"}

[2026-09-22 11:20:38][INFO] [Aucun][Test GlowScreen] : appui sur le bouton 0 « Lampe » → Allumer.
[2026-09-22 11:20:41][INFO] [Aucun][Test GlowScreen] : appui sur le bouton 0 « Lampe » → Éteindre.
[2026-09-22 11:20:44][INFO] [Aucun][Test GlowScreen] : appui sur le bouton 0 « Lampe » → Allumer.
[2026-09-22 11:20:47][INFO] API : unknown_button (HTTP 404) depuis 192.168.1.10
                              — [Aucun][Test GlowScreen] : aucun bouton de rang 3 dans la mise en page
```

Le journal dit **quelle commande a finalement été choisie**. C'est l'information
qui compte quand un interrupteur part dans le mauvais sens : le rang seul ne
l'aurait pas dit.

Le niveau de journal a été **remis à son défaut** après le contrôle (`400`,
erreurs seulement).

### 5.4 Un rang hors de la mise en page

Sur la mise en page à deux boutons :

```
### press id=5      (la mise en page n'en compte que 2)   HTTP 404  unknown_button
### press id=2                                            HTTP 404  unknown_button
### press id=-1                                           HTTP 404  unknown_button
### press id=1560   (l'ANCIEN id de commande)             HTTP 404  unknown_button
### press id=abc                                          HTTP 400  bad_request
### press sans id                                         HTTP 400  bad_request
### mauvaise clé                                          HTTP 401  bad_apikey
### device inconnu                                        HTTP 404  unknown_device
```

La quatrième ligne est celle qui compte : `1560` est l'identifiant Jeedom de la
commande « Allumer », celui qu'une carte de la génération v1.2 enverrait. Il
n'ouvre plus rien.

Un `id` absent ou non numérique reste `bad_request` : c'est un défaut de
programmation du firmware, et le réessayer trois fois avec backoff n'y changera
rien. Un rang bien formé mais hors mise en page est autre chose — une carte qui
a gardé en cache une mise en page devenue plus courte — et c'est
`unknown_button`.

### 5.5 `lastcontact`, avant et après

Avant correction, sur le même équipement qui dialoguait parfaitement :

```
conf lastcontact = []
cmd #5463 logicalId=lastcontact  value=[2026-09-22 11:03:05]
```

Après :

```
getConfiguration('lastcontact')       = [2026-09-22 11:20:07]
lastContact()                         = [2026-09-22 11:20:07]
humanContact()                        = [22/09/2026 11:20:07 (il y a 3 s)]
contactAge()                          = 3        isOnline() = true
commande « Dernier contact »          = [2026-09-22 11:20:07]
```

Et la granularité tient — douze pings en seize secondes, une seule valeur :

```
t        lastcontact
11:17:19 2026-09-22 11:16:45
11:17:20 2026-09-22 11:16:45
…                                    (douze lignes, aucune écriture)
11:17:35 2026-09-22 11:16:45
```

Puis, sans aucun ping de notre part, la carte réelle posée sur le mur écrit
d'elle-même à l'expiration de la minute :

```
11:18:06 2026-09-22 11:16:45
11:18:08 2026-09-22 11:18:07    ← une écriture, 82 s après la précédente
11:18:59 2026-09-22 11:18:07    (et plus rien pendant la minute qui suit)
```

Le parc, tel que les pages l'affichent :

```
#454 Fictif GlowScreen  mac=aa:bb:cc:dd:ee:ff  boutons=1  version=1
                         contact=22/09/2026 10:50:52 (il y a 32 min)   online=false
#453 Test GlowScreen    mac=24:6f:28:12:34:56  boutons=1  version=7
                         contact=22/09/2026 11:22:08 (il y a 54 s)     online=true
```

L'écran Fictif est un faux écran de test : aucune carte ne porte cette MAC,
il est donc légitimement `hors ligne`. C'est exactement ce que le champ doit
dire.

### 5.6 Les deux pages rendues hors navigateur

Rendues avec une session d'administrateur forgée, pour s'assurer qu'aucune n'a
d'erreur fatale — une page cassée ne laisse qu'une fenêtre blanche, et la cause
ne va que dans `log/http.error` :

```
desktop/php/glowscreen32.php   : OK, 25 232 octets
plugin_info/configuration.php  : OK,  5 859 octets
```

Les deux contiennent bien « Dernier contact », « hors ligne », le rang sur les
tuiles de l'aperçu, et les dates en clair — et, depuis la 1.2, l'interrupteur
global (`cb_glowscreen32Ota`), le cadre du firmware
(`div_glowscreen32Firmware`), le bouton de dépôt, le verrou de l'écran
(`data-l2key="ota_allowed"`), la version annoncée par la carte
(`span_glowscreen32Firmware`) et les colonnes « Firmware » et « Verrou OTA ».

### 5.7 Les quatre combinaisons de verrous

> Cette série et les trois suivantes ont été menées avec une image
> **fabriquée** — en-tête ESP32 valide, contenu factice — avant que le marqueur
> de version n'existe. Elles restent valables pour ce qu'elles montrent : les
> verrous, le téléchargement, la survie au déploiement. La § 5.11 refait
> l'essentiel avec le vrai binaire.

Firmware déposé : `glowscreen32-1.4.0.bin`, 1 002 288 octets. Carte annonçant
`fw=1.3.0`. Les verrous sont posés entre chaque appel, et l'appel est le même
à chaque fois :

```
curl -H "X-GLOWSCREEN32-APIKEY: <clé>" \
     ".../api.php?action=firmware&device=246f28123456&fw=1.3.0"
```

```
ota_enabled=0  ota_allowed=0   HTTP 200  {"ok":true,"update":false}
ota_enabled=1  ota_allowed=0   HTTP 200  {"ok":true,"update":false}
ota_enabled=0  ota_allowed=1   HTTP 200  {"ok":true,"update":false}
ota_enabled=1  ota_allowed=1   HTTP 200  {"ok":true,"update":true,
                                          "version":"1.4.0",
                                          "url":"http://192.168.1.10/plugins/glowscreen32/data/firmware/glowscreen32-1.4.0.bin",
                                          "sha256":"0fbb3369b36ecb85fc24b5a76693693edff7e2090250fa53dee78285397ebf78",
                                          "size":1002288}
```

Les trois réponses négatives sont **rigoureusement identiques** : la carte ne
peut pas distinguer « je suis à jour » de « on me l'interdit ». Le journal, lui,
nomme le ou les verrous :

```
[INFO] OTA refusé — la carte annonce 1.3.0, bloqué par le verrou global du
       plugin (ota_enabled) et par le verrou de cet écran (ota_allowed).
[INFO] OTA refusé — la carte annonce 1.3.0, bloqué par le verrou de cet écran
       (ota_allowed).
[INFO] OTA refusé — la carte annonce 1.3.0, bloqué par le verrou global du
       plugin (ota_enabled).
[INFO] OTA accordé — la carte annonce 1.3.0, le firmware 1.4.0 lui est proposé
       (1002288 octets, sha256 0fbb3369…).
```

> Ces lignes sont en `info`, et le niveau de journal du plugin est à *Error* par
> défaut sur cette installation : elles n'apparaissent qu'en l'abaissant. Le
> niveau a été mis à *debug* le temps du contrôle, puis **remis à son défaut**
> (`config::remove`, vérifié à 400).

### 5.8 Le déploiement progressif, en vrai

Deux écrans déclarés. Verrou global ouvert, **un seul** écran ouvert :

```
Test    (ota_allowed=1)  {"ok":true,"update":true,"version":"1.5.0", …}
Fictif  (ota_allowed=0)  {"ok":true,"update":false}
```

Puis l'arrêt d'urgence — le verrou global refermé, rien d'autre touché :

```
Test    {"ok":true,"update":false}
```

Et le binaire, **réellement téléchargé en HTTP** comme le ferait la carte, sans
clé API puisque c'est un fichier statique :

```
$ curl -o dl.bin http://192.168.1.10/plugins/glowscreen32/data/firmware/glowscreen32-1.5.0.bin
HTTP 200   1002288 octets   type=application/octet-stream
$ sha256sum dl.bin
0cd869ce3e1ff7fb17fc55b504446e99d13bfc34b1762b44ea70a5e46cbdc051
```

**Identique** au `sha256` annoncé dans la réponse `firmware`, et à celui calculé
sur le fichier avant dépôt. C'est cette empreinte que la carte vérifie avant de
basculer de partition.

Les cas limites, dans la foulée :

```
carte déjà en 1.4.0, verrous ouverts   HTTP 200  {"ok":true,"update":false}
carte en 1.10.0 (plus récente)         HTTP 200  {"ok":true,"update":false}
binaire retiré du dépôt à la main      HTTP 404  {"ok":false,"error":"firmware_unavailable"}
binaire remis en place                 HTTP 200  {"ok":true,"update":true, …}
paramètre fw absent                    HTTP 400  {"ok":false,"error":"bad_request"}
mauvaise clé                           HTTP 401  {"ok":false,"error":"bad_apikey"}
device inconnu                         HTTP 404  {"ok":false,"error":"unknown_device"}
POST au contrôleur AJAX sans session   {"state":"error","result":"401 - Accès non autorisé"}
```

Et le refus du dépôt d'un fichier qui n'est pas une image ESP32 :

```
Ce fichier ne commence pas par l'octet 0xE9 : ce n'est pas une image
d'application ESP32. Déposer autre chose ferait écrire n'importe quoi dans la
partition inactive d'une carte, qui ne redémarrerait plus.
```

### 5.9 La version de la carte est retenue, et l'enregistrement ne la perd pas

Un `ping` portant `fw` suffit :

```
GET ?action=ping&device=246f28123456&fw=1.4.2
  → {"ok":true,"version":7,"time":1790071296,"states":[1]}
fw retenue : 1.4.2
```

Puis l'écran est enregistré **par le chemin de production** —
`jeedom::fromHumanReadable()`, `utils::a2o()`, `save()`, avec une clé `cmd` dans
le formulaire, exactement comme `core/ajax/eqLogic.ajax.php` : c'est ce chemin,
et lui seul, qui déclenche le piège `setCmd()`.

```
on ferme le verrou par le formulaire
  avant : fw=[1.3.0] ota_allowed=1 version=7 lastcontact=[2026-09-22 12:01:07]
  après : fw=[1.3.0] ota_allowed=0 version=7 lastcontact=[2026-09-22 12:01:07]
on le rouvre
  avant : fw=[1.3.0] ota_allowed=0 version=7 lastcontact=[2026-09-22 12:01:07]
  après : fw=[1.3.0] ota_allowed=1 version=7 lastcontact=[2026-09-22 12:01:07]
commande « Version du firmware » : créée
```

Trois choses d'un coup : `fw` et `lastcontact` **survivent** à l'enregistrement,
le verrou se pose et se retire, et **la version de mise en page ne bouge pas**.
C'est ce dernier point qui compte : autoriser un écran à se mettre à jour ne
change rien à ce qu'il affiche, et ne doit donc pas faire redessiner toute la
maison.

Le tableau du parc, rendu par le serveur :

```
Fictif GlowScreen  aa:bb:cc:dd:ee:ff  fw=—      ota=0 global=1 ouvert=non  22/09/2026 10:50:52 (il y a 1 h)
Test GlowScreen    24:6f:28:12:34:56  fw=1.3.0  ota=1 global=1 ouvert=oui  22/09/2026 11:59:39 (il y a 55 s)
```

### 5.10 Le déploiement n'efface pas le firmware déposé

Le contrôle qui justifiait la ligne de `.deployignore` :

```
avant   -rw-rw-r-- www-data 1002288  glowscreen32-1.4.0.bin
        0fbb3369b36ecb85fc24b5a76693693edff7e2090250fa53dee78285397ebf78

$ sudo /home/<utilisateur>/dev/tools/deploy-plugin.sh
  .d..t...... data/firmware/          ← le dossier, pas son contenu
  Déploiement terminé.

après   -rw-rw-r-- www-data 1002288  glowscreen32-1.4.0.bin
        0fbb3369b36ecb85fc24b5a76693693edff7e2090250fa53dee78285397ebf78

action=firmware  → update:true, même URL
téléchargement du .bin → HTTP 200
```

Même taille, **même empreinte**, et l'API continue de le proposer. Sans la
ligne `data/firmware/*.bin`, le `rsync --delete` l'aurait effacé — et rien
n'aurait prévenu : c'est la réponse `firmware_unavailable` d'une carte, une
semaine plus tard, qui l'aurait appris.

### 5.11 Le marqueur de version, sur de vrais binaires

Les essais des § 5.7 à 5.10 avaient été menés avec des images **fabriquées** :
en-tête et descripteur valides, contenu factice. Elles suffisaient à exercer le
chemin du plugin — et c'est précisément pourquoi elles n'ont pas montré que la
source de la version était mauvaise (§ 0.3). Cette série-ci est faite avec les
binaires réellement produits par PlatformIO.

```
.pio/build/cyd/firmware.bin      1 011 600 octets   premier octet 0xE9
.pio/build/calib/firmware.bin      327 072 octets   premier octet 0xE9
```

La lecture du marqueur, seule :

```
cyd    → [1.4.1]
calib  → []          ← pas de marqueur : c'est voulu, l'outil de calibration
                       ne doit jamais pouvoir partir en OTA
```

Le préfixe est bien cherché **en entier** :

```
« GLOWSCREEN32-FW: » : 1 occurrence
    0x00132A  GLOWSCREEN32-FW:1.4.1
« GLOWSCREEN32 »     : 2 occurrences
    0x00132A  GLOWSCREEN32-FW:1.4.1
    0x0018BA  GLOWSCREEN32-APIKEY      ← ce qu'une recherche laxiste attraperait
```

Le dépôt du binaire `calib`, refusé :

```
Ce binaire ne porte pas de marqueur de version GlowScreen32
(« GLOWSCREEN32-FW:<version> », terminé par un octet nul) : il n'a pas été
produit par ce projet, ou la version n'a pas été incrémentée. […]
```

Le dépôt du vrai firmware :

```json
{ "file": "glowscreen32-1.4.1.bin", "version": "1.4.1",
  "sha256": "512ad22d0ae9b1022119dbc104612c155660b1f1145a9f68f4d238e58f9b30f7",
  "size": 1011600, "human": "987,9 Kio",
  "url": "http://192.168.1.10/plugins/glowscreen32/data/firmware/glowscreen32-1.4.1.bin" }
```

`sha256sum` du fichier source : `512ad22d…`, **identique**, et 1 011 600 octets
des deux côtés.

Puis les quatre combinaisons de verrous, avec ce binaire-là — menées sur
**« Fictif GlowScreen » (`#454`), derrière lequel il n'y a aucune carte** : les
verrous de l'écran de test sont restés fermés tout du long, pour qu'aucun
scénario d'essai ne puisse déclencher une mise à jour sur la carte du mur.

```
ota_enabled=0 ota_allowed=0  HTTP 200  {"ok":true,"update":false}
ota_enabled=1 ota_allowed=0  HTTP 200  {"ok":true,"update":false}
ota_enabled=0 ota_allowed=1  HTTP 200  {"ok":true,"update":false}
ota_enabled=1 ota_allowed=1  HTTP 200  {"ok":true,"update":true,"version":"1.4.1", …}

téléchargement du .bin       HTTP 200  1 011 600 octets  application/octet-stream
sha256sum                    512ad22d0ae9b1022119dbc104612c155660b1f1145a9f68f4d238e58f9b30f7
carte déjà en 1.4.1          HTTP 200  {"ok":true,"update":false}
```

### 5.12 Le marqueur à cheval sur deux lectures, et les marqueurs malformés

Le fichier est parcouru par tranches d'un mégaoctet : un marqueur posé sur la
frontière serait coupé en deux sans le recouvrement. Le vrai binaire ne
l'exerce pas — son marqueur est à `0x132A` — mais un firmware plus gros
l'exercerait un jour, et la panne serait alors un dépôt refusé sans raison
apparente.

```
bien avant la frontière         offset 1 043 576  → [1.9.9]
juste avant (préfixe coupé)     offset 1 048 568  → [1.9.9]
pile sur la frontière           offset 1 048 576  → [1.9.9]
préfixe entier, valeur coupée   offset 1 048 558  → [1.9.9]
juste après                     offset 1 048 579  → [1.9.9]
```

Et ce qui doit être rejeté :

```
sans marqueur, mais avec « X-GLOWSCREEN32-APIKEY »   → []
marqueur sans octet nul dans la fenêtre              → []
version contenant « / » et « .. »                    → []
fausse occurrence vide AVANT la vraie                → [2.0.0]
```

Le dernier cas est celui qui justifie de poursuivre la recherche au lieu
d'abandonner à la première occurrence : un littéral de diagnostic citant le
préfixe ne doit pas faire refuser un binaire qui porte le vrai marqueur.

---

## 6. Ce qui reste à faire côté utilisateur

1. **Écrire le côté carte de l'OTA.** Le plugin est prêt et vérifié ; le
   firmware doit encore appeler `action=firmware` avec sa version, télécharger,
   **vérifier le SHA-256 avant de basculer**, écrire dans la partition inactive,
   et surtout **ne se déclarer sain qu'après coup** — Wi-Fi connecté, API ayant
   répondu, écran initialisé. Ne jamais appeler
   `esp_ota_mark_app_valid_cancel_rollback()` dans `setup()` : ce serait
   désactiver le filet de sécurité tout en croyant l'avoir.
   `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` doit être actif.
2. **Valider l'OTA sur la carte réelle.** Le plugin a été vérifié avec le vrai
   `firmware.bin` du projet, version `1.4.1` (§ 5.11) : la version est extraite,
   le fichier se range, l'empreinte correspond, le téléchargement fonctionne.
   Ce qui reste à voir, et que seule la carte peut dire, c'est l'écriture en
   partition inactive et le redémarrage dessus. **Le dépôt a été vidé et les
   deux verrous refermés à la fin** (§ 8) : rien ne peut partir vers un écran
   tant que ce n'est pas décidé.
3. **Valider sur la carte.** L'écran `#453` est laissé avec un bouton
   interrupteur fonctionnel sur la lampe extérieure de test. Le firmware doit être à jour du
   contrat v1.3 : `id` est un rang, et il y a un champ `mode` et un champ
   `pending` de plus. Un firmware v1.2 renverrait encore un identifiant de
   commande, et recevrait `unknown_button`.
4. **Abaisser le niveau de journal** du plugin à *info* le temps d'un
   déploiement (Analyse → Journaux). Par défaut Jeedom ne retient que les
   erreurs, et **toutes les décisions d'OTA sont en `info`** : le plugin sera
   muet là où l'on a justement besoin de le lire.
5. **Supprimer l'écran d'essai « Fictif GlowScreen »** (`#454`), qui ne
   correspond à aucune carte et s'affichera éternellement *hors ligne*.
6. **Reporter la clé API dans le firmware** — voir § 4.

---

## 7. Remarques sur le contrat

Le fichier `docs/api-contract.md` est passé en **v1.4** et sert de spécification
à cette version ; il n'a pas été modifié par ce travail, il a été suivi — y
compris sa section « Autorisation de mise à jour — double verrou », dont le
plugin est la mise en œuvre exacte.

Les deux premières remarques de la version précédente de ce document sont
**closes** :

1. ~~Aucune place pour les scénarios~~ — la convention de l'identifiant négatif
   disparaît : `id` étant un rang, un bouton-scénario n'a plus rien de
   particulier.
2. ~~`press` peut rendre un `state` en retard d'un instant~~ — le contrat le dit
   maintenant, et `pending` le signale explicitement.

Restent :

3. **Rien ne décrit ce que la carte doit faire d'un `bad_request`.** Les règles
   côté firmware couvrent la perte de Wi-Fi et d'API, et `unknown_device` a son
   protocole d'enrôlement. Un `bad_request` sur un appui, lui, est un défaut de
   programmation : il ne sert à rien de le réessayer trois fois avec backoff.
4. **Rien ne décrit non plus ce que la carte doit faire d'un `unknown_button`.**
   Ce n'est pas un défaut de programmation mais une mise en page périmée : la
   bonne réaction est de redemander `layout` immédiatement, sans attendre le
   prochain changement de `version`. Une ligne dans le contrat l'éviterait à
   chaque implémenteur de firmware.
5. **La clé circule en clair sur le réseau local**, en-tête ou query string : le
   point d'entrée est en HTTP. C'est assumé pour un réseau domestique, et
   l'en-tête évite au moins les journaux d'Apache — mais il faut rappeler que ce
   point d'entrée ne doit pas être exposé sur Internet.

Et trois nouvelles, toutes nées de la v1.4 :

6. **Le binaire est servi sans aucune authentification.** Le contrat impose une
   URL statique sous `data/firmware/`, donc un fichier qu'Apache sert à qui le
   demande : n'importe qui sur le réseau local peut télécharger le firmware.
   C'est sans gravité — il ne contient ni clé ni secret, et il finira de toute
   façon en clair dans la flash d'une carte — mais le contrat gagnerait à le
   dire, plutôt que de le laisser découvrir.
7. **Rien ne dit ce que la carte doit faire d'un `firmware_unavailable`.** Le
   code existe, sa cause est décrite, mais pas la réaction attendue. La bonne
   est manifestement de réessayer plus tard, sans rien écrire en flash — un
   firmware qui traiterait ce 404 comme une erreur réseau générique pourrait,
   lui, retenter en boucle.
8. **Le contrat ne dit pas d'où vient la version.** Il exige que la carte
   annonce `fw` et que le serveur réponde une `version`, sans préciser que les
   deux doivent avoir la **même origine**. C'est pourtant la seule chose qui
   rende la comparaison valable, et la v1.2 l'a appris de la mauvaise manière :
   une version tirée du descripteur ESP-IDF est identique dans tous les
   binaires, et l'OTA ne se déclenche jamais (§ 0.3). Une ligne du contrat
   disant que la version est un marqueur du projet, gravé par le firmware et lu
   par le plugin, éviterait à quiconque reprend ce code de refaire le même
   raisonnement — qui paraît juste jusqu'à ce qu'on regarde un vrai binaire.
9. **Le contrat ne prévoit pas le retour en arrière volontaire.** `update` est
   annoncé quand le dépôt est **plus récent** que la carte, et un binaire plus
   ancien n'est donc jamais proposé : redescendre de version demande un
   reflashage par USB. C'est défendable — le retour arrière automatique de
   l'ESP32 est le vrai filet — mais c'est un choix que le contrat laisse
   implicite, et qui mériterait d'y figurer.

---

## 8. Effets de bord — inventaire complet

Modifié **dans le plugin** (commits `eef7364`, `8a6a141` et `1e5b3ad` sur la
branche `beta`) :

- `core/class/glowscreen32.class.php` — toute la section OTA : `firmwareDir()`,
  `firmwareUrl()`, `firmware()`, `otaEnabled()`, `enableOta()`,
  `sanitizeVersion()`, `markerVersion()`, `humanSize()`, `publishFirmware()`,
  `removeFirmware()`, `otaState()`, `otaAllowed()`, `firmwareVersion()`,
  `noteFirmware()`, `otaDecision()` ; `preSave()` normalise `ota_allowed`,
  `createCommands()` ajoute « Version du firmware », `overview()` rend `fw`,
  `ota`, `otaGlobal` et `otaOpen` ;
- `core/php/api.php` — `action=firmware`, le paramètre `fw` retenu sur toute
  action, et `firmware_unavailable` ;
- `core/ajax/glowscreen32.ajax.php` — `otastate`, `otaglobal`, `firmwareupload`,
  `firmwareremove`, tous derrière la session d'administrateur ;
- `desktop/php/glowscreen32.php` — le cadre « Firmware », le verrou de l'écran
  dans l'onglet *Écran*, et deux colonnes de plus au tableau du parc ;
- `desktop/js/glowscreen32.js` — le dépôt par `FormData`/`fetch`, le rendu du
  cadre firmware, et la colonne « Verrou OTA » recalculée à chaque bascule du
  verrou global ;
- `plugin_info/configuration.php` — l'état des verrous et du firmware, en
  lecture seule, et les mêmes colonnes ;
- `.deployignore` — `data/firmware/*.bin`, et `.gitignore` la même ligne : le
  binaire ne doit ni sortir du dépôt vers l'installation, ni y entrer ;
- `data/firmware/.htaccess` — **nouveau fichier**, l'exception sur les `.bin` ;
- `plugin_info/info.json` — version 1.2, description à jour ;
- `tests/check-classes.php` — quatre contrôles de plus ;
- `README.md`, `docs/{fr_FR,en_US}/{index.md,changelog.md}`.

Modifié **ailleurs** :

- `/var/www/html/plugins/glowscreen32/` — la copie déployée, écrasée par
  `deploy-plugin.sh` ;
- **configuration du plugin** : `ota_enabled` créée puis **refermée** ; les cinq
  clés `firmware_*` créées puis **vidées** par le retrait du firmware d'essai ;
- **configuration des eqLogic `#453` et `#454`** : `ota_allowed` et `fw` créées,
  puis **remises à leur défaut** — verrou fermé, version vide. La version
  annoncée pendant les essais était fictive : aucune carte ne l'avait envoyée, et
  la laisser aurait affiché un mensonge dans le tableau du parc. L'eqLogic `#453`
  a reçu une commande d'information de plus, « Version du firmware » ;
- le **niveau de journal du plugin**, abaissé à *debug* le temps du contrôle puis
  **retiré** (`config::remove`, niveau effectif revérifié à 400) ;
- `docs/plugin-jeedom.md` (dépôt du firmware) — ce document.

Aucune écriture dans `/var/www/html/core/`, aucun autre plugin touché, aucune
configuration Apache, aucun `.htaccess` hors du plugin, aucune table créée,
aucun scénario créé ni supprimé.

**Aucun équipement réel n'a été actionné par ce travail** : la 1.2 ne touche pas
à `press`, et aucun essai de ce document n'en a joué un. Le relais
de la lampe extérieure de test (`#84`) est resté dans l'état où la 1.1 l'avait laissé.

**Aucune mise à jour n'a pu partir vers la carte du mur.** Les essais de verrous
menés après l'arrivée du vrai binaire (§ 5.11) l'ont été sur
« Fictif GlowScreen » (`#454`), derrière lequel il n'y a aucune carte ; le
verrou de l'écran de test est resté fermé tout du long.

**Le dossier de dépôt a été vidé.** Les images `1.4.0` et `1.5.0` des premiers
essais étaient fabriquées, avec un contenu factice : les laisser en place avec
des verrous ouverts, c'était garder une carte à un flash d'un firmware qui ne
démarre pas. Le vrai binaire `1.4.1` déposé ensuite a été retiré lui aussi — il
est authentique, mais son envoi sur la carte du mur n'est pas encore décidé, et
un dépôt plein est une décision qui n'a pas à se prendre par défaut. État final
vérifié :

```
firmware déposé : aucun
ota_enabled     : 0
  Fictif GlowScreen   ota=0  fw=[]
  Test GlowScreen     ota=0  fw=[]
data/firmware/  : .htaccess seulement
action=firmware → {"ok":true,"update":false}
action=layout   → 1 bouton, version 7, l'écran fonctionne
niveau de journal du plugin : 400 (défaut), clé retirée
```

Les scripts et les images d'essai déposés dans `/tmp` du serveur ont été
effacés.
