# État du projet GlowScreen32 — 2026-10-06

**Firmware 2.3.1 (compilé, à publier ; 2.3.0 en service, OTA validé) · contrat d'API v3.0 ·
plugin Jeedom 3.0**

Une fois publiée par OTA et validée, la 2.3.1 sera la version en service. Le plugin part
toujours en premier (contrat, « Négociation de schéma ») ; c'est le cas.

---

## Historique du firmware — ce qui a été PROUVÉ sur la carte

| Version | Apport | Prouvé sur la carte réelle |
|---|---|---|
| 2.1.1 | `verifyRollbackLater()` : le retour arrière existe enfin (absent jusqu'en 2.1.0, sans symptôme) | **retour arrière** effectif après une version volontairement invalide |
| 2.2.0 | réseau hors de l'affichage, attente longue, commandes à distance, diagnostics, NVS, menu, portail, calibration à l'écran ; validation renforcée (6 preuves, 10 min) et mémoire des versions rejetées | en service et validée ; **OTA déclenché à distance en moins d'une seconde** (commande `ota` livrée par un ping retenu) ; second **retour arrière** prouvé |
| 2.2.1 | — | **`rst=wdt` intermittent trouvé et corrigé** : `BoundedStream::readBytes` attendait en boucle active (`Stream::timedRead`) sur le cœur 0, dont IDLE est surveillé avec panique. Constaté pendant sa période d'essai, selon la qualité radio |
| 2.3.0 | schéma 3 : tuiles `view`, `values`, `ui.readonly`, cache v04 | en service, OTA validé ; **page État avec de vraies tuiles** |
| 2.3.1 | corrections de revue complète (ci-dessous) | **pas encore** |

## Firmware 2.3.1 — corrections de revue

Détail et raisons : `docs/firmware.md`, « Corrections 2.3.1 ». En bref :
- échéances `millis()` robustes au-delà de 24,8 jours (la plus grave : écran bloqué en
  LAYOUT après 24,8 jours, avec des appuis sur des `id` périmés) ; plus aucun `press` sur une
  mise en page que la carte sait périmée ;
- tâches `net`/`ping` sur le **cœur 1** à la priorité de l'affichage (plus aucune attente
  active possible sous le chien de garde du cœur 0), HTTP/1.0 ;
- validation : écran non déclaré validable (deux `unknown_device` espacées ≥ 1 min), toute
  réponse JSON de `action=firmware` vaut preuve, `reboot` refusé avant validation ; version
  visée notée seulement après `Update.end()` ;
- portail : la clé n'est jamais envoyée à un hôte nouvellement saisi sans être ressaisie,
  échec précis à l'écran et sur la page, fermeture après 20 min sans client ;
- calibration contrôlée par une cible centrale ; tuiles `view` lisibles en 3×3, ≥ 3:1 ;
- tampons dimensionnés en caractères (24 accentués), cache NVS **v05**.

Empreinte : flash 1 291 737 o (65,7 %), RAM statique 69 024 o (21,1 %) — 2.3.0 :
1 288 085 o / 65 936 o.

### À regarder au premier démarrage de la 2.3.1

1. `[nvs] cache présent mais incompatible (magic 47533204 …), ignoré`, écran vide jusqu'au
   premier `layout` — voulu (cache v05). `[cfg] hôte et clé migrés en un seul enregistrement`.
2. `[pile]` : marges de `net`/`ping` désormais sur le cœur 1 ; l'UI doit rester fluide
   pendant un `layout` ou un OTA.
3. `[ota] firmware declare sain` en 2 à 3 min ; aucun `rst=wdt` dans les diagnostics.
4. Tuiles `view` en 3×3 : valeur en 20 px, lisible ; libellés lisibles en 4 colonnes.
5. Calibration : cible centrale de contrôle après les 4 coins.
6. Portail depuis le menu : changer d'hôte sans clé doit être refusé.

---

## Vérifié en conditions réelles (acquis de la v1.4)

La chaîne complète a été validée : **un appui sur l'écran tactile a réellement allumé une
lampe extérieure de test**, et la relecture côté Jeedom l'a confirmé (commande d'état `#1556` = 1).

```
Écran tactile → HTTP → plugin glowscreen32 → Jeedom → Shelly → lampe
```

| Élément | État |
|---|---|
| Toolchain macOS (PlatformIO, arduino-cli, cores ESP32) | ✅ installée |
| Matériel de la carte (écran, tactile, LED, LDR) | ✅ testé |
| Plugin Jeedom (eqLogic, endpoint, page de config) | ✅ déployé, testé au curl |
| Cycle allumer / **éteindre** (mode `toggle`) | ✅ validé au curl côté serveur |
| `lastcontact` | ✅ corrigé |
| Multi-écrans (MAC unique, `version` par écran) | ✅ vérifié |
| OTA : double verrou, dépôt du `.bin`, suivi du parc | ✅ testé au curl (4 combinaisons) |
| **OTA de bout en bout sur la carte** | ✅ **1.4.0 → 1.4.1 en 8 s**, bascule sur `app1`, `fw` remontée à Jeedom |
| Déploiement progressif | ✅ l'écran au verrou fermé n'a rien reçu |
| Filet de sécurité OTA | ✅ éprouvé : 2 échecs de téléchargement, carte intacte à chaque fois |

## Ce qui reste à faire — par ordre d'urgence

1. **Publier la 2.3.1 sur l'écran témoin**, la laisser se valider (2 à 3 min), parcourir la
   liste « À regarder » ci-dessus, puis ouvrir le verrou OTA aux autres écrans.
2. **Ménage Jeedom** : l'écran fictif (#454) ne correspond à aucune carte, à
   supprimer. L'écran de test (#453) porte la MAC de la carte de référence.
3. **Limites firmware assumées** : `TOUCH_SWAP_XY` reste compilé (une dalle aux axes
   permutés demanderait un binaire) ; clé API et commande `wifi` en clair sur le LAN (HTTP).

L'historique détaillé de la v2.0 (pages, polices, icônes, sprite…) est dans
`docs/firmware.md` ; tout y est désormais éprouvé sur la carte.

---

## Chantiers hors périmètre, assumés

| Sujet | Détail |
|---|---|
| **Qualité Wi-Fi** | Déterminante pour l'OTA : à **−92/−93 dBm** le téléchargement meurt à 2 %, à −64 dBm il passe en 8 s ; un `press` a été perdu à −88 dBm. Les `ping` passent dans tous ces cas, ce qui rend le diagnostic trompeur. **Regarder le RSSI du bandeau avant de soupçonner le code.** Le bandeau l'affiche en clair sous −75 dBm (`UI_RSSI_WEAK_DBM`). C'est un problème d'installation : répéteur, déplacement, antenne. |
| **Clé API en clair sur le LAN** | L'en-tête HTTP (v1.1) la sort des journaux d'Apache, mais le transport reste HTTP. Hors périmètre. |
| **`ping` ne dit pas quel écran il décrit** | Deux cartes partageant une MAC ne seraient pas détectées côté firmware. Le contrat charge le plugin de refuser les doublons à la sauvegarde. |

---

## Historique des décisions de conception

Le contrat d'API a été corrigé à presque chaque version, chaque fois par un constat de terrain
ou par une relecture qui a trouvé un garde-fou inopérant. Les premières corrections
ci-dessous ; la liste complète (jusqu'à la v3.0) est l'« Historique des corrections de
terrain » de `docs/api-contract.md` :

| Version | Correction | Déclencheur |
|---|---|---|
| v1.1 | endpoint `core/php/` (pas `core/api/`), clé en en-tête HTTP | aucun des 14 plugins existants n'utilise `core/api/` ; une clé en query string finit dans les logs Apache |
| v1.2 | `ping` renvoie `states` | `version` ne bougeait qu'à la configuration : un allumage fait ailleurs laissait la pastille périmée |
| v1.3 | `id` = rang du bouton (opaque) ; `mode` `action`/`toggle` ; `press` optimiste | le bouton allumait mais n'éteignait pas ; et la carte ne doit pas avoir autorité sur *quelle* commande exécuter |
| v1.4 | `action=firmware`, double verrou d'autorisation, retour arrière automatique | reflasher des écrans muraux au câble est intenable ; et une mise à jour défectueuse ne doit jamais partir sur tout le parc |
| v1.4 (suite) | la version vient d'un marqueur `GLOWSCREEN32-FW:` gravé dans le binaire | `esp_app_desc_t` vient des bibliothèques Arduino précompilées et annonçait la même valeur pour tous nos binaires : l'OTA ne se serait **jamais** déclenché, silencieusement |
| **v2.0** | **négociation de schéma** par en-tête `X-GLOWSCREEN32-SCHEMA` | le plugin se déploie en une seconde, le parc se met à jour écran par écran sur plusieurs jours. Sans elle, publier le plugin v2 rendait **tous** les écrans inutilisables au même instant — et un écran qui n'affiche plus rien ne peut plus recevoir l'OTA qui le réparerait |
| **v2.0** | plafond de taille sur les **octets lus**, pas sur `Content-Length` | `http.getSize()` vaut **−1** en `Transfer-Encoding: chunked` : `-1 > 8192` est faux, le garde-fou v1.4 ne gardait rien **précisément dans le cas où il servait** |
| **v2.0** | toute troncature **journalisée** | la v1.4 tronquait en silence : un écran affichait calmement 6 boutons sur 8 |

Deux corrections v2.0 supplémentaires n'ont pas touché le contrat mais méritent la même
mémoire :

- **`Layout` est passé de 316 o à 1 608 o**, pour 8 ko de pile. Toute instance doit être
  statique ou globale ; en locale, le chemin `save() → load()` sous HTTPClient débordait.
- **`icon` était de la donnée morte** : parsé, stocké sur 12 octets par bouton, dessiné
  nulle part. Il coûte désormais 1 octet et sert.

Ces corrections sont la vraie valeur du travail : elles viennent de tests réels et de
relectures ciblées, pas de la conception initiale.

---

## Sécurité

Les boutons pilotent du matériel réel. **Le portail d'entrée a été ouvert pour de vrai**
pendant un test automatisé. Depuis, les essais sont restreints à la lampe extérieure de test
(eqLogic #84 — état 1556, on 1560, off 1561, toggle 1562). Garder
cette restriction sauf accord explicite de l'utilisateur.

⚠️ Ce point devient plus sensible en v2.0 : la carte peut désormais porter **32 boutons sur
4 pages**. Une page de test remplie de commandes réelles est une page de test dangereuse.
