# Jeedom — document de référence

Relevé le 22 septembre 2026 sur le serveur `<utilisateur>@192.168.1.10`, en lecture seule.
Tous les extraits de code de ce document sont **réels** : ils sont copiés depuis
le serveur, et le chemin de chaque fichier est indiqué. Ce qui n'a pas pu être
vérifié est signalé explicitement en fin de section ou en section 10.

---

## 0. L'environnement en une page

| Élément | Valeur relevée | Comment c'est vérifié |
|---|---|---|
| Version Jeedom | **4.6.1** | `cat /var/www/html/core/config/version` |
| PHP | **8.4.24** (Debian, NTS) | `php -v` |
| SGBD | **MariaDB 11.8.6** | `mariadb --version` |
| Serveur web | **Apache 2** (`/usr/sbin/apache2 -k start`), workers en `www-data` | `ps aux \| grep apache2` |
| OS | **Debian GNU/Linux 13 (trixie)**, noyau 6.12 amd64 | `/etc/os-release`, `uname -a` |
| Nom d'hôte | `<hôte-jeedom>` | `uname -a` |
| Racine web Jeedom | `/var/www/html` | — |
| Plugins **installés** | `/var/www/html/plugins/<id>/` | — |
| Plugins **sources** | `/home/<utilisateur>/dev/jeedom-plugin-<id>/` | — |
| Logs | `/var/www/html/log/` | — |
| Propriétaire des fichiers | `www-data:www-data`, `drwxrwxr-x` / `-rwxrwxr-x` | `ls -la` |
| Utilisateur `<utilisateur>` | uid 1000, **pas** dans le groupe `www-data` ; est dans `sudo` | `id <utilisateur>` |

**Deux arborescences, une seule source de vérité.** `/home/<utilisateur>/dev/jeedom-plugin-<id>`
est le dépôt git de développement ; `/var/www/html/plugins/<id>` n'en est qu'une
copie non versionnée, écrasée à chaque déploiement. Toute modification faite
directement dans `/var/www/html/plugins/` sera perdue.

Plugins présents : une quinzaine de plugins tiers et maison (caméras, météo, MQTT,
volets, etc.), dont ceux cités en exemple plus bas.

Accès SSH utilisé :

```bash
export SSHPASS='<mot-de-passe-ssh>'
sshpass -e ssh -o StrictHostKeyChecking=no <utilisateur>@192.168.1.10 '<commande>'
```

---

## 1. Synthèse de `STRUCTURE-PLUGIN-JEEDOM.md`

Source : `/home/<utilisateur>/dev/STRUCTURE-PLUGIN-JEEDOM.md` (16 618 octets).
Une copie plus ancienne et **incomplète** traîne dans `/var/www/html/plugins/STRUCTURE-PLUGIN-JEEDOM.md` :
il lui manque toute la sous-section « Les deux noms interdits ». Se référer à
celle de `/home/<utilisateur>/dev`.

### 1.1 La règle qui décide de tout : un dépôt par plugin

La racine du dépôt git **doit être** la racine du plugin. Pas de sous-dossier,
pas de mono-dépôt. `update::doUpdate()` (`core/class/update.class.php`, ~ligne 324) fait :

```php
if (!file_exists($cibDir . '/plugin_info')) {
    $files = ls($cibDir, '*');
    if (count($files) == 1 && file_exists($cibDir . '/' . $files[0] . 'plugin_info')) {
        $cibDir = $cibDir . '/' . $files[0];
    }
}
```

Il ne descend que d'**un** niveau, et uniquement si l'archive contient
**exactement une** entrée portant un `plugin_info/`. Ce mécanisme existe pour
absorber le dossier d'enrobage `proprietaire-depot-sha/` que GitHub ajoute à ses
archives, rien d'autre. Un mono-dépôt échoue sur « Le nom du plugin est
différent de l'ID ou le plugin n'est pas correctement formé. »

Le fournisseur GitHub (`core/repo/github.repo.php`) n'expose que quatre
paramètres : `user`, `repository`, `token`, `version` (la branche). **Aucun champ
chemin/sous-répertoire n'existe.**

### 1.2 Les trois noms qui doivent être identiques

L'`id` de `info.json`, le nom du dossier dans `plugins/`, et le nom de la classe PHP.
L'autoload du cœur (`core/php/core.inc.php`) et `plugin::getPathById()` s'appuient dessus.

Contraintes sur l'`id` : unique sur le Market, commence par une lettre, pas
d'accents, pas d'espaces, **pas d'underscore** (explicitement interdit).

### 1.3 Branches : la version d'un plugin, c'est le SHA du dernier commit

`repo_github::checkUpdate()` :

```php
$branch = self::getBranchInfo($_update);            // GET /repos/{user}/{repo}/branches/{branche}
$_update->setRemoteVersion($branch['commit']['sha']);
if ($branch['commit']['sha'] != $_update->getLocalVersion()) { … mise à jour disponible … }
```

Ni tag, ni release, ni numéro de version. **Tout commit poussé sur la branche
stable est proposé immédiatement comme mise à jour à tous les utilisateurs.**
D'où la convention : `beta` = branche par défaut où l'on développe, `master` =
canal stable. (100 des 129 plugins de `github.com/jeedom` ont `beta` par défaut.)
`pluginVersion` dans `info.json` n'est lu **nulle part** dans le cœur : purement indicatif.

Le changelog par défaut construit par `repo_github::objectInfo()` pointe vers
`https://github.com/{user}/{repo}/commits/{branche}` : **les messages de commit
sont lus par les utilisateurs.**

Convention de nommage retenue ici pour les dépôts : `jeedom-plugin-<id>`.

### 1.4 Section 8 — les pièges vérifiés en production

C'est la partie la plus chère du document. Deux d'entre eux ont mis un plugin à
terre et **ne se voient ni à la relecture, ni au `php -l`, ni en test hors ligne** :
le cœur travaille par réflexion sur les classes du plugin.

#### Piège A — une propriété sans souligné devient une colonne de table

`DB::save()` parcourt les propriétés de l'objet et les traite comme des colonnes,
**sauf** celles dont le nom commence par `_` (`core/class/DB.class.php`, ~ligne 556 :
`if ('_' !== $name[0])`).

```php
private $refreshError = '';   // INTERDIT : « Unknown column 'refreshError' »
private $_refreshError = '';  // correct
```

Symptôme : la création d'un équipement échoue sur une erreur SQL 1054, le bouton
« Ajouter » ne produit rien. **Toute propriété d'état d'une classe eqLogic ou cmd
doit porter le souligné.**

#### Piège B — une méthode `set` + clé de formulaire est appelée par le cœur

À l'enregistrement, le cœur passe le formulaire à `utils::a2o()`
(`core/ajax/eqLogic.ajax.php`, ~ligne 571), qui pour chaque clé reçue construit
`'set' . ucfirst($clé)` et l'appelle sur l'objet du plugin
(`core/class/utils.class.php`, ~ligne 117).

```php
private function setCmd($_logicalId, $_value) { … }      // INTERDIT
private function publishCmd($_logicalId, $_value) { … }  // correct
```

La page envoie **toujours** une clé `cmd` (les lignes du tableau des commandes) :
le cœur appelle donc `setCmd()` sur la classe du plugin. Si elle est privée,
l'enregistrement meurt sur `Call to private method … from scope utils`, **avant
d'écrire quoi que ce soit**. L'utilisateur voit la page se rafraîchir et sa
saisie disparaître ; **rien dans le journal du plugin, tout dans
`/var/www/html/log/http.error`**.

Le piège ne se déclenche que si le tableau `cmd` envoyé n'est pas vide — donc dès
qu'un équipement a des commandes. Un test qui envoie un tableau vide ne reproduit rien.

Noms à ne jamais employer :
`setId`, `setName`, `setLogicalId`, `setGeneric_type`, `setObject_id`,
`setEqType_name`, `setIsVisible`, `setIsEnable`, `setConfiguration`, `setTimeout`,
`setCategory`, `setDisplay`, `setOrder`, `setComment`, `setTags`, **`setCmd`**.

#### Les autres pièges

- **`preSave()` ne doit jamais lever d'exception à la création d'un équipement.**
  Le cœur crée l'eqLogic avec son seul nom ; une validation stricte rend le
  bouton « Ajouter » définitivement inopérant.
- **Les pages sont chargées en AJAX** : `DOMContentLoaded` a déjà eu lieu quand le
  JS du plugin s'exécute. Attacher les écouteurs à la racine du script.
- **Une ligne du tableau des commandes doit contenir `.cmdAttr[data-l1key="id"]`**,
  sinon chaque sauvegarde détruit et recrée les commandes : historique perdu,
  scénarios cassés.
- **`insertAdjacentHTML` sur un `<table>`** crée un `<tbody>` par insertion.
  Construire les lignes avec `createElement('tr')` puis `appendChild`.
- **La classe `<id>Cmd` est obligatoire, même vide** : sans elle la sauvegarde
  d'un équipement échoue.
- **Contraintes d'unicité SQL** : `(eqLogic_id, name)` sur la table `cmd`,
  `(name, object_id)` sur `eqLogic`. Renommer une commande peut faire échouer
  l'enregistrement de tout l'équipement.
- **`catch (Exception)` ne rattrape pas les `Error` de PHP 8** : utiliser
  `Throwable`, sinon une méthode inexistante produit un HTTP 500 muet.
- **`core/config/<id>.config.ini`** : la section doit porter l'`id` du plugin, pas
  `[default]`, sinon le fichier est **totalement inerte**.
- **i18n** : les fichiers vont dans `core/i18n/<lang>.json`, clé de premier niveau
  = chemin du fichier depuis la racine de Jeedom. Aucun `fr_FR.json` (langue source).
- **Un démon doit éviter de charger `core.inc.php`** : cela l'expose aux mises à
  jour du cœur et à un cache de configuration jamais invalidé. Préférer un
  processus autonome qui reçoit sa configuration et repousse ses données par HTTP.

### 1.5 `.gitignore` de référence (repris des plugins officiels)

```
core/config/common.config.php
data/
nbproject/
node_modules/
test.php
sftp-config.json
.project
.remote-sync.json
.vscode/
.DS_Store
Thumbs.db
*.php~
```

`core/config/common.config.php` et `data/` contiennent des données propres à
l'installation de l'utilisateur : jamais dans le dépôt.

### 1.6 Intégration continue

`.github/workflows/work.yml`, tel qu'il est réellement dans `pluginexemple` :

```yaml
# Contrôles de conformité du plugin (syntaxe PHP, structure, info.json).
# Workflow réutilisable maintenu par Jeedom, utilisé par les plugins officiels.
name: 'Full Workflows Plugin Jeedom'

on:
  push:
    branches: [beta]
  pull_request:
    branches: [beta, master]

jobs:
  plugin:
    uses: jeedom/workflows/.github/workflows/plugin.yml@main
```

---

## 2. Le plugin le plus simple comme modèle : `pluginexemple`

C'est le plus petit des 14 dépôts (`720 Ko`, 24 fichiers hors `.git`), et le seul
sans `core/config/`, sans `data/`, sans `core/template/`.

### 2.1 Arborescence réelle

```
/home/<utilisateur>/dev/jeedom-plugin-pluginexemple/
├── .github/workflows/work.yml
├── .gitignore
├── LICENSE
├── README.md
├── core/
│   ├── ajax/pluginexemple.ajax.php
│   ├── class/
│   │   ├── .htaccess
│   │   └── pluginexemple.class.php
│   └── i18n/
│       ├── .htaccess
│       └── en_US.json
├── desktop/
│   ├── css/pluginexemple.css
│   ├── js/pluginexemple.js
│   └── php/pluginexemple.php
├── docs/
│   ├── en_US/{changelog.md,index.md}
│   └── fr_FR/{changelog.md,index.md}
├── plugin_info/
│   ├── .htaccess
│   ├── configuration.php
│   ├── info.json
│   ├── install.php
│   └── pluginexemple_icon.png
└── tools/                        ← non déployé (voir §9)
    ├── dump-model.php
    ├── functest.py
    └── make-icon.php
```

Arborescence **maximale** observée (union de tous les plugins) — ce qui existe
réellement sur ce serveur, rien d'inventé :

```
<id>/
├── plugin_info/  info.json, install.php, configuration.php, <id>_icon.png, .htaccess
│                 [packages.json si dépendances apt/pip]
├── core/
│   ├── class/<id>.class.php  + <id>Xxx.class.php pour les classes annexes
│   ├── ajax/<id>.ajax.php    endpoint AJAX de l'interface (session)
│   ├── php/                  endpoints HTTP appelés par un démon    ← plugincam, plugincam2, pluginaspi, pluginmqtt
│   ├── config/<id>.config.ini valeurs par défaut, section [<id>]
│   ├── i18n/<lang>.json      traductions (jamais fr_FR.json)
│   └── template/{dashboard,mobile}/cmd.<type>.<subtype>.<id>.html   widgets
├── desktop/
│   ├── php/<id>.php          page principale
│   ├── js/<id>.js
│   ├── css/<id>.css
│   └── modal/                fenêtres modales           ← plugincam, pluginlampe, pluginmqtt,
│                                                          pluginpresence, pluginsimu…, pluginvolet
├── resources/                démon, venv                ← plugincam, plugincam2, pluginmqtt
├── data/                     données d'exécution (+ .htaccess Deny)
├── docs/<lang>/{index.md,changelog.md}
├── tests/                    banc d'essai hors ligne (non déployé)
├── tools/                    outils de dev (non déployés)
├── LICENSE, README.md, .gitignore, .deployignore
└── .github/workflows/work.yml
```

**`core/api/` n'est utilisé par aucun plugin ici.** Un dossier
`/var/www/html/plugins/plugincam/core/api/` existe mais il est **vide** (vestige).
La convention réellement employée est `core/php/` — voir §4.

**Pas de dossier `mobile/`** dans aucun des 14 plugins : le rendu mobile passe par
`core/template/mobile/` et par la CSS responsive de la page desktop.

**Pas de `3rdparty/`** dans aucun des 14 plugins.

### 2.2 `plugin_info/info.json` — exemple réel intégral

`/home/<utilisateur>/dev/jeedom-plugin-pluginexemple/plugin_info/info.json` :

```json
{
    "id": "pluginexemple",
    "name": "MonPlugin",
    "pluginVersion": "0.2",
    "description": {
        "fr_FR": "Dashboard moderne pour Jeedom, pensé pour l'écran mural comme pour le téléphone. Il n'altère rien : le dashboard d'origine reste en place, MonPlugin en construit un second à côté, en relisant vos objets, équipements et commandes. […]",
        "en_US": "A modern dashboard for Jeedom, designed for wall tablets as much as for phones. […]"
    },
    "licence": "AGPL",
    "author": "<auteur>",
    "require": "4.4",
    "requireOsVersion": "11",
    "category": "other",
    "index": "pluginexemple",
    "display": "pluginexemple",
    "hasDependency": 0,
    "hasOwnDeamon": 0,
    "maxDependancyInstallTime": 5,
    "eventjs": 0,
    "issue": "https://github.com/<compte-github>/jeedom-plugin-pluginexemple/issues",
    "documentation": "https://github.com/<compte-github>/jeedom-plugin-pluginexemple/blob/master/docs/#language#/index.md",
    "documentation_beta": "https://github.com/<compte-github>/jeedom-plugin-pluginexemple/blob/beta/docs/#language#/index.md",
    "changelog": "https://github.com/<compte-github>/jeedom-plugin-pluginexemple/blob/master/docs/#language#/changelog.md",
    "changelog_beta": "https://github.com/<compte-github>/jeedom-plugin-pluginexemple/blob/beta/docs/#language#/changelog.md",
    "language": ["fr_FR", "en_US"],
    "compatibility": ["smart", "rpi", "docker", "diy"]
}
```

> Les descriptions sont tronquées ici par `[…]` pour la lisibilité ; dans le
> fichier réel elles font chacune plus de 600 caractères. Le Market exige **au
> moins 80 caractères par langue** et un objet multilingue, pas une chaîne.

#### Clés réellement lues par le cœur

Relevées dans `plugin::byId()` (`core/class/plugin.class.php`, lignes 74-136) :

| Clé | Rôle |
|---|---|
| `id` | identifiant unique, = dossier = classe PHP |
| `name` | nom affiché |
| `description` | texte ou objet multilingue |
| `licence` (ou `license`) | licence |
| `author` | auteur |
| `require` | version minimale du cœur — **bloque l'activation** si supérieure (vérifié dans `setIsEnable()`) |
| `requireOsVersion` | version Debian minimale — **bloque aussi l'activation** |
| `category` | clé de la liste de `core/config/jeedom.config.php` |
| `index` | page cible du menu, par défaut l'`id` |
| `hasDependency` | le plugin a des dépendances à installer |
| `hasOwnDeamon` | le plugin gère son propre démon *(la faute de frappe est dans le cœur : la respecter)* |
| `maxDependancyInstallTime` | minutes avant abandon de l'installation des dépendances |
| `eventjs` | charge `desktop/js/event.js` sur toutes les pages |
| `issue`, `documentation`, `changelog` | URLs ; `#language#` y est remplacé par la langue |
| `specialAttributes` | champs supplémentaires sur eqLogic / objet / utilisateur |

Le Market exige en plus `changelog_beta` et `documentation_beta`.
`pluginVersion` n'est lu nulle part dans le cœur.

Catégories valides (`core/config/jeedom.config.php`) : `security`,
`automation protocol`, `home automation protocol`, `programming`, `organization`,
`weather`, `communication`, `devicecommunication`, `multimedia`, `wellness`,
`monitoring`, `health`, `nature`, `automatisation`, `energy`, `other`.

### 2.3 `plugin_info/install.php` — réel et complet

```php
<?php
/* This file is part of Jeedom.
 * … en-tête GPL …
 */

require_once __DIR__ . '/../../../core/php/core.inc.php';

/*
 * MonPlugin ne crée rien et ne migre rien : il lit. L'installation se limite donc
 * à poser les réglages par défaut, une fois, pour que la page de configuration
 * ne s'ouvre pas sur des champs vides dont personne ne sait ce qu'ils valent.
 */
function pluginexemple_install() {
    /* config::byKey() ne renvoie jamais null sur une clé absente : elle place le
     * défaut en cache puis retourne '' parce que isset() est faux sur null. Le
     * test doit donc porter sur la chaîne vide, sans quoi la valeur par défaut
     * n'est jamais écrite et la fonction ne sert à rien. */
    if (config::byKey('showUnassigned', 'pluginexemple', '') === '') {
        config::save('showUnassigned', 1, 'pluginexemple');
    }
}

function pluginexemple_update() {
    pluginexemple_install();
}

/*
 * Rien à retirer : aucun équipement, aucune commande, aucun fichier produit à
 * l'exécution. Les clés de configuration du plugin sont supprimées par le coeur
 * avec le plugin lui-même.
 */
function pluginexemple_remove() {
}
```

Le cœur appelle ces fonctions par `plugin::callInstallFunction()`, qui cherche
`plugins/<id>/plugin_info/install.php` puis `function <id>_<nom>()`. Les noms
reconnus : `install`, `update`, `remove`, et les variantes `pre_*` dans un
`plugin_info/pre_install.php` séparé.

### 2.4 `plugin_info/.htaccess` — réel

```apache
Order allow,deny
<Files ~ "\.(jpg|jpeg|png|gif|pdf|txt|bmp)$">
   allow from all
</Files>
Deny from all
```

Le même fichier existe en `core/class/.htaccess`, `core/config/.htaccess`,
`core/i18n/.htaccess`, `data/.htaccess` (généralement `Deny from all` seul).

> **Ne jamais mettre de `.htaccess` dans `core/php/`** : un démon ou un appareil
> externe appelle ces URL depuis l'extérieur d'Apache (commentaire explicite dans
> `pluginmqtt/core/php/callback.php`).

### 2.5 Squelette de la classe eqLogic — code réel

Le modèle le plus complet est `pluginhoraires` (un plugin à équipements, cycle de vie
complet). Extraits de `/home/<utilisateur>/dev/jeedom-plugin-pluginhoraires/core/class/pluginhoraires.class.php`.

```php
<?php
/* This file is part of Jeedom.
 * … en-tête GPL …
 */

require_once __DIR__ . '/../../../../core/php/core.inc.php';

class pluginhoraires extends eqLogic {

    /* … constantes … */

    /* Widgets fournis par le plugin, déclarés au coeur. */
    public static function templateWidget() {
        $icons = array(
            '#_icon_on_#'  => "<i class='icon_red fas fa-exclamation-triangle'></i>",
            '#_icon_off_#' => "<i class='icon_green fas fa-check'></i>",
        );
        return array(
            'info' => array(
                'binary' => array(
                    'trouble'     => array('template' => 'tmplicon',     'replace' => $icons),
                    'troubleLine' => array('template' => 'tmpliconline', 'replace' => $icons),
                ),
            ),
        );
    }

    /* ==================================================================== CRON */

    /*
     * Appelé chaque minute par plugin::cron(). Chaque trajet décide lui-même
     * s'il doit interroger iRail […]
     */
    public static function cron() {
        $deadline = microtime(true) + self::CRON_BUDGET;

        foreach (self::byType(__CLASS__, true) as $eqLogic) {
            try {
                if (!$eqLogic->shouldPoll() || microtime(true) > $deadline) {
                    $eqLogic->refreshFromCache();
                    continue;
                }
                $eqLogic->update();
            } catch (Throwable $e) {
                // Un trajet en échec ne doit pas priver les autres de leur tour.
                log::add(__CLASS__, 'error', $eqLogic->getHumanName() . ' : ' . $e->getMessage());
            }
        }
    }
```

Noms de crons reconnus par le cœur, tous **statiques** sur la classe du plugin :
`cron()` (minute), `cron5()`, `cron10()`, `cron15()`, `cron30()`, `cronHourly()`,
`cronDaily()`. Observés en vrai : `pluginhoraires::cron()`, `pluginhygiene::cronHourly()`,
`pluginhygiene::cron5()`.

#### Cycle de vie

```php
    public function preSave() {
        /*
         * Aucune exception ici : le coeur crée l'équipement avec son seul nom.
         * Toute validation rendrait le bouton « Ajouter » définitivement
         * inopérant. […]
         */

        /*
         * Un trajet neuf naît actif et visible. « Ajouter » n'envoie que le nom
         * (plugin.template.js, addEqLogic) : les cases « Activer » et « Visible »
         * du formulaire, pourtant cochées dans le HTML, ne sont jamais lues à ce
         * moment-là, et le trajet se retrouvait désactivé — absent du dashboard,
         * ignoré par le cron, sans que rien ne le signale.
         */
        if ($this->getId() == '') {
            $this->setIsEnable(1);
            $this->setIsVisible(1);
        }
        foreach (array(
            'slot_start'    => self::DEFAULT_SLOT_START,
            'slot_end'      => self::DEFAULT_SLOT_END,
            'max_trains'    => self::DEFAULT_MAX_TRAINS,
            'threshold'     => self::DEFAULT_THRESHOLD,
            'watch_before'  => self::DEFAULT_WATCH_BEFORE,
            'watch_enabled' => 1,
        ) as $key => $default) {
            if ($this->getConfiguration($key, '') === '') {
                $this->setConfiguration($key, $default);
            }
        }
    }

    public function postSave() {
        $this->createCommands();

        if (!$this->isConfigured()) {
            return;
        }
        try {
            $journeys = $this->getJourneys();
            $changed = !isset($journeys['signature']) || $journeys['signature'] !== $this->signature();
            if ($changed) {
                $this->clearAlertState();
            }
            $this->update($changed);
        } catch (Throwable $e) {
            // L'enregistrement ne doit pas échouer parce qu'iRail est
            // indisponible : le trajet est valide, le cron réessaiera.
            log::add(__CLASS__, 'error', $this->getHumanName() . ' : ' . $e->getMessage());
        }
    }

    public function preRemove() {
        /*
         * DB::remove() met l'id à null avant postRemove : les caches doivent
         * être nettoyés tant que l'identifiant est encore lisible.
         */
        $this->clearJourneys();
        $this->clearAlertState();
        $this->clearProblem();
        return true;
    }
```

Les hooks disponibles sur `eqLogic` sont `preInsert`, `postInsert`, `preUpdate`,
`postUpdate`, `preSave`, `postSave`, `preRemove`, `postRemove`.
**Aucun des 14 plugins de ce serveur n'utilise `preInsert`/`postInsert`/`preUpdate`/`postUpdate`** :
le motif retenu partout est `preSave` (défauts + `isEnable`/`isVisible` à la
création) + `postSave` (création des commandes, premier rafraîchissement) +
`preRemove` (nettoyage des caches et messages).

#### Création des commandes — motif idempotent réel

```php
    private function addCmdIfMissing($_logicalId, $_name, $_type, $_subType, $_options = array()) {
        $cmd = $this->getCmd(null, $_logicalId);
        if (is_object($cmd)) {
            return $cmd;
        }
        $cmd = new pluginhorairesCmd();
        $cmd->setEqLogic_id($this->getId());
        $cmd->setLogicalId($_logicalId);
        /*
         * La table cmd impose l'unicité du couple (eqLogic_id, name) : un nom
         * déjà pris ferait échouer l'enregistrement de tout l'équipement. On
         * suffixe plutôt que de laisser planter.
         */
        $name = __($_name, __FILE__);
        if (is_object(cmd::byEqLogicIdCmdName($this->getId(), $name))) {
            $name .= ' (' . $_logicalId . ')';
        }
        $cmd->setName($name);
        $cmd->setType($_type);          // 'info' ou 'action'
        $cmd->setSubType($_subType);    // info: string|numeric|binary|other
                                        // action: other|slider|color|message|select
        $cmd->setIsVisible(isset($_options['isVisible']) ? $_options['isVisible'] : 0);
        $cmd->setIsHistorized(isset($_options['isHistorized']) ? $_options['isHistorized'] : 0);
        if (isset($_options['order']))   { $cmd->setOrder($_options['order']); }
        if (isset($_options['unite']))   { $cmd->setUnite($_options['unite']); }
        if (isset($_options['generic'])) { $cmd->setGeneric_type($_options['generic']); }
        if (isset($_options['icon']))    { $cmd->setDisplay('icon', '<i class="' . $_options['icon'] . '"></i>'); }
        if (isset($_options['template'])) {
            $cmd->setTemplate('dashboard', $_options['template']);
            $cmd->setTemplate('mobile', $_options['template']);
        }
        $cmd->save();
        return $cmd;
    }

    private function createCommands() {
        $order = 0;
        $this->addCmdIfMissing('summary', 'Prochain train', 'info', 'string', array(
            'order' => $order++, 'isVisible' => 1,
            'template' => 'pluginhoraires::pluginhoraires',
        ));
        /* … 22 autres commandes … */
    }
```

Et pour publier une valeur sur une commande info :

```php
    private function publishCmd($_logicalId, $_value) {
        if ($_value === '' || $_value === null) {
            $cmd = $this->getCmd(null, $_logicalId);
            if (is_object($cmd) && $cmd->execCmd() === '') {
                return;
            }
        }
        $this->checkAndUpdateCmd($_logicalId, $_value);
    }
```

> `checkAndUpdateCmd()` est la bonne porte : elle n'écrit et ne publie
> l'événement que si la valeur change. **Attention** (relevé dans
> `plugincam/core/php/jeePluginCam.php`) : elle renvoie `true` à valeur inchangée dès
> qu'on lui passe une date plus récente (`eqLogic.class.php:693`). Ne jamais
> déduire un front de son retour ; comparer `$cmd->execCmd()` avant écriture.

### 2.6 La classe `<id>Cmd extends cmd` et sa méthode `execute()`

Version avec actions, `pluginhoraires` (fin de fichier, lignes 1839-1863) :

```php
class pluginhorairesCmd extends cmd {

    public function execute($_options = array()) {
        $eqLogic = $this->getEqLogic();

        switch ($this->getLogicalId()) {
            case 'refresh':
                $eqLogic->update(true);
                /*
                 * update() ne lève pas quand des horaires sont déjà en cache :
                 * sans ce relais, un scénario appelant cette commande croirait
                 * ses trains relus alors qu'iRail est en panne.
                 */
                if ($eqLogic->getRefreshError() != '') {
                    throw new Exception($eqLogic->getRefreshError());
                }
                return true;

            case 'acknowledge':
                $eqLogic->acknowledge();
                return true;
        }
        return true;
    }
}
```

Version minimale, `pluginexemple` (fin de fichier) — **obligatoire même vide** :

```php
/*
 * Obligatoire même vide : le coeur instancie <plugin>Cmd par réflexion pour
 * toute commande du plugin. MonPlugin n'en crée aucune, mais son absence ferait
 * échouer le chargement de la classe au premier passage du coeur.
 */
class pluginexempleCmd extends cmd {
}
```

`execute()` n'est appelé que pour les commandes de type `action` : le cœur
court-circuite les `info` en tête de `cmd::execCmd()` (`core/class/cmd.class.php:1622`) :

```php
    public function execCmd($_options = null, $_sendNodeJsEvent = false, $_quote = false) {
        if ($this->getType() == 'info') {
            $state = $this->getCache(array('collectDate', 'valueDate', 'value', 'usage'));
            /* … */
            return $state['value'];
        }
        $eqLogic = $this->getEqLogic();
        if (!is_object($eqLogic) || $eqLogic->getIsEnable() != 1) {
            throw new Exception(… disableEqNoExecCmd … . $this->getHumanName());
        }
        /* … normalisation des options, timeline, alreadyInState, log 'event' … */
        $this->preExecCmd($options);
        $value = $this->formatValue($this->execute($options), $_quote);
        /* … */
    }
```

**Une commande `info` ne lit jamais la base** : sa valeur vit dans le cache.
C'est `$eqLogic->checkAndUpdateCmd()` / `$cmd->event($value)` qui l'y écrit.

### 2.7 Classe eqLogic « sans équipement » — le cas `pluginexemple`

Utile si le plugin est une page/dashboard et ne crée rien. En-tête réel :

```php
require_once __DIR__ . '/../../../../core/php/core.inc.php';

/*
 * MonPlugin ne crée aucun équipement : il relit ceux des autres plugins et en
 * dessine un second dashboard. La classe existe quand même, et hérite d'eqLogic,
 * parce que le coeur la charge par réflexion dès que le plugin est actif — un
 * plugin sans classe homonyme tombe en erreur sans message exploitable.
 *
 * Elle ne porte donc que des méthodes statiques : la construction du modèle
 * envoyé à la page. Aucune propriété n'y est déclarée, ce qui écarte d'office le
 * piège des colonnes inconnues de DB::save().
 */
class pluginexemple extends eqLogic {
```

### 2.8 `desktop/php/<id>.php` — réel

`/home/<utilisateur>/dev/jeedom-plugin-pluginexemple/desktop/php/pluginexemple.php`, début :

```php
<?php
/*
 * Le dashboard MonPlugin.
 *
 * isConnect() sans argument : cette page n'est pas une page d'administration,
 * c'est celle que l'on regarde tous les jours. Le compte tablette au profil
 * restreint doit pouvoir l'ouvrir ; ce qu'il a le droit de voir est décidé
 * équipement par équipement dans pluginexemple::model().
 *
 * Le modèle est rendu ici, en PHP, plutôt que demandé en ajax au chargement :
 * la page s'affiche remplie du premier coup, sans écran vide ni aller-retour,
 * et les droits sont appliqués avant que quoi que ce soit ne parte au
 * navigateur.
 */
if (!isConnect()) {
	throw new Exception('{{401 - Accès non autorisé}}');
}

$pluginexempleUser = isset($_SESSION['user']) ? $_SESSION['user'] : null;
sendVarToJS('pluginexempleModel', pluginexemple::model($pluginexempleUser));
?>

<div id="jg-root" class="jg-root" data-kiosk="0" data-tone="light" data-view="home" data-panel="0">
	<!-- … HTML de la page … -->
</div>
```

Points structurants :

- **Pas de `require core.inc.php`** : la page est incluse par `index.php`, le cœur
  est déjà chargé. (C'est précisément ce que montre l'erreur `Call to undefined
  function isConnect()` dans `http.error` quand Apache sert le fichier en direct.)
- `{{…}}` est la syntaxe de traduction côté page.
- `sendVarToJS('nom', $valeur)` passe des données PHP au JS ; il écrit sa balise
  `<script>` **sur-le-champ** (`core/php/utils.inc.php`, ligne 167), donc la
  variable est disponible pour un script inline qui suit.
- URL de la page : `index.php?v=d&p=<id>` (valeur de `index` dans `info.json`).
- Attention (commentaire réel) : **ne pas utiliser `<header>`** dans une page de
  plugin — la règle du cœur en plein écran est `body.fullscreen header { display: none }`.

### 2.9 `desktop/js/<id>.js` — réel, en-tête

```js
/* MonPlugin — dashboard alternatif pour Jeedom.
 * […]
 * 3. Le temps réel est déjà là. jeedom.changes() tourne pour toute page servie
 *    par index.php ; il suffit d'écouter cmd::update sur document.body. Aucun
 *    transport à écrire, aucun démon.
 */
;(function () {
  'use strict'

  var ROOT = document.getElementById('jg-root')
  if (ROOT === null) {
    return
  }

  var MODEL = (typeof pluginexempleModel !== 'undefined' && pluginexempleModel) ? pluginexempleModel : { rooms: [], devices: {} }

  /* Index plats […]
   * Ils vivent sur window et non dans cette fermeture, pour une raison précise :
   * Jeedom charge ses pages en ajax et RÉ-EXÉCUTE ce fichier à chaque retour sur
   * le dashboard. Les classes de cartes, elles, ne peuvent être enregistrées
   * qu'une fois dans le registre des éléments personnalisés — celles du second
   * passage seraient refusées. […] */
  var JG = window.pluginexempleRuntime || (window.pluginexempleRuntime = {})
```

Deux enseignements réutilisables :

1. **Le fichier JS est ré-exécuté à chaque navigation AJAX.** Tout état global
   (custom elements, écouteurs, timers) doit être idempotent.
2. **Le temps réel est gratuit** : `jeedom.changes()` tourne déjà ; écouter
   l'événement `cmd::update` sur `document.body` suffit pour recevoir les
   changements de valeur des commandes, sans démon ni websocket à écrire.

### 2.10 `core/ajax/<id>.ajax.php` — motif réel

```php
<?php
/* … en-tête GPL … */

try {
    require_once __DIR__ . '/../../../../core/php/core.inc.php';
    include_file('core', 'authentification', 'php');

    /*
     * isConnect() sans argument, et non isConnect('admin') : le dashboard est
     * fait pour être consulté par les habitants de la maison […]
     */
    if (!isConnect()) {
        throw new Exception(__('401 - Accès non autorisé', __FILE__));
    }

    ajax::init();

    if (init('action') == 'model') {
        $user = isset($_SESSION['user']) ? $_SESSION['user'] : null;
        ajax::success(pluginexemple::model($user, init('reveal') == 1));
    }

    if (init('action') == 'override') {
        if (!isConnect('admin')) {
            throw new Exception(__('401 - Accès non autorisé', __FILE__));
        }
        ajax::success(pluginexemple::applyOverride(init('scope'), init('key'), init('state')));
    }
    /* … */
} catch (Exception $e) {
    ajax::error(displayException($e), $e->getCode());
}
```

Les quatre lignes obligatoires : `require_once core.inc.php`,
`include_file('core','authentification','php')`, `isConnect(...)`, `ajax::init()`.
`ajax::init()` vérifie le jeton anti-CSRF ; **c'est ce qui distingue un endpoint
AJAX (session + CSRF) d'un endpoint machine (clé API) de la §4.**

---

## 3. L'API HTTP de Jeedom

### 3.1 Où ça vit

```
/var/www/html/core/api/
├── jeeApi.php      54 870 o   ← l'API générale (GET simple + JSON-RPC 2.0)
├── marketApi.php    2 077 o
├── proApi.php      26 281 o
└── tts.php          6 980 o
```

URL : `http://<jeedom>/core/api/jeeApi.php`.

### 3.2 Les clés d'API — `jeedom::getApiKey()`

`core/class/jeedom.class.php`, ligne 535 :

```php
	public static function getApiKey(string $_plugin = 'core', string $_mode = 'enable') {
		if ($_plugin == 'core') {
			if (config::byKey('api') == '') {
				config::save('api', config::genKey());
				config::save('api::api::mode', $_mode, 'core');
			}
			return config::byKey('api');
		}
		/* … idem pour 'apipro', 'apitts', 'apimarket' … */
		if (config::byKey('api', $_plugin) == '') {
			try {
				plugin::byId($_plugin);
			} catch (\Throwable $th) {
				return '';
			}
			config::save('api', config::genKey(), $_plugin);
			config::save('api::' . $_plugin . '::mode', $_mode, 'core');
		}
		if (config::byKey('api::' . $_plugin . '::mode', 'core', 'enable') == 'disable' && $_mode != 'disable') {
			config::save('api::' . $_plugin . '::mode', $_mode, 'core');
		}
		/* … retourne config::byKey('api', $_plugin) … */
	}
```

À retenir :

- **Chaque plugin a sa propre clé API**, distincte de la clé du cœur.
  `jeedom::getApiKey('monplugin')` la **crée à la volée** au premier appel
  (`config::genKey()`), et la range dans `config` sous
  (`key='api'`, `plugin='monplugin'`).
- Elle n'est créée que si `plugin::byId($_plugin)` réussit, donc que si le
  plugin existe réellement dans `plugins/`.
- Le mode d'accès est stocké à part, côté `core` :
  `config::byKey('api::<plugin>::mode', 'core', 'enable')`, valeurs `enable`,
  `disable`, `localhost`, `whiteip`.
- La clé du cœur est `config::byKey('api')` (sans plugin).

### 3.3 La validation — `jeedom::apiAccess()`

`core/class/jeedom.class.php`, ligne 610 :

```php
	public static function apiAccess(string $_apikey = '', string $_plugin = 'core') {
		if (trim($_apikey) == '' || strlen($_apikey) < 16) {
			return false;
		}
		$user = user::byHash($_apikey);
		if (is_object($user)) {
			if ($user->getEnable() == 0 || !self::apiModeResult($user->getOptions('api::mode', 'enable'))) {
				return false;
			}
			if ($user->getOptions('localOnly', 0) == 1 && !self::apiModeResult('whiteip')) {
				return false;
			}
			global $_USER_GLOBAL;
			$_USER_GLOBAL = $user;
			log::add('connection', 'info', __('Connexion par API de l\'utilisateur :', __FILE__) . ' ' . $user->getLogin());
			return true;
		}
		if (!self::apiModeResult(config::byKey('api::' . $_plugin . '::mode', 'core', 'enable'))) {
			return false;
		}
		$apikey = self::getApiKey($_plugin);
		if (trim($apikey) != '' && $apikey === $_apikey) {
			/** @var bool $_RESTRICTED */
			global $_RESTRICTED;
			$_RESTRICTED = config::byKey('api::' . $_plugin . '::restricted', 'core', false);
			return true;
		}
		return false;
	}
```

Une clé est donc acceptée si c'est **soit** le hash d'un utilisateur (`user::byHash`),
**soit** la clé du plugin nommé. Le contrôle d'origine (`apiModeResult`) gère
`disable` / `whiteip` (liste `security::whiteips`) / `localhost` (127.0.0.1, sauf
en Docker où `localhost` devient `whiteip`).

### 3.4 Les appels GET simples de `jeeApi.php`

`jeeApi.php` commence par charger le cœur et bannir les IP en échec :

```php
header('Access-Control-Allow-Origin: *');
header("Access-Control-Allow-Methods: POST, GET");
header("Access-Control-Allow-Headers: Content-Type");
require_once __DIR__ . "/../php/core.inc.php";
if (user::isBan()) { /* 404 */ }
```

puis, si `type` est présent :

```php
		$plugin = init('plugin', 'core');
		if (in_array($plugin, array('apitts', 'apipro', 'apimarket'))) {
			throw new Exception(__('Vous n\'êtes pas autorisé à effectuer cette action', __FILE__));
		}
		if (!jeedom::apiAccess(init('apikey', init('api')), $plugin)) {
			user::failedLogin();
			sleep(5);
			throw new Exception(…);
		}
```

Types acceptés (lignes 76-244) : `event`, `cmd`, `interact`, `scenario`,
`message`, `object`, `eqLogic`, `command`, `fullData`, `variable`.

Exemples utilisables tels quels :

```bash
# Exécuter une commande action
curl "http://192.168.1.10/core/api/jeeApi.php?apikey=CLE&type=cmd&id=1234"

# Lire la valeur d'une commande info (même appel : execCmd renvoie la valeur)
curl "http://192.168.1.10/core/api/jeeApi.php?apikey=CLE&type=cmd&id=1234"

# Forcer la valeur d'une commande info
curl "http://192.168.1.10/core/api/jeeApi.php?apikey=CLE&type=cmd&id=1234&value=42"

# Lancer un scénario (actions: start|stop|activate|deactivate|enable|disable)
curl "http://192.168.1.10/core/api/jeeApi.php?apikey=CLE&type=scenario&id=12&action=start"

# Lister tous les scénarios (JSON)
curl "http://192.168.1.10/core/api/jeeApi.php?apikey=CLE&type=scenario"

# Tout le dashboard d'un coup : objets + équipements + commandes + valeurs
curl "http://192.168.1.10/core/api/jeeApi.php?apikey=CLE&type=fullData"

# Commandes d'un équipement
curl "http://192.168.1.10/core/api/jeeApi.php?apikey=CLE&type=command&eqLogic_id=57"

# Variable de scénario (lecture si value absent, écriture sinon)
curl "http://192.168.1.10/core/api/jeeApi.php?apikey=CLE&type=variable&name=maVar"
```

`type=fullData` appelle `jeeObject::fullData(null, $_USER_GLOBAL)` : c'est **le**
point d'entrée pour un afficheur qui veut tout l'état de la maison en une requête.

Détail du `type=scenario&action=start` (lignes 160-183) :

```php
				case 'start':
					$tags = array();
					foreach ($_REQUEST as $key => $value) {
						$tags['#' . $key . '#'] = $value;
					}
					/* … */
					$scenario->addTag('trigger','api');
					$scenario->addTag('trigger_message',__('Scénario exécuté sur appel API', __FILE__));
					$scenario_return = $scenario->launch();
```

### 3.5 L'interface JSON-RPC 2.0

POST sur la même URL, corps JSON, ou paramètre `request=`. Squelette :

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "cmd::execCmd",
  "params": { "apikey": "CLE", "id": 1234 }
}
```

Méthodes exposées, relevées exhaustivement par `grep -oP "getMethod\(\) == .[a-zA-Z:_]+."` :

```
ping · version · datetime · getJson
jeedom::isOk · jeedom::halt · jeedom::reboot · jeedom::update · jeedom::backup · jeedom::getUsbMapping
config::byKey · config::save
event::changes
jeeObject::all|byId|full|fullById|save        (alias object::…)
summary::global · summary::byId
timeline::all · timeline::listFolder · timeline::byFolder
datastore::byTypeLinkIdKey · datastore::save
eqLogic::all|byType|byObjectId|byId|fullById|save|byTypeAndId
cmd::all · cmd::byEqLogicId · cmd::byId · cmd::execCmd · cmd::event · cmd::save
cmd::getStatistique · cmd::getTendance · cmd::getHistory
scenario::all|byId|changeState|export|import|save
plugin::listPlugin|install|remove|deamonStart|deamonStop|deamonInfo|deamonInfoAll
plugin::deamonChangeAutoMode · plugin::dependancyInfo · plugin::dependancyInstall
log::add|get|list|empty|remove|getDelta|getLastLine
message::add|all|removeAll|removebyId
interactQuery::all · interact::tryToReply
network::dnsRun|restartDns|stopDns
update::all|checkUpdate|doUpdate|nbNeedUpdate|update
user::all|save|getHash|useTwoFactorAuthentification
```

`cmd::execCmd` accepte `id` scalaire **ou tableau**, et gère `codeAccess` et
`confirmAction` (lignes 751-803). Pour une commande `info` il renvoie
`{"value": …, "collectDate": …}` ; pour une `action` il ne renvoie rien.

`scenario::changeState` prend `state` ∈ `run|stop|enable|disable` :

```php
		if ($params['state'] == 'run') {
			$scenario->addTag('trigger','api');
			$scenario->addTag('trigger_message',__('Scénario exécuté sur appel API', __FILE__));
			$jsonrpc->makeSuccess($scenario->launch());
		}
```

Deux garde-fous configurables côté cœur, à connaître si un appel est refusé sans
raison apparente : `config::byKey('api::forbidden::method')` et
`config::byKey('api::allow::method')`, des expressions régulières appliquées au
nom de la méthode (et au `type` pour les appels GET).

---

## 4. Le motif exact d'un endpoint HTTP de plugin — **le point clé**

### 4.1 Le constat

**Aucun des 14 plugins n'utilise `core/api/`.** Le dossier
`/var/www/html/plugins/plugincam/core/api/` existe mais est vide.
Le motif réellement employé, et qui fonctionne, est **`core/php/<nom>.php`** :

| Plugin | Fichier | Auth |
|---|---|---|
| `pluginmqtt` | `core/php/callback.php` | clé API du plugin, **en en-tête HTTP** |
| `plugincam` | `core/php/jeePluginCam.php` | clé API du plugin, en query string |
| `plugincam2` | `core/php/jeePluginCam2.php` | clé API du plugin, en query string |
| `plugincam` / `plugincam2` | `core/php/snapshot.php` | **session** (`isConnect()`), pas de clé |
| `pluginaspi` | `core/php/map.php` | — (non relu en détail) |

L'URL résultante est directement servie par Apache :
`http://<jeedom>/plugins/<id>/core/php/<fichier>.php`

### 4.2 Motif recommandé — `pluginmqtt/core/php/callback.php`, intégral

C'est le plus abouti : clé par en-tête HTTP, rétrocompatibilité, journalisation
prudente, isolation des erreurs par message.

```php
<?php
/* This file is part of the pluginmqtt plugin for Jeedom.
 * … en-tête AGPL …
 */

/*
 * Le seul point d'entrée du démon vers Jeedom.
 *
 * Ce fichier est appelé pour chaque lot de messages : il est sur le chemin
 * chaud et doit rester court. […]
 *
 * Il ne doit surtout pas y avoir de .htaccess « Deny from all » dans ce
 * dossier : le démon appelle cette URL depuis l'extérieur d'Apache.
 */

require_once __DIR__ . '/../../../../core/php/core.inc.php';

/*
 * La clé arrive par en-tête, et non plus dans la chaîne de requête : celle-ci
 * est journalisée en clair par Apache à chaque appel. init('apikey') reste
 * accepté pour qu'un démon d'une version antérieure, pas encore redémarré,
 * continue de fonctionner le temps de la mise à jour.
 */
$apikey = isset($_SERVER['HTTP_X_PLUGINMQTT_APIKEY']) ? $_SERVER['HTTP_X_PLUGINMQTT_APIKEY'] : init('apikey');

if (!jeedom::apiAccess($apikey, 'pluginmqtt')) {
    echo 'Unauthorized access.';
    $origine = sprintf(__('Accès non autorisé depuis %s', __FILE__),
                       isset($_SERVER['REMOTE_ADDR']) ? $_SERVER['REMOTE_ADDR'] : '?');
    if ($apikey != '') {
        /* Les huit premiers caractères suffisent à reconnaître la clé sans
         * l'écrire en clair dans un journal que d'autres peuvent lire. */
        $origine .= sprintf(__(", avec une clé commençant par %.8s…", __FILE__), $apikey);
    }
    log::add('pluginmqtt', 'error', $origine);
    die();
}

/* Le démon teste la joignabilité de cette URL par un simple GET au démarrage :
 * on répond et on s'arrête là. */
if ($_SERVER['REQUEST_METHOD'] != 'POST') {
    die();
}

require_once __DIR__ . '/../class/pluginmqtt.class.php';

$uid      = init('uid');
$entete   = __('Démon', __FILE__) . ' [' . $uid . '] : ';
$messages = json_decode(file_get_contents('php://input'), true);

if (!is_array($messages)) {
    log::add('pluginmqtt', 'error', $entete . __('corps de requête illisible', __FILE__));
    die();
}

foreach ($messages as $message) {
    if (!isset($message['cmd'])) {
        log::add('pluginmqtt', 'error', $entete . __('message sans clé cmd :', __FILE__) . ' ' . json_encode($message));
        continue;
    }
    /* … validation de l'identité du démon … */
    try {
        switch ($message['cmd']) {
            case 'values':
                if (isset($message['items']) && is_array($message['items'])) {
                    pluginmqttDaemon::onValues($message['items']);
                }
                break;
            /* … 'discovered', 'hb', 'daemonUp', 'daemonDown', 'brokerUp', 'brokerDown' … */
            default:
                log::add('pluginmqtt', 'error', $entete . __('commande inconnue :', __FILE__) . ' ' . $message['cmd']);
        }
    } catch (Throwable $e) {
        /* Un message fautif ne doit jamais empêcher le traitement des suivants :
         * le lot contient peut-être des valeurs parfaitement valides. */
        log::add('pluginmqtt', 'error', $entete . sprintf(
            __('le traitement de « %1$s » a levé : %2$s', __FILE__),
            $message['cmd'], $e->getMessage()
        ));
    }
}
```

### 4.3 Variante avec codes HTTP — `plugincam/core/php/jeePluginCam.php`

```php
<?php
/* … en-tête GPL … */

/*
 * Point d'entrée appelé exclusivement par le démon plugincamd.
 *   GET  ?apikey=…&test=1        → vérification de joignabilité au démarrage
 *   GET  ?apikey=…&action=config → configuration des NVR à écouter
 *   POST ?apikey=…  + corps JSON → remontée d'un lot d'événements
 */

require_once __DIR__ . '/../../../../core/php/core.inc.php';
/* L'autochargeur de Jeedom ne résout que la classe portant le nom du plugin.
 * plugincam.class.php inclut plugincamRule : sans cet appel explicite, une évolution qui
 * sortirait de la boucle d'événements plus tôt ferait échouer checkHold() sur
 * une classe introuvable, et l'erreur serait avalée par le catch. */
require_once __DIR__ . '/../class/plugincam.class.php';

if (!jeedom::apiAccess(init('apikey'), 'plugincam')) {
    /* 401 et non 200 : le démon ne dispose que du code HTTP pour savoir si son lot
     * a été pris. Répondre 200 sur un refus lui fait jeter des événements que
     * Jeedom n'a jamais enregistrés, sans la moindre trace d'un côté ni de l'autre. */
    http_response_code(401);
    echo __('Vous n\'êtes pas autorisé à effectuer cette action', __FILE__);
    die();
}

if (init('test') != '') {
    echo 'OK';
    die();
}

if (init('action') == 'config') {
    header('Content-Type: application/json');
    echo json_encode(plugincam::getDaemonConfig());
    die();
}

$input = json_decode(file_get_contents('php://input'), true);
if (!is_array($input) || empty($input)) {
    die();
}

$events = isset($input['events']) && is_array($input['events']) ? $input['events'] : array($input);

foreach ($events as $event) {
    try {
        handleCamEvent($event);
    } catch (Throwable $e) {
        log::add('plugincam', 'error', __('Traitement de l\'événement en échec :', __FILE__)
               . ' ' . $e->getMessage() . ' — ' . json_encode($event));
    }
}
/* … */
echo 'OK';
```

### 4.4 Variante « session » — `plugincam/core/php/snapshot.php`

Pour servir un fichier à un **utilisateur connecté** plutôt qu'à une machine :

```php
require_once __DIR__ . '/../../../../core/php/core.inc.php';
require_once __DIR__ . '/../class/plugincam.class.php';
include_file('core', 'authentification', 'php');

if (!isConnect()) {
    header('HTTP/1.0 401 Unauthorized');
    die('401 - Unauthorized');
}
```

### 4.5 Construire l'URL et transmettre la clé au client

```php
    public static function getCallbackUrl() {
        return network::getNetworkAccess('internal', 'http:127.0.0.1:port:comp')
             . '/plugins/plugincam/core/php/jeePluginCam.php';
    }
```

Et la clé, **jamais** sur la ligne de commande :

```php
        $cmd  = 'php ' . escapeshellarg($daemon);
        $cmd .= ' --callback '   . escapeshellarg(self::getCallbackUrl());
        $cmd .= ' --pid '        . escapeshellarg(jeedom::getTmpFolder(__CLASS__) . '/deamon.pid');
        $cmd .= ' --socketport ' . escapeshellarg(config::byKey('socketport', __CLASS__, 55060));
        $cmd .= ' --loglevel '   . escapeshellarg(log::convertLogLevel(log::getLogLevel(__CLASS__)));

        /* La clé d'API passe par l'entrée standard et jamais par la ligne de
         * commande : ps est lisible par n'importe quel utilisateur local. */
        $full = 'echo ' . escapeshellarg(jeedom::getApiKey(__CLASS__)) . ' | ' . $cmd
              . ' >> ' . log::getPathToLog(__CLASS__ . 'd') . ' 2>&1 &';
        exec($full);
```

### 4.6 Check-list pour écrire l'endpoint d'un nouveau plugin

1. Fichier dans `core/php/`, **pas** de `.htaccess` dans ce dossier.
2. `require_once __DIR__ . '/../../../../core/php/core.inc.php';` (quatre niveaux :
   `plugins/<id>/core/php/` → racine).
3. `require_once __DIR__ . '/../class/<id>.class.php';` si le fichier utilise des
   classes annexes du plugin (l'autoload du cœur ne résout **que** la classe
   homonyme du plugin).
4. Lire la clé : en-tête HTTP dédié de préférence (`$_SERVER['HTTP_X_<ID>_APIKEY']`),
   `init('apikey')` en repli.
5. `if (!jeedom::apiAccess($apikey, '<id>')) { http_response_code(401); … die(); }`
6. Répondre à un GET de sonde avant tout traitement.
7. `json_decode(file_get_contents('php://input'), true)` pour le corps.
8. `try { … } catch (Throwable $e) { log::add('<id>', 'error', …); }` **autour de
   chaque élément du lot**, pas autour du lot entier.
9. Côté client : `jeedom::getApiKey('<id>')` fournit la clé ;
   `network::getNetworkAccess('internal', 'http:127.0.0.1:port:comp')` fournit la base d'URL.

---

## 5. Configuration d'un plugin

Deux étages, complémentaires.

### 5.1 Les défauts : `core/config/<id>.config.ini`

Fichier INI, **section nommée avec l'`id` du plugin**. Avertissement recopié dans
plusieurs plugins :

```ini
; La section doit porter l'identifiant du plugin : nommée [default], elle serait
; entièrement inerte et chaque config::byKey retomberait sur son défaut codé.
[plugincam2]
socketport = 55061
event_heartbeat = 10
reconnect_delay = 15
pulse_duration = 5
snapshot_on_ring = 1
snapshot_keep = 50
allow_open_door = 0
```

Les clés peuvent être hiérarchiques avec `::` (`pluginmqtt`) :

```ini
[pluginmqtt]
broker::host = ""
broker::port = 1883
broker::tls = 0
daemon::socketport = 55062
daemon::batchDelay = 0.2
discovery::enabled = 1
topics::exclude = "jeedom/#
$SYS/#"
```

Plus simple, `pluginhoraires` :

```ini
[pluginhoraires]
api_timeout = 8
lang =
```

### 5.2 Lecture et écriture

```php
config::byKey('api_timeout', 'pluginhoraires', 8)   // clé, plugin, défaut
config::save('showUnassigned', 1, 'pluginexemple')
config::byKey('api')                          // clé du coeur (plugin = 'core')
```

**Piège documenté dans `pluginexemple/plugin_info/install.php`** :

> `config::byKey()` ne renvoie jamais `null` sur une clé absente : elle place le
> défaut en cache puis retourne `''` parce que `isset()` est faux sur `null`. Le
> test doit donc porter sur la chaîne vide.

```php
if (config::byKey('showUnassigned', 'pluginexemple', '') === '') {
    config::save('showUnassigned', 1, 'pluginexemple');
}
```

Les valeurs sont stockées dans la table SQL `config` (colonnes `key`, `value`,
`plugin`). Le `.config.ini` ne fournit que les défauts.

Une valeur propre à l'installation et non versionnée peut aussi vivre dans
`core/config/common.config.php` — exclu du git et du déploiement.

### 5.3 La page de réglages : `plugin_info/configuration.php`

Réel, `pluginverif` :

```php
<?php
if (!isConnect('admin')) {
	throw new Exception('{{401 - Accès non autorisé}}');
}
?>
<form class="form-horizontal">
	<fieldset>
		<legend><i class="fas fa-shield-alt"></i> {{Accès aux pages}}</legend>

		<div class="form-group">
			<label class="col-md-4 control-label">{{Proxy de lecture}}</label>
			<div class="col-md-3">
				<input class="configKey form-control" data-l1key="proxy" placeholder="https://r.jina.ai/">
			</div>
			<div class="col-md-5">
				<span class="help-block" style="margin:0;">{{L'adresse du proxy, barre oblique finale comprise […]}}</span>
			</div>
		</div>

		<div class="form-group">
			<label class="col-md-4 control-label">{{Délai d'attente}}</label>
			<div class="col-md-2">
				<input type="number" class="configKey form-control" data-l1key="api_timeout" placeholder="45">
			</div>
			<div class="col-md-6">
				<span class="help-block" style="margin:0;">{{Secondes avant d'abandonner la lecture d'une page. […]}}</span>
			</div>
		</div>
	</fieldset>
</form>
```

Le contrat : un `<input class="configKey" data-l1key="<clé>">`. Le cœur se charge
seul de lire et d'enregistrer ; **aucun JS à écrire**. La clé `data-l1key`
correspond exactement à la clé lue par `config::byKey('<clé>', '<id>')`, et le
`placeholder` reprend en général la valeur par défaut du `.config.ini`.
`{{…}}` = traduction. `isConnect('admin')` obligatoire en tête.

### 5.4 Configuration d'un **équipement** (≠ configuration du plugin)

Dans `desktop/php/<id>.php`, les champs portent `class="eqLogicAttr"` avec
`data-l1key="configuration"` et `data-l2key="<clé>"`. Côté PHP :

```php
$this->getConfiguration('from_id', '')   // lecture, avec défaut
$this->setConfiguration('threshold', 5)  // écriture
```

---

## 6. Déclencher une commande et un scénario depuis PHP

### 6.1 Commandes

```php
$cmd = cmd::byId($id);
if (!is_object($cmd)) { /* … */ }

// Commande ACTION : l'exécuter
$cmd->execCmd();                              // sans options
$cmd->execCmd(array('slider' => 40));         // avec options
$cmd->execCmd(array('color'  => '#ff8800'));
$cmd->execCmd(array('title' => 'Alerte', 'message' => 'Texte'));   // subType message

// Commande INFO : lire sa valeur (lecture en cache, pas de requête SQL)
$valeur = $cmd->execCmd();

// Commande INFO : publier une valeur
$cmd->event($valeur);
// ou, depuis l'eqLogic, par logicalId, avec détection de changement :
$eqLogic->checkAndUpdateCmd('summary', $texte);
```

> **`execCmd()` et non `execute()`.** `execute()` est la méthode que le *plugin*
> implémente ; `execCmd()` est celle que l'*appelant* utilise : elle applique les
> droits sur l'eqLogic désactivé, la timeline, `alreadyInState`, la journalisation
> dans `log/event`, et n'appelle `execute()` qu'ensuite.

### 6.2 Scénarios

```php
$scenario = scenario::byId($id);
if (is_object($scenario)) {
    $scenario->addTag('trigger', 'monplugin');
    $scenario->addTag('trigger_message', 'Lancé par monplugin');
    $scenario->setTags(array('#piece#' => 'salon'));   // facultatif
    $retour = $scenario->launch();
}

$scenario->stop();
$scenario->setIsActive(0); $scenario->save();   // désactiver
$scenario->setIsActive(1); $scenario->save();   // activer

$tous = scenario::all();
$scenario->getState();        // 'in progress', 'stop', …
$scenario->getLastLaunch();
$scenario->hasRight('x', $user);   // 'x' = exécuter, 'r' = lire, 'w' = écrire
```

### 6.3 Lister les commandes — ce qu'il faut savoir pour une UI

```php
cmd::all()                                          // toutes, coûteux
cmd::byId($id)
cmd::byIds(array($a, $b))
cmd::byEqLogicId($eqLogic_id)
cmd::byEqLogicIdCmdName($eqLogic_id, $name)
cmd::byGenericType('LIGHT_ON', $eqLogic_id = null, $one = false)
cmd::byGenericTypeObjectId('LIGHT_STATE', $object_id, $type)
$eqLogic->getCmd(null, $logicalId)                  // par logicalId
$eqLogic->getCmd('action', $logicalId)              // typée
$eqLogic->getCmd()                                  // toutes les cmds de l'eqLogic
```

Filtrage sur le type : `$cmd->getType()` vaut `'info'` ou `'action'`.
`$cmd->getSubType()` : pour `info` → `string|numeric|binary|other` ;
pour `action` → `other|slider|color|message|select`.

**Le motif « actionnable » réel**, relevé dans `pluginexemple::deviceModel()` — c'est
exactement le cas d'un afficheur qui propose des boutons :

```php
        /* Voir sans pouvoir agir est un droit à part entière dans Jeedom, et
         * core/ajax/cmd.ajax.php refuse l'exécution sans le droit « x ». Envoyer
         * quand même les boutons donnerait un dashboard qui répond par une
         * alerte rouge à chaque appui : on les retire à la source. */
        $canExecute = (!is_object($_user) || $_eqLogic->hasRight('x', $_user));

        foreach ($_eqLogic->getCmd() as $cmd) {
            $entry = array(
                'id'      => intval($cmd->getId()),
                'name'    => $cmd->getName(),
                'type'    => $cmd->getType(),
                'subType' => $cmd->getSubType(),
                'unit'    => $cmd->getUnite(),
                'generic' => $cmd->getGeneric_type(),
                'visible' => ($cmd->getIsVisible() == 1),
                'invert'  => ($cmd->getSubType() == 'binary' && $cmd->getDisplay('invertBinary') == 1),
                'history' => ($cmd->getIsHistorized() == 1),
            );
            if ($cmd->getSubType() == 'slider' || $cmd->getSubType() == 'numeric') {
                $entry['min'] = ($cmd->getConfiguration('minValue', '') === '') ? 0 : floatval($cmd->getConfiguration('minValue'));
                $entry['max'] = ($cmd->getConfiguration('maxValue', '') === '') ? 100 : floatval($cmd->getConfiguration('maxValue'));
            }
            if ($cmd->getConfiguration('listValue', '') != '') {
                $entry['list'] = self::parseListValue($cmd->getConfiguration('listValue'));
            }
            if ($entry['type'] == 'action' && !$canExecute) {
                continue;               // ← le filtre qui compte
            }
            $meta[$entry['id']] = $entry;
        }
```

Et le même filtrage pour les scénarios :

```php
    private static function scenarioList($_user) {
        $return = array();
        foreach (scenario::all() as $scenario) {
            if ($scenario->getIsActive() != 1 || $scenario->getIsVisible() != 1) {
                continue;
            }
            /* Le droit « x » et non « r » : un scénario qu'on ne peut que voir
             * n'a pas de carte, puisque la carte ne sait qu'une chose, le
             * lancer. */
            if (is_object($_user) && !$scenario->hasRight('x', $_user)) {
                continue;
            }
            $return[] = array(
                'id'     => intval($scenario->getId()),
                'name'   => $scenario->getName(),
                'icon'   => trim($scenario->getIcon(true)),
                'group'  => $scenario->getGroup(),
                'roomId' => intval($scenario->getObject_id(0)),
                'state'  => $scenario->getState(),
                'last'   => $scenario->getLastLaunch(),
            );
        }
        return $return;
    }
```

**Les types génériques** sont la bonne clé pour comprendre à quoi sert une
commande sans connaître le plugin qui l'a créée. Le cœur en décrit 171, rangés en
familles, accessibles par `jeedom::getConfiguration('cmd::generic_type')`.
`pluginexemple` a choisi de ne pas dépendre de cette table à l'exécution et déclare
la carte des types qui l'intéressent :

```php
    const CARDS = array(
        'light' => array(
            'state'      => array('LIGHT_STATE', 'LIGHT_STATE_BOOL'),
            'slider'     => array('LIGHT_SLIDER'),
            'brightness' => array('LIGHT_BRIGHTNESS'),
            'on'         => array('LIGHT_ON'),
            'off'        => array('LIGHT_OFF'),
            'toggle'     => array('LIGHT_TOGGLE'),
            'color'      => array('LIGHT_COLOR'),
            'setColor'   => array('LIGHT_SET_COLOR'),
        ),
        'cover' => array(
            'state'  => array('FLAP_STATE', 'FLAP_BSO_STATE'),
            'slider' => array('FLAP_SLIDER'),
            'up'     => array('FLAP_UP', 'FLAP_BSO_UP'),
            'down'   => array('FLAP_DOWN', 'FLAP_BSO_DOWN'),
            /* … */
        ),
        /* … prise, chauffage, caméra, multimédia, capteur … */
    );
    const PRIMARY_ROLES = array('state', 'on', 'off', 'toggle', 'slider', 'up', 'down');
```

Pour un afficheur externe (ESP32), le chemin le plus court est
`type=fullData` sur l'API HTTP (§3.4) : objets + équipements + commandes +
valeurs courantes, filtrés par les droits de la clé utilisée, en une requête.

### 6.4 Équipements

```php
eqLogic::byType('pluginhoraires')            // tous
eqLogic::byType('pluginhoraires', true)      // seulement les activés  ← ce qu'utilisent les crons
eqLogic::byId($id)
eqLogic::byLogicalId($logicalId, 'pluginhoraires')
eqLogic::byObjectId($object_id)
eqLogic::byTypeAndSearchConfiguration('plugincam', array('type' => plugincam::TYPE_CAMERA), true)
jeeObject::all()  ·  jeeObject::buildTree(null, false)  ·  jeeObject::fullData(array(), $user)
```

---

## 7. Les logs

### 7.1 Chemin et contenu réel

`/var/www/html/log/`, propriétaire `www-data:www-data`, un fichier **sans
extension** par canal :

```
cron_execution   plugincam       plugincamd      event        http.error
jeedom           pluginmqtt      pluginmqttd     packages     scenario_execution
scenarioLog/     pluginhoraires    starting    .htaccess
(+ un fichier par autre plugin installé)
```

Le nom du fichier est le premier argument de `log::add()`. Par convention :
`<id>` pour le plugin, `<id>d` pour son démon (`plugincamd`, `pluginmqttd`, `plugincam2d`).

### 7.2 `log::add()` — signature réelle

`/var/www/html/core/class/log.class.php`, ligne 111 :

```php
	public static function add(string $_log, string $_type, string $_message, string $_logicalId = '') {
		if (trim($_message) == '') {
			return;
		}
		$level = (isset(self::$level[strtolower($_type)])) ? self::$level[strtolower($_type)] : 100;
		if ($level < self::getLogLevel($_log)) {
			return;
		}
		$fp = fopen(self::getPathToLog($_log), 'a');
		fwrite($fp, '[' . date('Y-m-d H:i:s') . '][' . strtoupper($_type) . '] ' . $_message . "\n");
		fclose($fp);
		try {
			$action = '<a href="/index.php?v=d&p=log&logfile=' . $_log . '">' . __('Log', __FILE__) . ' ' . $_log . '</a>';
			if ($level == 400 && self::getConfig('addMessageForErrorLog') == 1) {
				@message::add($_log, $_message, $action, $_logicalId);
			} elseif ($level >= 500 && $_log != 'update') {
				@message::add($_log, $_message, $action, $_logicalId);
			}
		} catch (Exception $e) {
		}
	}
```

Usage :

```php
log::add('monplugin', 'debug',   'message');
log::add('monplugin', 'info',    'message');
log::add('monplugin', 'warning', 'message');
log::add('monplugin', 'error',   'message', 'identifiantLogique');  // 4e arg = clé du message
log::add(__CLASS__, 'error', $eqLogic->getHumanName() . ' : ' . $e->getMessage());
```

Niveaux : `debug` (100) < `info` (200) < `warning` (300) < `error` (400) <
`critical`/`alert`/`emergency` (≥ 500). Le niveau effectif d'un canal vient de
`log::getLogLevel('<canal>')`, réglable par canal dans l'UI.

**`error` (400) ne crée un message au centre de messages que si le réglage global
`addMessageForErrorLog` est actif — il ne l'est pas par défaut.** D'où le motif
réel de `plugincam` quand il faut vraiment prévenir l'utilisateur :

```php
        log::add(__CLASS__, 'error', __('Impossible de lancer le démon, consultez le log plugincamd', __FILE__), 'unableStartDeamon');
        /* log::add ne pose un message qu'avec le réglage global addMessageForErrorLog,
         * inactif par défaut : sans ce message::add, un démon qui refuse de démarrer
         * (identifiants faux, port occupé) ne se voit que dans l'onglet Santé. */
        message::add(__CLASS__, __('Impossible de lancer le démon, consultez le log plugincamd', __FILE__), '', 'unableStartDeamon');
```

Et le pendant, à ne pas oublier dans `preRemove()` :
`message::removeAll('monplugin', 'unableStartDeamon');`

Autres API utiles :

```php
log::getPathToLog('monplugin')            // → /var/www/html/log/monplugin
log::getLogLevel('monplugin')             // niveau numérique effectif
log::convertLogLevel(log::getLogLevel('monplugin'))   // → 'debug' | 'info' | … | 'none'
log::exception($e)                        // trace formatée
log::clear('monplugin')
```

### 7.3 Consulter les logs

```bash
# En ligne
tail -f /var/www/html/log/monplugin
tail -f /var/www/html/log/http.error

# Les logs applicatifs sont lisibles sans sudo (o+r)
sshpass -e ssh <utilisateur>@192.168.1.10 'tail -50 /var/www/html/log/pluginhoraires'
```

Dans l'UI : **Analyse → Logs**, ou l'URL directe
`index.php?v=d&p=log&logfile=<canal>`. Par l'API JSON-RPC : `log::list`,
`log::get`, `log::getLastLine`, `log::getDelta`, `log::empty`.

### 7.4 `/var/www/html/log/http.error` — **le premier endroit à regarder**

C'est le journal d'erreurs d'Apache, et **le seul endroit où apparaissent les
erreurs fatales PHP** : un plugin qui « ne fait rien » à l'enregistrement, une
page blanche, un 500 muet, le piège `setCmd()` de la §1.4. Le journal du plugin,
lui, reste silencieux dans ces cas.

Extrait réel du 21 septembre :

```
[Mon Sep 21 15:03:25.413072 2026] [php:error] [pid 179602:tid 179602] [client 127.0.0.1:60008] PHP Fatal error:  Uncaught Error: Call to undefined function isConnect() in /var/www/html/plugins/pluginexemple/desktop/php/pluginexemple.php:15\nStack trace:\n#0 {main}\n  thrown in /var/www/html/plugins/pluginexemple/desktop/php/pluginexemple.php on line 15
[Sun Sep 20 19:55:12.975620 2026] [access_compat:error] [pid 1117:tid 1117] [client 127.0.0.1:55564] AH01797: client denied by server configuration: /var/www/html/plugins/pluginaspi/core/class/pluginaspi.class.php
```

La première ligne est le symptôme d'une page `desktop/php` servie directement par
Apache au lieu d'être incluse par `index.php`. Les lignes `AH01797: client denied`
sont **normales** : c'est le `.htaccess` `Deny from all` qui fait son travail.

> Note de lecture : `http.error` est en `-rwxrwxr-x www-data:www-data` ; il est
> donc lisible par `<utilisateur>` sans sudo.

---

## 8. Droits, propriétaire, activation d'un plugin

### 8.1 Propriétaire et droits

```
drwxrwxr-x  6 www-data www-data  4096 21 sep 12:44 /var/www/html/plugins/pluginexemple/
drwxrwxr-x  5 www-data www-data  4096 21 sep 09:48 core
-rwxrwxr-x  1 www-data www-data 34523 21 sep 09:48 LICENSE
```

Tout appartient à `www-data:www-data`, en `775` / `775`.
`id <utilisateur>` → `uid=1000(<utilisateur>) … groupes=1000(<utilisateur>),24(cdrom),25(floppy),27(sudo),29(audio),30(dip),44(video),46(plugdev),100(users),101(netdev)` :
**`<utilisateur>` n'est pas dans `www-data`**, mais il est dans `sudo`.

Conséquence pratique, telle que la formulent les consignes de `/home/<utilisateur>/dev` :

> Le déploiement fonctionne malgré tout, les fichiers restant lisibles par Apache,
> mais pour que les **nouveaux** fichiers reçoivent le groupe `www-data` il faut
> lancer l'outil avec `sudo`.

### 8.2 Comment Jeedom détecte un plugin

`plugin::listPlugin()` (`core/class/plugin.class.php`, ligne 208) :

```php
		if ($_activateOnly) {
			$sql = "SELECT plugin
			FROM config
			WHERE `key`='active'
			AND `value`='1'";
			/* … */
		} else {
			$rootPluginPath = __DIR__ . '/../../plugins';
			foreach (ls($rootPluginPath, '*') as $dirPlugin) {
				if (is_dir($rootPluginPath . '/' . $dirPlugin)) {
					$pathInfoPlugin = $rootPluginPath . '/' . $dirPlugin . 'plugin_info/info.json';
					if (!file_exists($pathInfoPlugin)) {
						continue;
					}
					/* … */
				}
			}
		}
```

Donc :

- **Détection** = parcours du système de fichiers. Un dossier dans
  `/var/www/html/plugins/` contenant `plugin_info/info.json` **est** un plugin,
  immédiatement, sans cache à vider ni commande à lancer. Il apparaît dans
  *Plugins → Gestion des plugins*.
- **Activation** = une ligne en base : table `config`, `key='active'`,
  `plugin='<id>'`, `value='1'`.

`plugin::isActive()` lit ce drapeau (via `config::getPluginEnable()`, mis en
cache statique par requête).

### 8.3 Ce que fait `setIsEnable(1)`

`core/class/plugin.class.php`, ligne 939 :

```php
	public function setIsEnable($_state, $_force = false, $_foreground = false) {
		if (version_compare(jeedom::version(), $this->getRequire()) == -1 && $_state == 1) {
			throw new Exception(__('Votre version de Jeedom n\'est pas assez récente pour activer ce plugin', __FILE__));
		}
		$osVersion = $this->getRequireOsVersion();
		$distrib = system::getDistrib();
		if (isset($osVersion)) {
			if ($distrib == 'debian' && version_compare(system::getOsVersion(), $osVersion) == -1 && $_state == 1) {
				throw new Exception(sprintf(__('Votre version Debian n\'est pas assez récente […] %s minimum demandé', __FILE__), $osVersion));
			}
		}
		$alreadyActive = config::byKey('active', $this->getId(), 0);
		if ($_state == 1) {
			config::save('active', $_state, $this->getId());
		}
		/* … désactivation : met tous les eqLogic du type à isEnable=0/isVisible=0,
		   en mémorisant previousIsEnable/previousIsVisible, et supprime les listeners … */
		} else if ($alreadyActive == 0 && $_state == 1) {
			try {
				include_file('core', $this->getId(), 'class', $this->getId());
			} catch (\Throwable $e) {
			}
			foreach (eqLogic::byType($this->getId()) as $eqLogic) { /* restaure … */ }
		}
		try {
			if ($_state == 1) {
				log::add($this->getId(), 'info', 'Début d\'activation du plugin');
				$this->deamon_stop();
				/* … démon, dépendances, callInstallFunction('install'|'update') … */
```

Conséquences à retenir :

- `require` et `requireOsVersion` d'`info.json` **bloquent l'activation**, pas
  seulement l'installation. Sur ce serveur : Jeedom 4.6.1 et Debian 13 — donc
  `"require": "4.4"` et `"requireOsVersion": "11"` passent sans problème.
- L'activation charge la classe du plugin (`include_file('core', <id>, 'class', <id>)`).
  **Une classe absente ou en erreur de syntaxe fait échouer l'activation.**
- Elle déclenche `plugin_info/install.php` → `<id>_install()` / `<id>_update()`.

### 8.4 Les trois façons d'activer

**a) Interface web** — la voie normale :
*Plugins → Gestion des plugins → \<le plugin\> → bouton « Activer »*, puis
*Configuration* pour la page de réglages. La page du plugin apparaît alors dans
le menu, à l'URL `index.php?v=d&p=<id>`.

**b) Ligne de commande** — `core/php/jeecli.php` :

```bash
# Installer depuis le Market
sudo -u www-data php /var/www/html/core/php/jeecli.php plugin install <id>

# Installer depuis GitHub : plugin install [id] [user] [repository=id] [branch=master]
sudo -u www-data php /var/www/html/core/php/jeecli.php plugin install monplugin <compte-github> jeedom-plugin-monplugin beta
```

Le code correspondant se termine par :

```php
                $plugin = plugin::byId($argv[3]);
                if (!is_object($plugin)) {
                    echo "Error plugin not found";
                    …
                }
                if ($plugin->setIsEnable(1,true,true)) { … }
```

> **Attention** : `jeecli.php plugin install` crée un objet `update` et **télécharge**
> le plugin depuis le Market ou GitHub. Il écrase donc un dossier déposé à la main.
> Ce n'est pas la bonne voie pour un plugin développé localement.

**c) API JSON-RPC** : `plugin::install`, `plugin::remove`, `plugin::listPlugin`.

### 8.5 Créer `/var/www/html/plugins/<id>` la première fois

`deploy-plugin.sh` refuse de créer le dossier :

```bash
CIBLE="$RACINE_PLUGINS/$IDENTIFIANT"
if [[ ! -d "$CIBLE" ]]; then
    echo "Dossier cible introuvable : $CIBLE" >&2
    echo "Installer le plugin dans Jeedom une première fois, ou créer le dossier." >&2
    exit 1
fi
```

C'est délibéré : un `rsync --delete` vers un chemin créé par erreur de frappe
serait irrattrapable. Pour le premier déploiement d'un plugin neuf, deux options.

**Option 1 — créer le dossier à la main (le plus direct).**

```bash
export SSHPASS='<mot-de-passe-ssh>'
sshpass -e ssh -o StrictHostKeyChecking=no <utilisateur>@192.168.1.10 \
  "printf '%s\n' "$SSHPASS" | sudo -S -p '' install -d -o www-data -g www-data -m 775 /var/www/html/plugins/<id>"

# puis, depuis le dépôt, avec sudo pour que les NOUVEAUX fichiers prennent le groupe www-data
sshpass -e ssh -o StrictHostKeyChecking=no <utilisateur>@192.168.1.10 \
  "cd /home/<utilisateur>/dev/jeedom-plugin-<id> && printf '%s\n' "$SSHPASS" | sudo -S -p '' /home/<utilisateur>/dev/tools/deploy-plugin.sh"
```

Puis activer dans l'UI (*Plugins → Gestion des plugins*) : le plugin apparaît
dès que `plugin_info/info.json` est présent sur disque.

**Option 2 — première installation par l'UI depuis GitHub.**
*Plugins → Gestion des plugins → Ajouter → Github*, avec `user`, `repository`,
`branch = beta`. C'est le seul moyen de valider le vrai chemin de mise à jour
que vivront les utilisateurs, et cela crée le dossier avec les bons droits.
Ensuite, `deploy-plugin.sh` prend le relais pour les itérations de développement.

**Contrôle après premier déploiement :**

```bash
sshpass -e ssh <utilisateur>@192.168.1.10 'ls -la /var/www/html/plugins/<id>/ && \
  php -l /var/www/html/plugins/<id>/core/class/<id>.class.php && \
  tail -20 /var/www/html/log/http.error'
```

---

## 9. Le déploiement : `deploy-plugin.sh`

`/home/<utilisateur>/dev/tools/deploy-plugin.sh` (3 165 o). Il lit l'`id` dans
`plugin_info/info.json` et en déduit la destination — générique, aucune
configuration.

```bash
/home/<utilisateur>/dev/tools/deploy-plugin.sh                  # plugin du dossier courant
/home/<utilisateur>/dev/tools/deploy-plugin.sh --simulation     # dry-run
/home/<utilisateur>/dev/tools/deploy-plugin.sh ~/dev/jeedom-plugin-autre
JEEDOM_PLUGINS=/autre/racine /home/<utilisateur>/dev/tools/deploy-plugin.sh
```

Lancé depuis un sous-dossier quelconque, il remonte jusqu'à la racine du dépôt
(présence de `plugin_info/info.json`). L'appel rsync réel :

```bash
OPTIONS=(-rlptD --delete --itemize-changes)   # -rlptD et non -a : propriétaire/groupe
                                              # de la cible conservés

rsync "${OPTIONS[@]}" "${EXCLUSIONS_LOCALES[@]}" \
    --exclude='.git/' \
    --exclude='.github/' \
    --exclude='.gitignore' \
    --exclude='.deployignore' \
    --exclude='tools/' \
    --include='/README.md' \
    --exclude='/*.md' \
    --exclude='data/snapshots/' \
    --exclude='core/config/common.config.php' \
    --exclude='*.log' \
    --exclude='*.pid' \
    "$SOURCE/" "$CIBLE/"

if ! chgrp -R www-data "$CIBLE" 2>/dev/null; then
    echo "Note : groupe www-data non appliqué, relancer avec sudo pour le corriger."
fi
```

Ce qui n'est **jamais** copié, donc jamais écrasé côté Jeedom :
`data/snapshots/`, `*.log`, `*.pid`, `core/config/common.config.php`.
Ce qui reste au développement : `.git/`, `.github/`, `.gitignore`,
`.deployignore`, `tools/`, et les `*.md` de la racine **sauf `README.md`**.
`docs/` **est** déployé : Jeedom s'en sert.

Un `.deployignore` à la racine du dépôt ajoute des motifs rsync, un par ligne.
`--delete` est actif : un fichier supprimé du dépôt disparaît de l'installation.

**Règle du projet, énoncée dans les consignes de `/home/<utilisateur>/dev` :**

> 1. Toute modification se fait dans le dépôt, jamais dans `/var/www/html/plugins/<id>`.
> 2. Chaque modification est immédiatement redéployée, sans attendre qu'on le demande.
>    Une modification non déployée est une modification invisible.
> 3. Le déploiement fait partie de la tâche. Il est annoncé dans la réponse.

---

## 10. Ce qui n'a pas pu être vérifié

Tout ce qui précède a été lu sur le serveur. Les points suivants sont des limites
explicites de ce relevé :

1. **Le contenu exact de `core/config/jeedom.config.php`** (liste des catégories,
   table `cmd::generic_type` avec ses 171 entrées) n'a pas été ouvert : la liste
   des catégories de la §2.2 et le nombre 171 proviennent de
   `STRUCTURE-PLUGIN-JEEDOM.md` et d'un commentaire de `pluginexemple.class.php`, pas
   d'une lecture directe du fichier de configuration du cœur.
2. **Les numéros de ligne cités pour `update.class.php` (~324), `DB.class.php` (~556),
   `utils.class.php` (~117), `eqLogic.ajax.php` (~571), `eqLogic.class.php` (693),
   `utils.inc.php` (167), `scenario.class.php` (1050)** proviennent de
   `STRUCTURE-PLUGIN-JEEDOM.md` et des commentaires des plugins. Ils n'ont pas été
   revérifiés un à un sur le serveur ; les extraits de `jeedom.class.php`,
   `jeeApi.php`, `log.class.php`, `plugin.class.php` et `cmd.class.php`, eux, ont
   été lus directement et leurs lignes sont exactes.
3. **La liste complète des hooks eqLogic** (`preInsert`, `postInsert`, `preUpdate`,
   `postUpdate`, `postRemove`) n'a pas été confirmée par un grep dans
   `eqLogic.class.php` : le grep tenté sur `method_exists($this, …)` n'a rien
   renvoyé, le cœur les appelle autrement. Ce qui **est** vérifié, c'est que les
   14 plugins n'utilisent que `preSave` / `postSave` / `preRemove`.
4. **`proApi.php`, `marketApi.php`, `tts.php`** n'ont pas été lus.
5. **Le contenu de `core/php/map.php` (pluginaspi)** et des fichiers `desktop/modal/`
   n'a pas été relu : les motifs d'endpoint de la §4 viennent de `pluginmqtt` et `plugincam`.
6. **La partie « Publier sur le Market »** de `STRUCTURE-PLUGIN-JEEDOM.md`
   (compte développeur, formulaire, synchronisation quotidienne vers 12h10,
   droits à donner à l'utilisateur GitHub `jeedom-market`) n'est pas reprise ici :
   elle n'a pas d'incidence sur le développement local. Se reporter au document
   source, section 7.
7. **Aucune écriture n'a été faite sur le serveur.** Les commandes de création de
   dossier et de déploiement de la §8.5 sont proposées, pas exécutées.
8. **`jeedom::getApiKey('<id>')` n'a pas été appelé** pour un plugin inexistant :
   la clé d'un futur plugin n'existe pas encore en base et ne sera créée qu'au
   premier appel, une fois le plugin présent dans `plugins/`.
