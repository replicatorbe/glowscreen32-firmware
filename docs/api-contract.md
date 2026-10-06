# Contrat d'API GlowScreen32 — v3.0

Contrat figé entre le plugin Jeedom et le firmware ESP32. **Les deux côtés doivent s'y tenir :
toute modification se décide ici d'abord.**

## Principe

Un « GlowScreen » = un eqLogic Jeedom = **un** écran physique, identifié par l'**adresse MAC**
de son ESP32. L'utilisateur choisit dans Jeedom quels boutons apparaissent sur cet écran ; la
carte interroge le plugin au démarrage pour récupérer sa mise en page, puis notifie les appuis.

### Plusieurs écrans

Le parc est **multi-écrans par construction** : on crée autant d'eqLogic que d'écrans, chacun
avec sa propre MAC et son propre jeu de boutons. Conséquences, à respecter des deux côtés :

- La MAC est la **clé d'identification unique**. Le plugin doit refuser deux eqLogic portant la
  même MAC (message d'erreur explicite à la sauvegarde).
- Le champ `version` est **propre à chaque écran** : modifier la configuration de l'écran du
  salon ne doit pas forcer celui de la cuisine à se rafraîchir.
- Le firmware est **identique sur toutes les cartes** — aucune configuration par écran n'est
  compilée dedans. Une carte découvre son identité en lisant sa propre MAC au démarrage. On
  flashe le même binaire partout.
- Le nom affiché dans le bandeau vient du champ `name` renvoyé par `layout`, donc de Jeedom.

### Enrôlement d'un écran neuf

Une carte flashée mais pas encore déclarée dans Jeedom reçoit `unknown_device`. Dans ce cas
elle **affiche sa propre MAC en grand à l'écran**, pour que l'utilisateur puisse créer
l'eqLogic correspondant sans avoir à brancher un câble série. Elle continue d'interroger le
plugin toutes les 10 s et bascule automatiquement en mode normal dès qu'elle est reconnue.

Carte de référence pour les tests : MAC `24:6f:28:12:34:56`, normalisée en minuscules sans
séparateur : `246f28123456`.

## Point d'entrée

```
http://192.168.1.10/plugins/glowscreen32/core/php/api.php
```

> **Corrigé en v1.1.** La v1 plaçait l'endpoint dans `core/api/`. L'inspection des 14 plugins
> du serveur montre qu'aucun n'utilise ce dossier : la convention réelle est **`core/php/`**,
> servi directement par Apache. On s'y conforme.

### ⚠️ Levée d'ambiguïté : quel « core » ?

Le mot `core` désigne deux choses différentes dans Jeedom, et les confondre reviendrait à
modifier le cœur du système :

| Chemin | Nature | Droit d'écriture |
|---|---|---|
| `/var/www/html/core/` | **cœur de Jeedom** | ❌ **INTERDIT** — jamais touché |
| `/var/www/html/plugins/glowscreen32/core/php/` | dossier `core` **du plugin** | ✅ notre code |

`core/php/api.php` désigne bien le **second** : un fichier interne au plugin, dans son propre
dossier. Tout le code de GlowScreen32 vit sous `plugins/glowscreen32/`.

**Règle absolue : aucune modification du cœur de Jeedom, ni de `jeeApi.php`, ni d'un autre
plugin.** Tout doit tenir dans le plugin, de sorte qu'il reste installable, désinstallable et
déployable par `deploy-plugin.sh` sans effet de bord. Le plugin ne fait que *consulter* le
cœur, via son API publique (`jeedom::apiAccess()`, `cmd::byId()`, `scenario::byId()`,
`log::add()`, `config::byKey()`).

### Authentification

Clé API du plugin, transmise **en en-tête HTTP** :

```
X-GLOWSCREEN32-APIKEY: <clé>
```

Repli toléré par `?apikey=<clé>` en query string, mais **l'en-tête est la méthode normale** :
une query string finit en clair dans les journaux d'Apache, ce que les plugins existants
évitent délibérément.

Côté plugin, la validation se fait par `jeedom::apiAccess($apikey, 'glowscreen32')`, et la clé
est obtenue par `jeedom::getApiKey('glowscreen32')`.

⚠️ **Ne jamais déposer de `.htaccess` dans `core/php/`** : cela rendrait l'endpoint inaccessible.

Toutes les réponses sont en `application/json`.

---

# Négociation de schéma — ajouté en v2.0

C'est la pièce qui rend tout le reste possible sans casser le parc. **À lire avant le reste.**

La carte annonce ce qu'elle sait lire, en en-tête :

```
X-GLOWSCREEN32-SCHEMA: 2
```

| En-tête reçu par le plugin | Réponse |
|---|---|
| absent, vide, ou `1` | **le schéma 1 à l'identique, octet pour octet** |
| `2` | le schéma 2 — **sans aucune tuile `view`** (v3.0) |
| `3` ou plus | le schéma 3 |

Le plugin ne répond **jamais** dans un schéma supérieur à celui annoncé. Une carte de schéma 1
qui interroge un plugin v2.0 reçoit exactement ce qu'elle recevait d'un plugin v1.4.

> **Pourquoi c'était indispensable.** Le plugin se déploie en une seconde et d'un seul coup
> (`deploy-plugin.sh`), alors que le parc se met à jour écran par écran, en OTA, sur plusieurs
> jours. Sans négociation, publier le plugin v2 rendait **tous** les écrans inutilisables au
> même instant — et un écran qui n'affiche plus rien ne peut plus recevoir l'OTA qui le
> réparerait. Il aurait fallu décrocher chaque carte et la rebrancher en USB. C'est
> exactement la panne que le retour arrière automatique de la v1.4 cherchait à éviter.
>
> Corollaire d'ordre de déploiement : **le plugin part toujours en premier**, le firmware
> ensuite. L'inverse donnerait des cartes réclamant un schéma que le serveur ne sait pas servir.

La réponse `layout` du schéma 2 porte `"schema": 2`. Son absence signifie schéma 1.

Le schéma 2 est **additif** : aucun champ de la v1.4 ne change de sens, ne change de type, ni
ne disparaît. C'est précisément ce que la v1.3 n'avait pas pu faire avec `id`, et ce qui lui a
coûté une rupture.

### ⚠️ La v2.1 ne change PAS le numéro de schéma

En v2.1, le schéma est resté **2**. Ce n'était pas un oubli (la v2.2 a suivi la même règle ; seul le
schéma 3 de la v3.0 en a eu besoin, voir plus bas).

Le numéro de schéma ne décrit que la **forme des réponses**. La v2.1 n'ajoute qu'un paramètre
de **requête** optionnel (`rssi`), et un paramètre qu'un serveur ne connaît pas, il l'ignore :
une carte v2.1 qui l'envoie à un plugin v2.0 est servie exactement comme avant. Il n'y a donc
rien à négocier, et rien qui puisse casser dans un sens ou dans l'autre.

Le précédent est celui des v1.1 à v1.4, qui ont toutes servi le schéma 1. **Incrémenter le
schéma pour une évolution qui n'en a pas besoin coûte un tour de négociation à tout le parc**
et rend l'en-tête moins lisible, puisqu'il ne dirait plus ce qu'il prétend dire.

---

## `action=layout` — récupérer les boutons

```
GET ...core/php/api.php?action=layout&device=246f28123456
X-GLOWSCREEN32-APIKEY: <clé>
X-GLOWSCREEN32-SCHEMA: 2
```

### Schéma 1 — inchangé depuis la v1.2

```json
{
  "ok": true,
  "device": "246f28123456",
  "name": "Salon",
  "version": 3,
  "poll": 30,
  "buttons": [
    { "id": 0, "label": "Lampe",  "color": "#2D7FF9", "icon": "bulb",  "mode": "toggle", "state": 1 },
    { "id": 1, "label": "Portail","color": "#9B59B6", "icon": "gate",  "mode": "action", "state": null }
  ]
}
```

Maximum **6 boutons** (grille 3×2 sur 320×240). Un écran configuré avec des pages est **aplati
à ses 6 premiers boutons** quand il est servi en schéma 1 : une vieille carte reste utilisable,
en mode dégradé, plutôt que de recevoir une structure qu'elle ne comprend pas.

### Schéma 2

```json
{
  "ok": true,
  "schema": 2,
  "device": "246f28123456",
  "name": "Salon",
  "version": 12,
  "poll": 30,
  "grid": { "cols": 3, "rows": 3 },
  "ui": { "swipe": true, "clock": true },
  "info": "21,4 °C",
  "time": 1758537600,
  "tzoffset": 7200,
  "pages": [
    {
      "id": 0,
      "title": "Salon",
      "buttons": [
        { "id": 0, "slot": 0, "label": "Lampe",    "color": "#9B59B6", "icon": "bulb",   "mode": "toggle", "state": 1 },
        { "id": 1, "slot": 1, "label": "Portail",  "color": "#E67E22", "icon": "gate",   "mode": "action", "state": null },
        { "id": 2, "slot": 8, "label": "Chambres", "color": "#34495E", "icon": "folder", "mode": "nav",    "page": 1 }
      ]
    },
    {
      "id": 1,
      "title": "Chambres",
      "parent": 0,
      "buttons": [
        { "id": 3, "slot": 0, "label": "Plafond", "color": "#2D7FF9", "icon": "ceiling", "mode": "toggle", "state": 0 }
      ]
    }
  ],
  "states": [1, null, null, 0]
}
```

#### Champs de niveau écran

| Champ | Schéma | Sens |
|---|---|---|
| `version` | 1 | incrémenté à chaque changement de configuration — la carte ne redessine que s'il change |
| `poll` | 1 | intervalle de rafraîchissement conseillé, en secondes. Borné par le serveur à **[5, 3600]** ; hors bornes, il renvoie **30**, pas la borne |
| `schema` | 2 | le schéma servi : `2` ou `3`. Absent = schéma 1 |
| `grid` | 2 | `cols` ∈ {3, 4}, `rows` ∈ {2, 3}. **Défaut `3×3`.** Absent = `3×2` |
| `ui.swipe` | 2 | autorise le changement de page au balayage. **Défaut `false`** |
| `ui.clock` | 2 | affiche l'heure dans le bandeau. **Défaut `false`** |
| `info` | 2 | chaîne **déjà formatée** à afficher au bandeau, ≤ 16 caractères, ou `null` |
| `time` | 2 | epoch Unix **UTC** au moment de la réponse |
| `tzoffset` | 2 | décalage local en secondes, **DST comprise**. Heure locale = `time + tzoffset` |
| `pages` | 2 | 1 à **4** pages |
| `states` | 2 | états de **tous** les boutons, indexés par `id` global — voir `ping` |

#### Champs de page

| Champ | Sens |
|---|---|
| `id` | rang de la page, 0 à 3. La page **0 est la page d'accueil** |
| `title` | titre affiché au bandeau, ≤ 24 caractères |
| `parent` | page vers laquelle remonte le bouton « retour ». Absent sur la page 0 |

#### Champs de bouton

| Champ | Schéma | Sens |
|---|---|---|
| `id` | 1 | **identifiant opaque** — voir ci-dessous |
| `label` | 1 | libellé, ≤ 24 caractères, UTF-8 |
| `color` | 1 | couleur en hexadécimal `#RRGGBB` |
| `mode` | 1 | `"action"`, `"toggle"`, `"nav"` (schéma 2+) ou `"view"` (schéma 3) |
| `state` | 1 | `0`, `1`, ou `null` si la commande est sans état |
| `icon` | 1 | nom d'icône, **vocabulaire fermé** — voir ci-dessous |
| `slot` | 2 | case occupée dans la grille, `0` à `cols*rows-1`. Absent = rang dans le tableau |
| `page` | 2 | **uniquement si `mode` vaut `"nav"`** : page à ouvrir |

### ⚠️ `id` est opaque — changé en v1.3, étendu en v2.0

Jusqu'en v1.2, `id` était l'**identifiant de commande Jeedom**, et la carte le renvoyait à
`press` pour dire quoi exécuter. C'était une erreur de conception : elle donnait à la carte
l'autorité sur *quelle commande* déclencher, alors qu'elle n'a aucun moyen de savoir laquelle
est pertinente.

Depuis la v1.3, `id` est un **rang**. La carte le traite comme une valeur opaque : elle ne
l'affiche jamais, ne trie pas dessus, n'en déduit rien, et le renvoie tel quel à `press`.
**C'est le plugin qui décide quelle commande Jeedom exécuter**, à partir de la configuration du
bouton et de son état courant.

**En v2.0, `id` reste un rang, mais un rang *global* à l'écran entier** — numérotation continue
`0 … N-1` dans l'ordre d'aplatissement des pages, pas un rang par page.

> **Pourquoi global et pas `(page, rang)`.** Un rang par page recréerait exactement
> l'ambiguïté que la v1.3 a éliminée : le bouton 0 de la page 1 et le bouton 0 de la page 0
> porteraient le même `id`, et le plugin devrait se fier à un second champ envoyé par la carte
> pour les distinguer. Ce serait redonner à la carte une parcelle d'autorité sur la résolution
> de la commande. Avec un rang global, `press` ne change pas d'un iota : `id` désigne un et un
> seul bouton, et `page`/`slot` ne servent **qu'à la mise en page**.

Rappel hérité de la v1.3 : **le rang `0` est un bouton parfaitement légitime.** Jusqu'en v1.2,
`id=0` était un identifiant de commande illégal ; tout test de la forme `if (!$id)` est un bug.

### Les trois modes

| `mode` | Comportement | Configuration côté Jeedom |
|---|---|---|
| `"action"` | déclenche toujours la même commande | une commande action (ou un scénario) |
| `"toggle"` | déclenche **« Allumer » ou « Éteindre » selon l'état courant** | une commande « allumer », une « éteindre », une commande d'état |
| `"nav"` | **ouvre la page `page`** — schéma 2 | aucune commande |

> **Pourquoi le mode `toggle`.** Un équipement comme un Shelly expose des commandes distinctes
> `Allumer` (#1560), `Éteindre` (#1561) et `Basculer` (#1562). Lier un bouton à une seule
> d'entre elles donne un bouton qui n'allume **que** (le défaut constaté en v1.2). Le mode
> `toggle` lit l'état, puis choisit la commande inverse.
>
> Quand l'équipement fournit une commande « basculer », le plugin **peut** l'utiliser
> directement ; mais choisir explicitement d'après l'état reste préférable, car tous les
> équipements n'en proposent pas, et une commande « basculer » désynchronisée inverse l'état
> que l'utilisateur voit à l'écran.

Un bouton `toggle` sans commande d'état configurée est refusé à la sauvegarde : sans état, il
n'y a rien pour décider du sens.

### ⚠️ Un bouton `nav` n'émet jamais de `press`

La navigation est **entièrement locale à la carte**. Elle est donc instantanée, et elle
**fonctionne hors réseau** : un écran coupé de Jeedom continue de naviguer dans ses pages et
d'afficher ses boutons depuis le cache NVS.

Conséquence pour le plugin : il n'a aucune commande à résoudre pour un bouton `nav`, et son
`state` vaut toujours `null`. Un `press` reçu sur l'`id` d'un bouton `nav` signale un firmware
en désaccord avec la configuration : le plugin répond `unknown_button` et le journalise.

### Vocabulaire d'icônes — fermé

`icon` n'est **pas** une chaîne libre. Le firmware embarque un jeu fini de glyphes ; un nom
inconnu vaut `none` et la tuile se rabat sur son seul libellé.

```
none
bulb  lamp  ceiling  strip  plug  power
gate  garage  door  window  shutter  blind  lock
heat  cool  fan  thermo  water  valve
tv  music  speaker  camera  alarm  shield
scene  movie  night  sun  moon  coffee
folder  home  grid  car  mower  vacuum  bell  clock
```

> **Pourquoi fermer le vocabulaire maintenant.** En v1.4, `icon` était parsé, stocké sur 12
> octets par bouton, et **dessiné nulle part** — de la donnée morte dans le blob NVS. Le jour
> où on le dessine, il faut que Jeedom ne puisse proposer que des noms que la carte sait
> rendre, sinon la liste dérive silencieusement et l'utilisateur configure des icônes qui ne
> s'affichent jamais. Côté Jeedom, le champ devient une **liste déroulante**, pas un champ
> texte. Côté firmware, le nom est converti en identifiant numérique **au parsing**, ce qui
> ramène le coût de 12 octets à 1 par bouton.

#### ⚠️ En schéma 1, `icon` reste un passe-plat brut

Le plugin renvoie en schéma 1 la chaîne **exactement telle qu'elle est stockée** : pas de
normalisation, pas de passage en minuscules, pas de repli sur `none` ni sur la chaîne vide.

> **Pourquoi.** En v1.4, `icon` était du texte libre que le firmware parsait sans jamais le
> dessiner. Fermer le vocabulaire est une règle du **schéma 2** ; l'appliquer au schéma 1
> reviendrait à modifier une réponse figée, et la promesse « octet pour octet » deviendrait
> fausse pour tout écran portant une icône hors liste. Le parc en comptait un — un bouton
> réglé sur `fire`, du temps où le champ était libre.
>
> C'est exactement à cela que sert la négociation : **deux schémas, deux comportements**,
> plutôt qu'une règle unique assortie d'exceptions qu'il faudra se rappeler.

#### Alias — schéma 2 et interface Jeedom uniquement

Un nom hors vocabulaire dont l'intention est claire est **résolu**, pas perdu. La table vit
côté plugin ; le firmware, lui, ne connaît que le vocabulaire fermé et n'a aucun alias à
embarquer.

| Alias | Résolu en |
|---|---|
| `fire`, `flame`, `chauffage` | `heat` |
| `light`, `lumiere` | `bulb` |
| `temp`, `temperature` | `thermo` |
| `volet` | `shutter` |
| `store` | `blind` |
| `porte` | `door` |
| `portail` | `gate` |
| `prise` | `plug` |
| `clim` | `cool` |
| `ventilateur` | `fan` |
| `musique` | `music` |
| `alarme` | `alarm` |
| `serrure` | `lock` |

La casse est ignorée à la résolution. Un nom qui ne correspond à rien, **même après alias**,
vaut `none` — et le plugin le **journalise avec le nom de l'écran et le libellé du bouton**,
pour que l'utilisateur sache lequel corriger. Une icône perdue en silence, c'est une tuile
qui s'appauvrit sans que personne ne sache pourquoi.

### Le bandeau — `info`, `clock`, et l'état API

`info` est une chaîne **déjà formatée par le plugin** : c'est lui qui connaît la commande
choisie, son unité et son arrondi. La carte ne fait que l'afficher. Elle n'interprète rien,
elle ne calcule rien, elle ne connaît même pas la notion de température.

> **Pourquoi une chaîne et pas un nombre + une unité.** Même principe que `id` : le plugin
> décide, la carte affiche. Une chaîne laisse l'utilisateur mettre au bandeau une température,
> une humidité, une puissance instantanée ou un niveau de cuve sans qu'une seule ligne du
> firmware ait à changer — et sans qu'un OTA soit nécessaire pour ajouter une unité.

L'heure est dérivée de `time + tzoffset`, recalés à chaque `ping`, et égrenés localement entre
deux pings. **Pas de NTP** : le serveur est déjà la référence de temps, et une carte sans accès
Internet doit continuer à donner l'heure.

#### ⚠️ Une valeur périmée vaut `null`, jamais la dernière connue

Le plugin renvoie `info: null` dès que la valeur de la commande choisie est plus vieille qu'un
seuil réglable par écran (défaut **60 minutes**, `0` = jamais). La carte affiche alors l'heure
seule.

> **Pourquoi.** Constaté sur l'installation de référence : la commande de température était
> câblée au bandeau mais son plugin ne la collectait pas — elle est restée vide jusqu'à ce
> qu'on lance son cron à la main. Sans péremption, le premier relevé obtenu serait resté figé
> à l'écran indéfiniment. Un bandeau vide est honnête ; un bandeau qui affiche la température
> d'hier comme si c'était celle de maintenant ne l'est pas, et c'est pire qu'inutile sur un
> panneau qu'on consulte d'un coup d'œil en passant.

⚠️ **Le plugin lit `collectDate`, pas `valueDate`.** Jeedom ne met `valueDate` à jour que
quand la valeur *change* ; `collectDate` l'est à chaque *collecte*. Une température stable à
18 °C depuis deux heures a une `valueDate` vieille de deux heures alors qu'elle est
parfaitement fraîche — se fier à `valueDate` masquerait des valeurs valides, soit exactement
le défaut symétrique de celui qu'on corrige.

⚠️ **La péremption ne fait PAS bouger `version`.** `version` ne suit que la *configuration* ;
une valeur qui périme est un changement d'*état*, et il se propage par le champ `info` du
`ping`, exactement comme `states`. Faire l'inverse déclencherait un rechargement complet de la
mise en page sur tout le parc à chaque péremption.

Règle d'affichage, **imposée par le contrat** :

| Situation | Bandeau, à droite |
|---|---|
| tout va bien | `info`, puis l'heure si `ui.clock` |
| Wi-Fi perdu, ou API en erreur | **l'anomalie, à la place de `info` et de l'heure** |

> **Pourquoi « API OK » disparaît.** Un voyant qui affiche « OK » 99,9 % du temps n'informe
> personne : il occupe la seule zone de l'écran qui pourrait porter une information utile, et
> l'œil cesse de le lire — donc il ne remarque pas non plus le jour où il passe au rouge. Un
> bandeau qui n'affiche **que** les anomalies se fait remarquer quand il en affiche une.
> L'heure et la température, elles, servent tous les jours.
>
> La qualité Wi-Fi suit la même règle : le niveau ne s'affiche qu'en dessous de **−75 dBm**.
> Ce n'est pas un seuil de panne mais une **alerte précoce** — les relevés de la v1.4 situent
> la perte d'un `press` vers **−88 dBm** et l'échec de l'OTA à **−92/−93 dBm**. Prévenir à
> −75 dBm laisse le temps de déplacer un répéteur avant que l'écran ne devienne inutilisable.
>
> ⚠️ Cet avertissement doit s'afficher **même quand tout le reste va bien**. Une liaison
> faible mais encore fonctionnelle est précisément le cas qu'il doit couvrir ; le
> subordonner à un état « dégradé » le rendrait muet au moment utile.

### Bornes du schéma 2

Elles sont **normatives** : les deux côtés les appliquent. Le plugin **refuse à
l'enregistrement** une configuration qui les dépasse ; ce qui arrive malgré tout hors bornes
(configuration antérieure, autre client) est **tronqué ET journalisé** — jamais absorbé en
silence. Les bornes de longueur se comptent en **caractères**, pas en octets : un firmware doit
réserver de quoi stocker 24 caractères accentués (2 octets chacun en UTF-8).

| Grandeur | Maximum | Raison |
|---|---|---|
| pages | **4** | au-delà, on cherche un bouton au lieu de l'appuyer |
| boutons par page | **12** | `cols` ≤ 4 × `rows` ≤ 3 |
| boutons par écran | **32** | `struct Layout` en NVS, et le plafond JSON ci-dessous |
| taille de la réponse | **8 192 octets** | `JEEDOM_JSON_MAX` côté firmware |
| `label` | 24 caractères | |
| `title` | 24 caractères | |
| `info` | 16 caractères | |

⚠️ **Un dépassement doit être journalisé, jamais absorbé en silence.** Le firmware v1.4
tronquait le tableau de boutons sans un mot : un écran affichait calmement 6 boutons sur 8 et
personne ne pouvait le savoir. En v2.0, la troncature est une **erreur journalisée** des deux
côtés.

⚠️ **Le plafond de taille se contrôle sur les octets réellement lus, pas sur `Content-Length`.**
Un serveur qui répond en `Transfer-Encoding: chunked` ne renvoie pas de `Content-Length` :
`http.getSize()` vaut alors `-1`, et un test de la forme `if (getSize() > MAX)` laisse passer
une réponse de taille **non bornée** — le garde-fou ne garde rien, exactement dans le cas où
il servirait. C'était le défaut de la v1.4.

## `action=press` — déclencher un bouton

```
GET ...core/php/api.php?action=press&device=246f28123456&id=0
X-GLOWSCREEN32-APIKEY: <clé>
X-GLOWSCREEN32-SCHEMA: 3        ← le schéma de la mise en page d'où vient l'id (v3.0)
```

### ⚠️ Un bouton qui ne se résout plus GARDE SON RANG — v3.0

Une commande supprimée ou recréée dans Jeedom (réinclusion, équipement refait) ne doit **jamais**
faire disparaître son bouton de l'aplatissement : tous les `id` suivants reculeraient d'un rang
sans que `version` change, et un appui fait avant le prochain `layout` déclencherait **le bouton
d'à côté**. Le plugin sert donc le bouton à sa place, inerte (`state: null`), journalise le défaut
une fois, et répond `unknown_button` à son `press`. *(Relevé en revue de code de la v3.0.)*

`id` est le **rang du bouton** reçu dans `layout`, pas un identifiant de commande Jeedom.
**Inchangé en v2.0** : le rang est simplement global à l'écran au lieu d'être borné à 5.

```json
{ "ok": true, "id": 0, "state": 1, "pending": true }
```

| Champ | Sens |
|---|---|
| `state` | état **attendu** après exécution — voir l'avertissement ci-dessous |
| `pending` | `true` si l'état n'est pas encore confirmé par l'équipement |

### ⚠️ `state` n'est pas une confirmation

Sur du matériel réel, l'état remonte **après** un aller-retour avec l'équipement. Au moment où
`press` répond, le relais vient de basculer mais Jeedom n'a pas encore reçu le nouvel état :
la valeur renvoyée est donc l'état *attendu*, pas l'état *constaté*.

Le firmware s'en sert uniquement pour un **retour visuel optimiste** immédiat. La source de
vérité reste le tableau `states` du `ping` suivant. Si celui-ci contredit l'état optimiste, le
firmware adopte la valeur du `ping` sans autre forme de procès.

En mode `toggle`, `state` vaut l'inverse de l'état lu avant l'exécution.

## `action=ping` — vérifier la liaison

```
GET ...core/php/api.php?action=ping&device=246f28123456&rssi=-64
X-GLOWSCREEN32-APIKEY: <clé>
X-GLOWSCREEN32-SCHEMA: 2
```

Schéma 1 :

```json
{ "ok": true, "version": 3, "time": 1758537600, "states": [1, 0, 1, null, 0, 0] }
```

Schéma 2 — deux champs de plus, `states` de longueur `N` :

```json
{ "ok": true, "version": 12, "time": 1758537600, "tzoffset": 7200,
  "info": "21,4 °C", "states": [1, null, null, 0] }
```

C'est l'appel de rafraîchissement périodique : il détecte un changement de `version` sans
transférer toute la mise en page.

### `rssi` — ajouté en v2.1, optionnel

La carte **peut** joindre à son `ping` le niveau de réception Wi-Fi qu'elle mesure, en dBm,
sous la forme `&rssi=-64`. Valeurs acceptées par le serveur : **−120 à 0**. Tout ce qui sort
de cet intervalle, ou n'est pas un nombre, est **ignoré sans erreur** — le ping reste un ping,
et une carte ne doit jamais voir sa liaison refusée parce qu'un diagnostic est mal formé.

Le paramètre n'est porté que par `ping` : c'est le seul appel périodique, donc le seul qui
donne une mesure suivie dans le temps sans rien coûter de plus.

> **Pourquoi la carte le remonte.** La qualité Wi-Fi est la première cause de panne constatée
> sur ce projet — un `press` perdu vers −88 dBm, un OTA qui meurt à 2 % vers −92/−93 dBm,
> alors que les `ping`, eux, passent encore. Le diagnostic est donc **trompeur par nature** :
> tout a l'air de marcher.
>
> Le firmware affiche déjà le niveau au bandeau sous −75 dBm, mais il faut **se tenir devant
> l'écran** pour le lire — sur un panneau mural, au fond d'un couloir, c'est précisément ce
> qu'on ne fait pas. Remonté à Jeedom, le niveau devient une commande d'information
> ordinaire : lisible sur le dashboard, historisable, et utilisable dans un scénario pour
> prévenir **avant** qu'un OTA ne soit tenté sur une liaison qui ne le supportera pas.

Le serveur en fait ce qu'il veut ; le contrat n'exige rien en retour, et **aucun champ de
réponse ne change**. Une carte qui l'envoie à un plugin v2.0 est servie exactement comme
avant, et un plugin v2.1 sert normalement une carte qui ne l'envoie pas.

### Cadence du `ping` — précisée en v2.1

`poll` reste la cadence de référence, et la carte s'y tient **écran allumé**.

Écran atténué — personne ne le regarde —, la carte est autorisée à espacer ses `ping` jusqu'à
**2 × `poll`**, et **jamais au-delà**. Elle doit en revanche émettre un `ping` **immédiat au
réveil**, avant même le premier appui : c'est ce qui garantit que les pastilles affichées à
quelqu'un qui s'approche sont fraîches.

⚠️ **Le serveur doit tolérer cette absence.** Un plugin qui déclarerait un écran hors ligne
au bout de trois `poll` verrait tout le parc « disparaître » chaque nuit, alors que chaque
carte fonctionne parfaitement. Le seuil de détection se calcule donc sur l'intervalle
**maximal** autorisé, soit `3 × 2 × poll`.

> **Pourquoi plafonner à 2 et pas davantage.** Le facteur d'espacement et le seuil de
> détection hors ligne sont **le même réglage vu des deux côtés**. Les laisser diverger
> donnerait la panne la plus coûteuse qui soit : un parc qui se signale en panne sans l'être,
> c'est-à-dire une alerte à laquelle on cesse de croire. Le plafond est donc **normatif**, et
> la constante existe des deux côtés sous le même nom.

### `states` — ajouté en v1.2, réindexé en v2.0

`states` donne l'état courant de chaque bouton (`0`, `1`, ou `null` si la commande est sans
état). En schéma 1, il est **dans le même ordre que `buttons`**. En schéma 2, il est **indexé
par l'`id` global**, ce qui revient au même puisque l'`id` global *est* le rang d'aplatissement
— mais l'énoncer par l'`id` le rend insensible à un futur changement d'ordre des pages.

> **Pourquoi `states` existe.** En v1.1, `version` ne changeait qu'aux modifications de
> *configuration*. Un allumage déclenché depuis l'application Jeedom, un interrupteur mural ou
> un scénario laissait donc la pastille de l'écran périmée jusqu'au prochain rechargement
> complet. Renvoyer les états dans `ping` corrige la cause plutôt que le symptôme, pour un coût
> de quelques octets.

Le firmware met à jour les pastilles à partir de `states` sans redessiner la grille — et, en
schéma 2, **sans redessiner les boutons des pages qui ne sont pas affichées**. Si la taille du
tableau ne correspond pas au nombre de boutons connus, il ignore `states` et force un `layout`
complet. **Cette règle est conservée telle quelle** : c'est elle qui a rattrapé la v1.2.

Les boutons `nav` occupent une case de `states`, toujours à `null`.

## Erreurs

Code HTTP non-200 et corps uniforme :

```json
{ "ok": false, "error": "unknown_device" }
```

| `error` | HTTP | Cause |
|---|---|---|
| `bad_apikey` | 401 | clé absente ou invalide |
| `unknown_device` | 404 | aucun eqLogic pour cette MAC, **ou eqLogic désactivé** |
| `unknown_button` | 404 | **aucun bouton de ce rang** sur cet écran, ou bouton de mode `nav` |
| `bad_request` | 400 | paramètre manquant, MAC malformée, ou action inconnue |
| `firmware_unavailable` | 404 | `update: true` annoncé mais le fichier a disparu ; ou jeton `fwfile` inconnu, expiré, ou OTA refermé depuis son émission |

> `unknown_button` disait « id de commande inconnu » jusqu'à la v1.4 incluse : un reste de la
> sémantique v1.2, alors que le serveur entendait déjà « aucun bouton de ce rang ». Corrigé ici.

⚠️ Le plugin rabat **toute exception PHP sur `bad_request` 400**. Un défaut serveur est donc
indiscernable d'un paramètre invalide côté carte. À garder en tête en dépannage : le journal
Jeedom tranche, pas le code HTTP.

## Règles côté firmware

- Timeouts **par action**, pas uniformes : `layout` 5 s × 3, `press` **2,5 s × 1**, `ping` 5 s × 1
  (**`wait` + 5 s** en attente longue, v2.2), `firmware` 5 s × 1, puis backoff.
  > `press` était écrit « 3 s × 2 » alors que le firmware fait 2,5 s × 1 depuis la v2.0 : un
  > second essai figeait l'écran 6 s pour un gain nul, le `ping` suivant rattrapant l'état.
  > Corrigé ici en v2.2 — le contrat suit le code, qui avait raison.
- Ne jamais bloquer l'UI sur le réseau : un appui affiche un retour visuel immédiat, la
  confirmation arrive ensuite. **Depuis la v2.2, aucun appel HTTP ne s'exécute dans la tâche
  qui lit le tactile et dessine** — voir « Réseau hors de la tâche d'affichage ».
- Perte de Wi-Fi ou d'API → bandeau d'état en haut de l'écran, les boutons restent affichés et
  **restent tactiles**.
- La mise en page reçue est mise en cache en NVS pour survivre à un redémarrage hors ligne.
- La navigation entre pages ne dépend **jamais** du réseau.
- Le `ping` suit `poll` écran allumé, **au plus `2 × poll`** écran atténué, et part
  **immédiatement au réveil** (v2.1).
- Le `ping` porte le niveau Wi-Fi mesuré en paramètre `rssi`, quand la liaison est établie
  (v2.1), et les diagnostics `up`, `rst`, `heap`, `blk`, `ip`, `ssid` (v2.2).
- Identifiants Wi-Fi, hôte Jeedom, clé API et calibration tactile vivent en **NVS** ; les
  valeurs compilées ne sont que des **valeurs d'usine** (v2.2).
- `url` de `action=firmware` est **opaque** : la carte la télécharge telle quelle (v2.2).

## `action=firmware` — mise à jour par le réseau (v1.4)

```
GET ...core/php/api.php?action=firmware&device=246f28123456&fw=1.3.0
X-GLOWSCREEN32-APIKEY: <clé>
```

`fw` = version actuellement exécutée par la carte. Elle sert à deux choses : décider s'il y a
une mise à jour, et **permettre à Jeedom d'afficher quelle version tourne sur chaque écran**.

### ⚠️ `fw` et `version` doivent avoir la même origine

C'est la seule chose qui rende la comparaison valable, et ce n'était pas écrit — la v1.2 du
plugin est tombée dans le piège.

La source unique de vérité est **`GLOWSCREEN_FW_VERSION`** dans `src/fw_version.h`. Elle est
utilisée de deux façons :

- la carte l'envoie telle quelle dans `fw` ;
- elle est **gravée dans le binaire** sous la forme `GLOWSCREEN32-FW:<version>`, terminée par
  un octet nul, et le plugin extrait `version` de ce marqueur.

**Ne pas lire la version dans `esp_app_desc_t`.** Avec `framework = arduino`, ce descripteur
provient des bibliothèques Arduino précompilées et annonce `esp-idf: v4.4.7 …` /
`arduino-lib-builder` — une valeur identique pour tous nos binaires. La panne qui en découle
est silencieuse et durable : le dépôt réussit, la valeur ressemble à une version, les verrous
fonctionnent, et le plugin compare indéfiniment une chaîne à elle-même en journalisant « rien
de plus récent à proposer ». Aucun code d'erreur ne la signale.

Le plugin **refuse** un binaire sans marqueur. Corollaire utile : l'outil de calibration
(`[env:calib]`) n'en porte pas, il ne peut donc pas partir en OTA par erreur.

⚠️ Chercher le **préfixe complet** `GLOWSCREEN32-FW:`. Le binaire contient aussi
`X-GLOWSCREEN32-APIKEY` : une recherche sur `GLOWSCREEN32` seul peut extraire `-APIKEY` comme
numéro de version.

### Pas de mise à jour (cas normal, et cas « bloqué »)

```json
{ "ok": true, "update": false }
```

Renvoyé aussi bien quand la carte est à jour que quand **l'OTA est désactivé**. La carte ne
fait pas la différence, et c'est voulu : elle n'a aucun moyen de passer outre.

### Mise à jour disponible

```json
{
  "ok": true,
  "update": true,
  "version": "1.4.0",
  "url": "http://192.168.1.10/plugins/glowscreen32/core/php/api.php?action=fwfile&token=629fa993…",
  "sha256": "3f9a…",
  "size": 1002288
}
```

La carte télécharge, vérifie **l'empreinte SHA-256 avant de basculer**, écrit dans la partition
inactive, puis redémarre dessus.

### ⚠️ `url` est opaque, et ne pointe plus vers `data/` — corrigé en v2.2

Jusqu'en v2.1, `url` désignait le fichier `data/firmware/glowscreen32-<version>.bin`, servi par
Apache. **Cette URL renvoyait 403 à coup sûr** : le `.htaccess` RACINE de Jeedom (cœur,
intouchable) porte un `RedirectMatch 403` sur tout fichier rangé sous un dossier `data/` dont
l'extension n'est pas dans sa liste, et `.bin` n'y est pas. Un `RedirectMatch` ne se lève pas
depuis le `.htaccess` d'un plugin. Toute mise à jour échouait au premier octet, sans autre
symptôme côté serveur qu'une ligne `AH01797` dans le journal d'Apache.

Depuis la v2.2, `url` désigne `api.php?action=fwfile&token=<jeton>` :

| Règle | Raison |
|---|---|
| **pas de clé API** exigée, le **jeton** autorise | les firmwares en service téléchargent `url` sans aucun en-tête ; et la clé n'a rien à faire dans une URL |
| jeton de 32 caractères hexadécimaux, aléatoire, émis **uniquement** quand les deux verrous sont ouverts | publier n'est pas autoriser |
| valable **15 minutes**, pour le **seul binaire déposé à l'émission** | un dépôt survenu entre-temps invalide le jeton au lieu de servir un fichier dont la carte n'a pas l'empreinte |
| réponse `application/octet-stream` avec **`Content-Length` obligatoire** | la carte compare la taille HTTP à `size` avant d'écrire un octet |
| jeton inconnu ou expiré → `404 firmware_unavailable` en JSON | |

La carte ne déduit **rien** de `url` : ni le nom du fichier, ni la version, ni l'hôte. Elle la
télécharge telle quelle. C'est ce qui a permis de corriger le serveur **sans toucher au
firmware** : une carte en 2.0.7 suit la nouvelle URL sans le savoir.

Bénéfice induit : le binaire, qui contient les secrets compilés (Wi-Fi, clé API), n'est plus
téléchargeable par n'importe qui sur le réseau local.

## Autorisation de mise à jour — double verrou

L'OTA est la seule fonction où Jeedom peut **casser durablement** un écran à distance. Deux
verrous indépendants, tous deux côté serveur :

| Verrou | Portée | Défaut |
|---|---|---|
| `ota_enabled` | **global au plugin** | **désactivé** |
| `ota_allowed` | **par écran** | désactivé |

Les deux doivent être actifs pour qu'un écran reçoive `update: true`. Conséquences voulues :

- **Un firmware défectueux ne se propage pas.** On coupe l'interrupteur global, les écrans
  restants continuent d'interroger et reçoivent `update: false`.
- **Déploiement progressif.** On autorise **un seul écran témoin**, on vérifie qu'il revient
  en ligne et fonctionne, puis on ouvre aux autres. C'est le vrai intérêt du verrou par
  écran : sans lui, une mauvaise version part partout en même temps.
- La décision est **entièrement côté serveur**. Une carte ne peut ni forcer, ni contourner.

## Retour arrière automatique

Sans cela, l'OTA transforme un bug logiciel en panne matérielle — il faut décrocher l'écran et
le rebrancher en USB.

Après un démarrage sur un firmware fraîchement installé, la carte doit **se déclarer saine**
(`esp_ota_mark_app_valid_cancel_rollback()`) seulement une fois qu'elle a vérifié :

1. le Wi-Fi est connecté,
2. l'API a répondu au moins une fois,
3. l'écran s'est initialisé.

Si elle n'y parvient pas dans le délai imparti, elle redémarre et l'ESP32 **rebascule seul sur
la partition précédente**. Un firmware qui ne sait plus joindre le réseau se défait donc tout
seul. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` doit être actif.

⚠️ **Avec `framework = arduino`, le projet DOIT définir `extern "C" bool verifyRollbackLater()
{ return true; }`.** Sinon le cœur Arduino (`initArduino()`, avant même `setup()`) déclare
l'image saine tout seul, et le filet n'existe pas. C'était le cas jusqu'en 2.1.0 : aucun
symptôme, l'OTA réussissait, le retour arrière n'avait jamais été possible. Corrigé en 2.1.1.

⚠️ Corollaire : **ne jamais marquer le firmware valide dès `setup()`**. Ce serait désactiver le
filet de sécurité tout en croyant l'avoir.

### Validation renforcée — v2.2

Depuis la 2.2.0 (version corrigée avant toute publication), les trois conditions ci-dessus ne
suffisent plus. La carte ne se déclare saine qu'après avoir **aussi** :

4. reçu au moins un `ping` réussi **de sa tâche ping** — en attente longue si `features.wait`
   est annoncé ;
5. obtenu une réponse valide à `action=firmware` (contrôle seul : rien n'est installé avant la
   validation) ;
6. tourné **120 s** sans interruption.

Délai porté à **10 minutes**.

> **Pourquoi.** Dans la première écriture de la 2.2.0, un seul `layout` réussi arrivait ~10 s après le démarrage, **avant**
> que la tâche ping, l'attente longue et les commandes à distance n'aient tourné une seule
> fois. Un plantage de ce code neuf serait survenu sur un firmware déjà déclaré sain : plus de
> retour arrière, redémarrage en boucle, et le contrôle OTA qui aurait pu le réparer jamais
> atteint. Un firmware n'est sain que s'il a exercé **le chemin qui permet de le remplacer**.

Une version qui a été rejetée par retour arrière est **mémorisée en NVS** : la carte ne la
réinstalle pas, même si le serveur la lui propose encore. Sans cela, elle la retéléchargerait
toutes les six minutes, usant la flash pour un résultat connu d'avance.

⚠️ `unknown_device` compte comme « l'API a répondu », `bad_apikey` non. **Écran non déclaré
(ENROLL)** : les preuves 4 et 5 sont impossibles — ni `ping` ni contrôle firmware n'aboutissent
pour une MAC inconnue. Elles sont alors **remplacées** par : au moins deux réponses
`unknown_device` espacées d'une minute ou plus, plus les 120 s. Sans cette exception (relevée en
revue de la v3.0), un écran supprimé de Jeedom ou pas encore déclaré reviendrait en arrière
après chaque OTA, et la version, saine, serait mémorisée comme rejetée.

Est aussi une preuve 5 valable toute réponse **JSON bien formée** de `action=firmware`, y compris
`firmware_unavailable` : elle prouve le chemin, pas la disponibilité d'un binaire.

Tant que le firmware n'est pas validé, la carte **refuse** la commande `reboot` (comme pendant un
OTA) : un redémarrage avant validation déclencherait le retour arrière et ferait rejeter une
version saine.

---

# Évolutions v2.2 — réactivité et exploitation du parc

Le **schéma reste 2**. La v2.2 n'ajoute que des paramètres de requête optionnels et des champs
de réponse que seule une carte qui les a demandés reçoit — même raisonnement qu'en v2.1.

> **Pourquoi le schéma ne bouge pas, une fois encore.** Une carte v2.0/v2.1 n'envoie ni `wait`
> ni `rev` : le plugin la sert exactement comme avant, aucun `ping` n'est retenu. Les champs
> `features`, `rev` et `cmd` ajoutés aux réponses du schéma 2 sont ignorés par un parseur qui ne
> les cherche pas (ArduinoJson ne lit que les clés qu'on lui demande). Une carte v2.2 face à un
> plugin v2.1 ne trouve pas `features` et se comporte en v2.1. **Rien à négocier.**

## Capacités annoncées — `features`

Le plugin v2.2 ajoute aux réponses `layout` **et** `ping` du schéma 2 :

```json
"features": { "wait": 25, "cmd": true }
```

| Champ | Sens |
|---|---|
| `wait` | durée maximale, en secondes, pendant laquelle le serveur accepte de **retenir** un `ping`. Absent ou `0` : pas d'attente longue |
| `cmd` | le serveur peut placer une commande à distance dans la réponse `ping` |

⚠️ **La carte n'utilise une capacité que si elle est annoncée.** C'est la condition qui rend
l'attente longue sûre : une carte qui enverrait `wait` à un plugin v2.1 recevrait une réponse
**immédiate**, relancerait aussitôt, et martèlerait Jeedom en boucle serrée. Sans `features.wait`,
la carte reste à la cadence `poll` de la v2.1.

## `ping` en attente longue — `wait` et `rev`

Avant la v2.2, un changement d'état fait ailleurs (application, interrupteur mural, scénario)
mettait jusqu'à `poll` secondes — 30 s, 60 s écran atténué — à atteindre l'écran. La v2.2 le
ramène à **une seconde environ**, sans rien ouvrir côté carte.

```
GET ...api.php?action=ping&device=246f28123456&rssi=-64&wait=25&rev=a41f09c2
```

| Paramètre | Sens |
|---|---|
| `wait` | secondes, de 1 à `features.wait`. La carte envoie **`min(features.wait, poll)`** |
| `rev` | la dernière valeur de `rev` reçue. Absente : réponse immédiate |

Toute réponse `ping` **et** `layout` du schéma 2 d'un plugin v2.2 porte :

```json
"rev": "a41f09c2"
```

`rev` est **opaque** (≤ 16 caractères) : la carte ne fait que la renvoyer. Elle change dès que
change l'un de ces éléments : un élément de `states`, `info` (y compris sa péremption),
`version`, ou la file des commandes à distance. Elle **ne** change **pas** avec `time`.

Comportement du serveur :

1. `wait` absent, nul ou invalide, ou `rev` absente ou différente de la valeur courante →
   **réponse immédiate**, exactement comme un `ping` v2.1.
2. Sinon le serveur **retient** la requête jusqu'à ce que `rev` change, ou que `wait` secondes
   s'écoulent — puis répond un `ping` complet, calculé **au moment de la réponse**.
3. Le contact (`lastcontact`, `online`, `rssi`, diagnostics) est noté **à l'arrivée** de la
   requête, pas à sa sortie.
4. Une nouvelle requête retenue pour le même écran **libère** la précédente (qui répond
   aussitôt) : une carte redémarrée ne laisse pas de requête fantôme occuper Apache.

Comportement de la carte :

- elle relance le `ping` **dès la réponse reçue** (au moins 200 ms d'écart), sans attendre `poll` ;
- après une erreur : backoff de 2 s, doublé, plafonné à 60 s ;
- timeout HTTP de **`wait` + 5 s** ;
- l'attente longue tourne **dans une tâche réseau dédiée** : un `press` ne doit jamais
  attendre qu'un `ping` retenu se termine (voir plus bas).

> **Pourquoi l'écran tire au lieu d'être poussé.** La carte est sur 192.168.20.x, Jeedom sur
> 192.168.1.x. Pousser supposerait que Jeedom connaisse l'adresse de chaque écran et traverse
> le routage dans l'autre sens, et que chaque carte fasse tourner un serveur HTTP — de la
> mémoire et une surface d'attaque en plus sur une carte sans PSRAM. L'attente longue garde le
> modèle « la carte va chercher » de l'OTA, fonctionne à l'identique avec 1 ou 10 écrans, et
> ne demande **aucune** ouverture réseau.

> **Coût côté serveur.** Une requête retenue occupe un processus Apache par écran. À l'échelle
> d'un parc domestique (≤ 20 écrans), c'est négligeable devant les 150 processus par défaut.
> Le serveur vérifie `rev` au plus toutes les 500 ms **sans interroger la base** : un
> *listener* Jeedom sur les commandes d'état et la commande du bandeau fait évoluer `rev` en
> cache. Recalculer tous les états toutes les 500 ms coûterait des centaines de requêtes SQL
> par seconde et par écran.

### Cadence et détection hors ligne

Avec l'attente longue, l'intervalle entre deux requêtes d'un écran vaut au plus
`wait` + quelques secondes, et `wait ≤ poll`. Le plafond `2 × poll` de la v2.1 est donc
respecté par construction, et le seuil hors ligne `3 × 2 × poll` **ne change pas**.

Après un `press` marqué `pending`, la carte n'a plus besoin d'avancer son `ping` à 2 s quand
l'attente longue est active : le `ping` retenu revient de lui-même dès que l'état change. Sans
attente longue, la règle v1.3 (`ping` avancé) s'applique toujours.

### ⚠️ Le serveur n'enregistre JAMAIS l'eqLogic depuis l'API

**Règle normative pour le plugin, ajoutée en v2.2.** Aucun chemin de `api.php` ne doit appeler
`save()` sur l'eqLogic. Contact, firmware annoncé, niveau Wi-Fi, diagnostics, péremption du
bandeau : tout va dans des **commandes info** ou dans le **cache**.

> **Pourquoi.** Jusqu'en v2.1, chaque contact réenregistrait l'eqLogic entier, tel que chargé
> au début de la requête. Si l'utilisateur enregistrait l'écran pendant ce temps, sa
> modification était écrasée sans message. Avec une requête courte, la fenêtre était étroite ;
> avec une requête **retenue 25 s**, elle devient la règle. L'attente longue rendait ce défaut
> latent systématique : il fallait le supprimer avant de l'introduire.

## Commandes à distance — `cmd`

Quand `features.cmd` est annoncé, une réponse `ping` **à une requête qui porte `rev`** peut
porter **une** commande :

```json
"cmd": { "seq": 17, "do": "message", "text": "On sonne au portail", "duration": 30 }
```

| Champ | Sens |
|---|---|
| `seq` | entier croissant **par écran**. La carte ignore une commande dont le `seq` est celui de la dernière exécutée |
| `do` | le verbe, **vocabulaire fermé** ci-dessous. Inconnu → ignoré et journalisé par la carte |

⚠️ **Une commande n'est livrée qu'à une requête portant `rev`**, donc à une carte v2.2. Une carte
2.0/2.1 en schéma 2 ignorerait le champ `cmd` ; la livraison étant « au plus une fois », la
commande serait retirée de la file et perdue sans effet. Elle reste donc en file et y expire.
Corollaire firmware : la carte envoie `rev` **sur chaque `ping`** dès qu'elle en connaît une,
attente longue active ou non. *(Relevé à l'implémentation du plugin v2.2.)*

### Vocabulaire

| `do` | Arguments | Effet sur la carte |
|---|---|---|
| `reboot` | — | redémarre ~1 s après avoir traité la réponse |
| `identify` | `duration` 1–120 s (défaut 10) | réveille l'écran, fait clignoter rétroéclairage et LED RVB, affiche nom + MAC + IP |
| `message` | `text` ≤ 64 caractères UTF-8, `duration` 1–600 s (défaut 30) | réveille l'écran et affiche le message par-dessus la grille ; un toucher le ferme **sans actionner de bouton** |
| `page` | `page` 0–3 | réveille l'écran et ouvre la page |
| `calibrate` | — | lance la calibration tactile à l'écran ; **abandon après 60 s sans toucher**, anciennes valeurs conservées |
| `ota` | — | contrôle de firmware **immédiat** — toujours soumis aux deux verrous du serveur |
| `wifi` | `ssid` ≤ 32, `pass` ≤ 64 | essaie les nouveaux identifiants ; **sans connexion sous 60 s, revient aux précédents**. Succès → écrits en NVS |

Un argument hors bornes est ramené dans ses bornes par le serveur **avant** d'entrer dans la
file ; la carte borne à nouveau à la lecture.

### Livraison : au plus une fois

Le serveur **retire** une commande de la file au moment où il la place dans une réponse. Une
réponse perdue en route perd la commande ; l'utilisateur la relance.

> **Pourquoi « au plus une fois » et pas « au moins une fois ».** Avec un acquittement, un
> `reboot` dont l'acquittement n'arrive pas est rejoué au redémarrage — qui le rejoue à son
> tour : une boucle de redémarrages déclenchée à distance. Perdre une commande de temps en
> temps est un désagrément ; une carte qui redémarre en boucle sur un mur est une panne.

Règles de file, côté serveur :

- **8 commandes** au plus par écran ; au-delà, la plus ancienne est abandonnée et journalisée ;
- **durée de vie 10 minutes** : une commande destinée à un écran éteint expire. Un `reboot`
  livré trois jours plus tard, au retour d'un écran, serait une surprise ;
- mettre une commande en file **change `rev`** : un `ping` retenu la livre dans la seconde ;
- une seule commande par réponse ; s'il en reste, `rev` reste « changée » et le `ping` suivant
  revient aussitôt.

⚠️ `wifi` fait transiter un mot de passe en clair sur le réseau local, comme la clé API depuis
la v1.1. Accepté pour un parc domestique, à reprendre le jour où le transport passera en TLS.
La commande n'est **pas** exposée comme commande Jeedom utilisable par un scénario : elle ne
part que depuis la page de l'équipement.

Côté Jeedom, chaque verbe sauf `wifi` devient une **commande action** de l'écran : utilisable
depuis le dashboard **et depuis un scénario** (« on sonne → afficher la caméra sur l'écran de
l'entrée »).

## Diagnostics — paramètres optionnels du `ping`

```
...&action=ping&rssi=-64&up=86400&rst=poweron&heap=142336&blk=86004&ip=192.168.20.42&ssid=MonReseau
```

| Paramètre | Sens | Bornes |
|---|---|---|
| `up` | secondes depuis le démarrage | 0 – 4 294 967 295 |
| `rst` | cause du dernier redémarrage | `poweron`, `sw`, `panic`, `wdt`, `brownout`, `ext`, `other` |
| `heap` | tas libre, octets | 0 – 400 000 |
| `blk` | plus gros bloc allouable, octets | 0 – 400 000 |
| `ip` | adresse IPv4 de la carte | forme pointée |
| `ssid` | réseau Wi-Fi auquel la carte est associée | ≤ 32 caractères |

Même règle que `rssi` : **tout ce qui est absent, mal formé ou hors bornes est ignoré sans
erreur**. Le serveur les range dans des commandes info ; `rssi`, `heap` et `blk` sont
**historisées**.

> **Pourquoi ces six-là.** `rst=panic` ou `wdt` signale un firmware qui plante sans que personne
> ne le voie — l'écran redémarre en deux secondes et paraît fonctionner. `heap` et surtout `blk`
> mesurent la fragmentation qui, sans PSRAM, finit par faire échouer l'allocation du sprite ou
> d'une réponse JSON. `ip` et `ssid` répondent à la question « où est cet écran sur le réseau ? »
> sans débrancher personne.

## Réseau hors de la tâche d'affichage — règle firmware

Jusqu'en v2.1, chaque appel HTTP s'exécutait dans la tâche qui lit le tactile : un `ping` lent
figeait l'écran jusqu'à 10 s, un `layout` en échec jusqu'à 30 s, et **tout appui fait pendant
ce temps était perdu** — y compris le premier appui après le réveil, puisque le `ping` de réveil
partait dans le même tour de boucle.

Depuis la v2.2 :

- la tâche d'affichage **n'émet aucun appel HTTP** : elle dépose des demandes dans une file et
  applique les résultats ;
- l'attente longue tourne dans **sa propre tâche** : un `press` n'attend jamais un `ping` retenu ;
- l'OTA suspend l'attente longue pendant le téléchargement ;
- seule la tâche d'affichage touche à l'écran (SPI) et au modèle `Layout` affiché.
- ⚠️ **aucune attente active sur le cœur 0.** `Stream::timedRead()` — utilisé par
  `readBytes()`, `readString*()`, ArduinoJson et `WebServer` — attend chaque octet en boucle
  sans jamais bloquer. Sur le cœur 0, dont la tâche IDLE est surveillée par le chien de garde
  **avec panique au bout de 5 s**, une réponse retardée par le Wi-Fi fait redémarrer la carte.
  Toute lecture réseau du cœur 0 attend en cédant la main (`vTaskDelay`). *Constaté en 2.2.1 :
  `rst=wdt` pendant la période d'essai, sur un code identique à la 2.2.0 — intermittent selon la
  radio. Jusqu'en 2.1, la même boucle tournait sur le cœur 1, non surveillé : elle figeait
  l'écran au lieu de le faire redémarrer.*

## Configuration locale de la carte — règle firmware

Identifiants Wi-Fi, hôte Jeedom, clé API et calibration tactile sont stockés en **NVS**. Les
valeurs de `secrets.h` et `touch_calib.h` ne servent qu'au **premier démarrage**, ou après un
effacement.

> **Pourquoi.** Changer le mot de passe du Wi-Fi imposait de décrocher et reflasher chaque
> écran en USB — l'OTA ne peut rien pour une carte qui a perdu le réseau. Et des bornes
> tactiles compilées, extrapolées d'une seule dalle, contredisaient « un seul binaire pour tout
> le parc ».

Trois chemins pour les modifier, sans câble :

1. **commande `wifi` / `calibrate`** depuis Jeedom, tant que l'écran est joignable ;
2. **menu local** : appui maintenu 5 s sur le bandeau → informations, calibration, portail
   Wi-Fi, redémarrage ;
3. **portail de secours** : sans Wi-Fi depuis 5 minutes, la carte ouvre en plus un point
   d'accès `GlowScreen-<6 derniers caractères de la MAC>`, protégé par un mot de passe
   **affiché seulement sur l'écran** — il faut être devant pour s'y connecter. La carte continue
   d'essayer son réseau habituel pendant ce temps, et referme le point d'accès dès qu'elle l'a
   retrouvé.

La carte s'annonce sous le nom d'hôte `glowscreen-<6 derniers caractères de la MAC>` et choisit,
parmi les points d'accès du même SSID, **le plus fort** — et non le premier trouvé.

---

# Schéma 3 — tuiles « valeur » et écran en lecture seule (v3.0)

Jusqu'en v2.2, une tuile **agit** : `action`, `toggle`, ou `nav` pour naviguer. La v3.0 ajoute
une tuile qui **montre** : l'état d'une porte, de l'alarme, du portail, du chauffage, une
température. Elle ne déclenche rien.

```
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ door  Porte  │  │ shield Alarme│  │ thermo Salon │
│   Ouverte    │  │    Armée     │  │   21,4 °C    │
│   (warn)     │  │   (alert)    │  │  (neutral)   │
└──────────────┘  └──────────────┘  └──────────────┘
```

C'est un **nouveau schéma**, et cette fois c'est indispensable : une carte de schéma 2 ne sait
pas dessiner une tuile `view`. Elle la prendrait pour un bouton, l'afficherait comme tel, et
enverrait un `press` à chaque toucher. Exactement le cas pour lequel la négociation existe.

## Négociation

```
X-GLOWSCREEN32-SCHEMA: 3
```

| Carte | Reçoit |
|---|---|
| schéma 1 | inchangé, octet pour octet |
| schéma 2 | le schéma 2 **dont toutes les tuiles `view` ont été retirées**, et la numérotation `id` recalculée sur ce qui reste |
| schéma 3 | tout, avec `"schema": 3` |

Le schéma 3 est **additif** sur le schéma 2 : tout ce qui y figure garde son sens.

> **Pourquoi retirer plutôt que dégrader.** Le plugin sait déjà servir une mise en page
> aplatie différemment selon le schéma — c'est ce qu'il fait pour le schéma 1 — et `press`
> résout le rang **dans l'aplatissement du schéma négocié**. Retirer les tuiles `view` pour une
> carte de schéma 2 ne coûte donc rien de nouveau, et lui évite d'afficher des boutons qui ne
> font rien. Une page qui ne contiendrait que des tuiles `view` apparaît vide sur une carte de
> schéma 2 : c'est le mode dégradé assumé, le temps de l'OTA.

⚠️ Corollaire, déjà vrai pour le schéma 1 et qui devient critique : **les `id` d'un même
bouton diffèrent d'un schéma à l'autre.** Un `id` n'a de sens que dans le schéma où il a été
reçu.

Conséquence normative pour `press` : la carte envoie avec `press` **l'en-tête de schéma de la
mise en page d'où vient l'`id`**, et non le schéma maximal qu'elle sait lire. Une carte de
schéma 3 qui s'est vu servir un `layout` en schéma 2 (plugin plus ancien) annonce donc `2` sur
ses `press`. Le plugin résout le rang dans l'aplatissement du schéma annoncé.

Une tuile reçue avec `mode: "view"` reste une tuile `view` quel que soit le schéma de la
réponse : la carte n'émet jamais de `press` pour elle.

## La tuile `view`

```json
{ "id": 4, "slot": 4, "label": "Porte", "color": "#34495E", "icon": "door",
  "mode": "view", "state": null, "value": "Ouverte", "tone": "warn" }
```

| Champ | Sens |
|---|---|
| `mode` | `"view"` |
| `value` | texte **déjà mis en forme par le plugin**, ≤ 16 caractères UTF-8 — ou `null` si la valeur est inconnue ou périmée : la carte affiche alors `—` |
| `tone` | `"neutral"`, `"ok"`, `"warn"` ou `"alert"`. Absent ou inconnu = `"neutral"` |
| `state` | toujours `null` |

Même principe que `info` au bandeau : **le plugin décide, la carte affiche.** Elle ne sait pas
ce qu'est une porte ni une alarme, ne connaît aucune unité et ne compare rien à un seuil.
Ajouter un type de capteur ne demande jamais d'OTA.

`tone` est un **sens**, pas une couleur : la palette (`ok` vert, `warn` orange, `alert` rouge)
est fixée dans le firmware, identique sur tout le parc. `neutral` garde la couleur `color` de
la tuile. Une porte ouverte a la même couleur sur tous les écrans de la maison, quelle que soit
l'humeur du jour de celui qui a configuré la tuile.

Côté carte :

- `value` s'affiche en grand ; la police est la plus grande des trois qui fasse tenir le texte
  dans la tuile, avec points de suspension en dernier recours ;
- **un toucher sur une tuile `view` ne fait rien** et n'émet **jamais** de `press` (pas même de
  retour visuel d'appui) ;
- `value` et `tone` sont **volatils** : ils ne vont **pas** dans le cache NVS. Au démarrage
  depuis le cache, une tuile `view` affiche `—` jusqu'au premier `ping`.

Côté plugin, un `press` reçu sur l'`id` d'une tuile `view` → `unknown_button`, journalisé,
comme pour `nav`.

## Valeurs dans le `ping` — `values`

```json
{ "ok": true, "version": 14, "states": [1, null, null, 0, null],
  "values": [null, null, null, null, {"v": "Ouverte", "t": "warn"}], ... }
```

`values` a **la même longueur que `states`**, indexé par l'`id` global : `null` pour toute
tuile qui n'est pas `view`, un objet `{"v": <value>, "t": <tone>}` sinon — `v` pouvant valoir
`null` (valeur inconnue ou périmée, affichée `—`). La règle de `states`
s'applique telle quelle : **longueur différente du nombre de tuiles connues → la carte ignore
`values` et force un `layout`.**

`rev` couvre `values` : une porte qui s'ouvre libère le `ping` retenu dans la seconde. Le
*listener* du plugin surveille les commandes des tuiles `view` comme celles de `states`.

## Mise en forme — côté plugin

Le contrat n'impose que le résultat (`value`, `tone`). La configuration, dans Jeedom, part de
la commande info choisie :

| Type de commande | Réglages | Exemple |
|---|---|---|
| binaire | libellé + ton pour 0 et pour 1, inversion | 1 → « Ouverte » `warn` ; 0 → « Fermée » `ok` |
| numérique | unité, décimales, seuils croissants → ton | `°C`, 1 décimale ; `≥ 25` → `warn`, `≥ 30` → `alert` |
| texte | table « valeur → libellé + ton » ; défaut : la valeur brute, `neutral` | `armed` → « Armée » `alert` |

Le plugin **préremplit** ces réglages d'après le type générique Jeedom de la commande
(`TEMPERATURE`, `LOCK_STATE`, `OPENING`, `ALARM_*`, `HEATING_*`…), **et affiche la valeur
courante mise en forme dans la page de configuration.** Ce second point n'est pas un confort :
le sens d'un `1` varie d'un module à l'autre (porte « ouverte » ou « fermée »), et seul
l'aperçu avec la vraie valeur permet à l'utilisateur de vérifier qu'il n'a pas configuré une
porte qui s'affiche fermée quand elle est ouverte.

### ⚠️ Péremption : pas la règle du bandeau

Le bandeau périme sur `collectDate`. **C'est faux pour un capteur d'ouverture** : une porte
fermée depuis trois jours n'émet rien depuis trois jours, et sa valeur est pourtant juste.
Pour une tuile `view` :

| Cas | `value` vaut `null` si… |
|---|---|
| tous | l'équipement de la commande est **en alerte de communication dans Jeedom** (délai maximal entre deux communications dépassé), ou désactivé |
| numérique | **en plus**, `collectDate` est plus vieille que le seuil de la tuile (défaut **60 min**, `0` = jamais) |
| binaire, texte | pas de seuil par défaut (réglable par tuile si l'équipement émet périodiquement) |

> **Pourquoi c'est critique ici.** Une tuile `view` est faite pour être crue d'un coup d'œil
> — « la porte est fermée, je peux partir ». Une valeur figée par un capteur muet est pire
> qu'une absence de valeur. Mais une valeur juste effacée à tort apprend à ignorer le `—`.
> Les deux erreurs sont symétriques, d'où une règle par type.

## Écran en lecture seule — `ui.readonly`

```json
"ui": { "swipe": true, "clock": true, "readonly": true }
```

Défaut `false`. À `true`, la carte **n'envoie aucun `press`**, quel que soit le mode des
tuiles ; la navigation entre pages reste possible. Le plugin, de son côté, **refuse** tout
`press` pour cet écran (`403 read_only`), journalisé.

> **Pourquoi des deux côtés.** La carte qui obéit est le confort ; le plugin qui refuse est la
> garantie. Un écran posé dans une entrée ou un garage ne doit pas pouvoir ouvrir le portail,
> même avec un firmware défectueux ou une clé API dérobée.

## Le cache NVS change de format — règle firmware

Le firmware de schéma 3 **change `STORE_MAGIC`**. Un firmware antérieur qui relit ce cache
(après un retour arrière) doit le **rejeter** et attendre son premier `layout`.

> **Pourquoi.** Le cache contient des `id` du schéma 3. Relus par une 2.2.0, qui les enverrait
> à `press` en schéma 2, ils désigneraient **d'autres boutons** : un appui fait dans les
> secondes qui suivent un retour arrière pourrait déclencher le bouton d'à côté — portail
> compris. Un retour arrière doit ramener un écran vide pendant deux secondes, jamais un écran
> qui ment.

## Bornes du schéma 3

Celles du schéma 2, plus :

| Grandeur | Maximum |
|---|---|
| `value` | 16 caractères |
| tuiles `view` | comprises dans les 32 tuiles par écran |
| réponse | toujours 8 192 octets — `values` ajoute ~30 octets par tuile `view` |

## Erreurs ajoutées

| `error` | HTTP | Cause |
|---|---|---|
| `read_only` | 403 | `press` sur un écran en lecture seule |

---

## Historique des corrections de terrain

Chacune vient d'une panne constatée, pas d'une revue de conception. Les réintroduire coûterait
le même temps une seconde fois.

| Version | Correction |
|---|---|
| v1.1 | endpoint en `core/php/`, pas `core/api/` ; clé API en **en-tête**, pas en query string ; jamais de `.htaccess` dans `core/php/` |
| v1.2 | `ping` renvoie `states` — sinon une pastille reste périmée après un allumage hors écran |
| v1.3 | `id` devient un rang **opaque** ; mode `toggle` ; `press` optimiste ; le rang `0` est légitime |
| v1.4 | OTA sous **double verrou** + retour arrière ; version lue dans le marqueur `GLOWSCREEN32-FW:`, **jamais** dans `esp_app_desc_t` |
| **v2.0** | **négociation de schéma** — sans elle, publier le plugin rend le parc entier inutilisable au même instant, et l'OTA ne peut plus le rattraper |
| **v2.0** | plafond de taille contrôlé sur les **octets lus**, pas sur `Content-Length` (inopérant en *chunked*) |
| **v2.0** | toute troncature est **journalisée**, jamais silencieuse |
| **v2.0** | `icon` en schéma 1 est un **passe-plat brut** : fermer le vocabulaire est une règle du schéma 2, l'appliquer à une réponse figée la rendrait fausse |
| **v2.0** | `info` **périme** : une valeur plus vieille que le seuil vaut `null`. Constaté — la commande de température du bandeau n'était pas collectée, et serait restée figée à l'écran indéfiniment |
| **v2.1** | `rssi` **optionnel** sur `ping` — le niveau Wi-Fi n'était lisible qu'au bandeau, donc seulement en se tenant devant l'écran, alors que c'est la première cause de panne du projet et que les `ping` passent encore là où les `press` et l'OTA échouent |
| **v2.1** | cadence du `ping` **plafonnée à `2 × poll`** écran atténué, et seuil hors ligne calculé sur ce maximum **des deux côtés** — sinon un parc parfaitement sain se signalerait en panne toutes les nuits |
| **v2.1** | le numéro de **schéma ne bouge pas** : un paramètre de requête optionnel n'a rien à négocier, et incrémenter pour rien coûterait un tour de négociation à tout le parc |
| **v2.2** | `url` du firmware servie par `api.php?action=fwfile&token=…` : le `.htaccess` racine de Jeedom renvoie 403 sur tout `.bin` sous `data/`, **aucune mise à jour ne pouvait aboutir** |
| **v2.2** | attente longue du `ping` (`wait`/`rev`), **seulement si `features.wait` est annoncé** — sans cette condition, une carte v2.2 martèlerait un plugin v2.1 en boucle serrée |
| **v2.2** | l'API n'enregistre **jamais** l'eqLogic : avec un `ping` retenu 25 s, l'écrasement d'une configuration enregistrée pendant ce temps devenait systématique |
| **v2.2** | commandes à distance livrées **au plus une fois** — un `reboot` rejoué faute d'acquittement serait une boucle de redémarrages |
| **v2.2** | `press` documenté à 2,5 s × 1, comme le code depuis la v2.0 |
| **v2.2** | diagnostics (`up`, `rst`, `heap`, `blk`, `ip`, `ssid`) sur `ping` — `rst=wdt` a révélé le défaut du cœur 0, `ip`/`ssid` ont dit où était l'écran |
| **v2.2** | configuration locale en NVS + portail de secours — changer le mot de passe Wi-Fi imposait de décrocher chaque écran |
| **v2.2** | validation OTA **renforcée** (6 preuves, 10 min) et **mémoire des versions rejetées** — la première écriture se validait avant d'avoir exercé le chemin qui permet de la remplacer |
| **v3.0** | **schéma 3** pour les tuiles `view` — une carte de schéma 2 les aurait prises pour des boutons ; elles lui sont **retirées**, pas dégradées |
| **v3.0** | péremption d'une tuile `view` sur l'**alerte de communication** de l'équipement, pas sur `collectDate` — une porte fermée depuis trois jours n'émet rien et dit vrai |
| **v3.0** | `STORE_MAGIC` change : relu après un retour arrière, un cache aux `id` du schéma 3 aurait fait jouer le bouton d'à côté |
| **v3.0** | lecture seule refusée **par le plugin**, pas seulement ignorée par la carte |
| **v2.2 / fw 2.3.0** | aucune attente active sur le **cœur 0** : `Stream::timedRead()` y affamait IDLE0, et une réponse Wi-Fi retardée de 5 s faisait redémarrer la carte (`rst=wdt` constaté en 2.2.1). Le même défaut, sur le cœur 1, était le « gel de l'écran » des versions 2.1 |
| **v3.0** | un bouton dont la commande ne se résout plus **garde son rang** — le retirer décalait tous les `id` suivants : bouton d'à côté jusqu'au prochain `layout` |
| **v3.0** | validation OTA d'un écran **non déclaré** par `unknown_device` — sinon retour arrière et rejet d'une version saine après chaque OTA |
| **v3.0** | bornes de longueur en **caractères** : un firmware dimensionné en octets coupait en silence un libellé accentué de 24 caractères |
| **v3.0** | nombres au format **français** (`21,4 °C`) au bandeau comme sur les tuiles — le bandeau écrivait `21.4` |
