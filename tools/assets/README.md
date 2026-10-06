# Régénérer les polices et les icônes du firmware

Ces quatre scripts produisent `src/ui/fonts/font_ui_*.cpp` et `src/ui/icons.cpp`.
**Ne jamais éditer ces fichiers à la main** : ce sont des tableaux d'octets, une retouche
manuelle y passe inaperçue jusqu'à l'affichage sur la carte.

| Script | Rôle |
|---|---|
| `gen_vlw.py` | rasterise Inter et produit les trois polices au format VLW |
| `check_vlw.py` | rejoue le parsing de `TFT_eSPI::loadMetrics()` et valide le résultat |
| `gen_icons.py` | dessine les 39 pictogrammes en vectoriel et les rastérise en alpha 8 bits |
| `roundtrip.py` | relit les `.cpp` **réellement installés** et revalide tout |

## Dépendances

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install freetype-py pillow
```

## Icônes

39 pictogrammes **en alpha 8 bits** — un octet d'opacité par pixel, mélangé par le firmware
entre la couleur d'avant-plan et le fond de la tuile, exactement comme TFT_eSPI le fait pour
ses polices lissées. En 1 bit ils faisaient « bricolage » à côté des libellés en VLW.

Deux tailles, rastérisées **depuis la même source vectorielle** — la 40 n'est jamais un
agrandissement de la 24 :

| Taille | Octets par icône | Total | Usage |
|---|---|---|---|
| 24×24 | 576 | 22 464 | tuiles des grilles à 3 rangées |
| 40×40 | 1 600 | 62 400 | grandes tuiles des grilles à 2 rangées |

Un seul registre graphique pour les 39 : le **trait** (« outline »), 2 unités d'épaisseur sur
une toile de 24, extrémités et jointures arrondies. Rien n'est plein sauf de petits points
d'accent (bouton de porte, serrure, diodes), trop petits pour être tracés. Mélanger du trait
et de l'aplat est précisément ce qui fait « dépareillé ».

Les formes sont décrites en coordonnées réelles puis rastérisées avec sur-échantillonnage ×8
et moyenne exacte. **Ne jamais flouter un bitmap pour obtenir l'alpha** : les bords bavent.

```bash
# planches de contrôle seules
python gen_icons.py /tmp/out
# planches + écriture de src/ui/icons.{h,cpp}
python gen_icons.py /tmp/out ../../src/ui
```

`icons-dark.png` et `icons-light.png` montrent les deux tailles agrandies sur les deux fonds
(les tuiles allumées sont claires, les icônes y sont teintées en sombre) ; `icons-actual.png`
les montre en **taille réelle**, seule planche qui dise si c'est lisible pour de bon.

## Fonte

**Inter**, sous [SIL Open Font License 1.1](https://openfontlicense.org/) — licence qui
autorise l'embarquement dans un binaire redistribué. Instance variable `wght=500` pour le
16 px (une graisse plus lourde se boucherait à cette taille), `wght=600` pour les 20 et
30 px, axe `opsz` calé sur la taille en pixels.

> ⚠️ **Ne pas substituer une fonte système Apple** (Helvetica, SF). Leur licence ne permet
> pas la redistribution dans un firmware.

## Invariants vérifiés

- `taille du fichier == 24 + 28 × gCount + Σ(largeur × hauteur)`
  Le pied de fichier de Processing est **volontairement omis** : TFT_eSPI ne le lit jamais,
  et l'omettre rend cette égalité vraie, donc vérifiable.
- `maxAscent` n'est **jamais** recalculé depuis les métriques par TFT_eSPI (le code est
  commenté) : l'`ascent` de l'en-tête doit donc porter le vrai maximum du jeu de glyphes,
  sinon les capitales accentuées sont rognées.
- Chaque codepoint demandé est présent ; aucune métrique ne déborde des types C de
  `loadMetrics()`.
- Les deux tables d'icônes ont 39 entrées de 576 et 1600 octets, dans l'ordre exact de
  `enum IconId`, et chacune est vérifiée **anti-crénelée** : un seuillage accidentel ne
  produirait que des 0 et des 255, ce qui est rejeté.
- Aucune forme ne sort de la toile de 24 unités. Le contrôle est fait à la génération
  (`Canvas.overflow()` rastérise sur une toile élargie et exige une couronne vide), pas
  déduit du bitmap : un trait rogné par le bord y serait invisible.

## ⚠️ L'ordre de `IconId` est un format de stockage

Un `IconId` est écrit **tel quel** dans le blob NVS par `layout_store`. Insérer une icône au
milieu de l'énumération ferait relire les anciens caches de travers. On n'ajoute qu'**avant
`ICON_COUNT`**, et de concert avec le vocabulaire du plugin Jeedom
(`docs/api-contract.md`). Toute autre modification impose d'incrémenter `STORE_MAGIC`.
