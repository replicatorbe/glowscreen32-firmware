#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Relit les .cpp RÉELLEMENT installés dans src/ et revalide leur contenu.

C'est le seul contrôle qui porte sur ce que le compilateur verra : les autres
scripts valident ce qu'ils viennent de produire en mémoire, celui-ci repart du
fichier sur disque.

  Polices  — les trois tableaux VLW repassent par check_vlw (qui rejoue le
             parsing de TFT_eSPI::loadMetrics) et _LEN doit coller.
  Icônes   — deux tables d'ALPHA 8 BITS de 39 entrées, 576 et 1600 octets,
             dans l'ordre exact de enum IconId, et dont on vérifie qu'elles
             sont bien anti-crénelées et qu'aucune ne déborde de sa toile.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_vlw import Vlw

SRC = os.environ.get('GS32_SRC', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src', 'ui'))

A24, A40 = 24 * 24, 40 * 40
NB = 39


def bytes_from_c(text, header):
    i = text.index(header)
    j = text.index('};', i)
    return bytes(int(h, 16) for h in re.findall(r'0x([0-9A-Fa-f]{2})', text[i:j]))


def split_blocks(text, decl):
    """[(nom, octets)] pour une table `const uint8_t NOM[39][...] = { { // x ..."""
    i = text.index(decl)
    body = text[i:text.index('\n};', i)]
    return [(n, bytes(int(h, 16) for h in re.findall(r'0x([0-9A-Fa-f]{2})', blk)))
            for n, blk in re.findall(r'\{ // (\w+)\n((?:.*\n)*?)  \},', body)]


def check_fonts():
    ok = True
    for sym in ('FONT_UI_16', 'FONT_UI_20', 'FONT_UI_30'):
        p = os.path.join(SRC, 'fonts', sym.lower() + '.cpp')
        txt = open(p).read()
        blob = bytes_from_c(txt, 'const uint8_t %s[] PROGMEM = {' % sym)
        declared = int(re.search(r'const uint32_t %s_LEN = (\d+);' % sym, txt).group(1))
        f = Vlw(blob)
        good = f.check() and declared == len(blob)
        ok &= good
        print('%s : %d octets, _LEN=%d %s, %d glyphes, %s'
              % (sym, len(blob), declared,
                 'OK' if declared == len(blob) else 'INCOHÉRENT',
                 f.gCount, 'cohérent' if good else 'ERREURS'))
        for e in f.errors:
            print('   [ERREUR]', e)
        for w in f.warn:
            print('   [avert]', w)
    return ok


def check_icons():
    ok = True
    hdr = open(os.path.join(SRC, 'icons.h')).read()
    cpp = open(os.path.join(SRC, 'icons.cpp')).read()

    e0 = hdr.index('enum IconId')
    enum = re.findall(r'ICON_([A-Z0-9]+)', hdr[e0:hdr.index('ICON_COUNT', e0)])
    expected = [e.lower() for e in enum][1:]          # sans ICON_NONE

    # l'en-tête doit annoncer les bonnes dimensions
    for name, want in (('ICON_W', 24), ('ICON_H', 24),
                       ('ICON_BIG_W', 40), ('ICON_BIG_H', 40)):
        m = re.search(r'%s\s*=\s*(\d+)' % name, hdr)
        if not m or int(m.group(1)) != want:
            print('   [ERREUR] %s != %d dans icons.h' % (name, want))
            ok = False

    for label, decl, side, nbytes in (
            ('ICON_A24', 'const uint8_t ICON_A24[39][ICON_A24_BYTES] PROGMEM = {', 24, A24),
            ('ICON_A40', 'const uint8_t ICON_A40[39][ICON_A40_BYTES] PROGMEM = {', 40, A40)):
        try:
            blocks = split_blocks(cpp, decl)
        except ValueError:
            print('   [ERREUR] table %s introuvable' % label)
            ok = False
            continue
        print('\n%s : %d blocs' % (label, len(blocks)))
        if len(blocks) != NB:
            print('   [ERREUR] %d blocs au lieu de %d' % (len(blocks), NB))
            ok = False
        if [n for n, _ in blocks] != expected:
            print('   [ERREUR] ordre des icônes != ordre de enum IconId')
            ok = False
        else:
            print('   ordre conforme à enum IconId')
        total = 0
        for n, b in blocks:
            total += len(b)
            if len(b) != nbytes:
                print('   [ERREUR] %s : %d octets au lieu de %d' % (n, len(b), nbytes))
                ok = False
                continue
            # 1. l'icône existe
            if max(b) == 0:
                print('   [ERREUR] %s : entièrement transparente' % n)
                ok = False
                continue
            # 2. elle atteint l'opacité pleine — sinon le tracé serait délavé
            if max(b) < 250:
                print('   [ERREUR] %s : alpha max = %d, jamais opaque' % (n, max(b)))
                ok = False
            # 3. elle est bien ANTI-CRÉNELÉE : c'est tout l'objet du format.
            #    Un seuillage binaire ne produirait que des 0 et des 255.
            partial = sum(1 for v in b if 0 < v < 255)
            if partial < side:
                print('   [ERREUR] %s : %d pixels partiels seulement, bord crénelé'
                      % (n, partial))
                ok = False
            # 4. rien n'est rogné par le bord. Un trait qui affleure le bord est
            #    normal (une icône a le droit d'occuper toute sa toile) ; un bord
            #    SATURÉ veut dire que le trait est centré dessus, donc coupé en
            #    deux. Le contrôle exact, lui, est fait à la génération par
            #    Canvas.overflow(), qui rastérise sur une toile élargie.
            edge = (list(b[:side]) + list(b[-side:])
                    + [b[y * side] for y in range(side)]
                    + [b[y * side + side - 1] for y in range(side)])
            if max(edge) >= 250:
                print('   [ERREUR] %s : bord saturé (alpha %d), trait rogné'
                      % (n, max(edge)))
                ok = False
            # 5. couverture plausible pour du trait : ni vide, ni aplat
            cover = sum(b) / (255.0 * nbytes)
            if not (0.04 <= cover <= 0.55):
                print('   [avert] %s : couverture %.1f %% (attendu 4-55 %% pour du trait)'
                      % (n, cover * 100))
        print('   %d octets' % total)
    return ok


if __name__ == '__main__':
    ok = check_fonts()
    ok &= check_icons()
    print('\n%s' % ('TOUT EST COHÉRENT' if ok else 'DES PROBLÈMES SUBSISTENT'))
    sys.exit(0 if ok else 1)
