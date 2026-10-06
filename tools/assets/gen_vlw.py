#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Génère des polices VLW (format « Create Font » de Processing) lisibles par
TFT_eSPI 2.5.43 (Extensions/Smooth_font.cpp), puis les émet en tableaux C
PROGMEM.

Structure du fichier, relue dans TFT_eSPI::loadFont()/loadMetrics() :

  En-tête : 6 x uint32 GROS-BOUTISTE (24 octets)
      gCount, version(11), fontSize(points), mboxY(0, déprécié), ascent, descent
  Puis gCount x 7 x int32 gros-boutiste (28 octets par glyphe) :
      unicode, gHeight, gWidth, gxAdvance, gdY, gdX, padding(0)
  Puis, dans le même ordre, les bitmaps : gWidth*gHeight octets d'alpha 8 bits.

  Le pied de fichier de Processing (nom de fonte + drapeau anti-crénelage)
  n'est JAMAIS lu par TFT_eSPI : on l'omet, ce qui économise de la flash et
  rend vraie l'égalité  taille_fichier == 24 + 28*gCount + somme(w*h).

Contraintes de types côté TFT_eSPI (loadMetrics) :
  gHeight/gWidth/gxAdvance -> uint8_t, gdX -> int8_t, gdY -> int16_t,
  unicode -> uint16_t.
"""
import struct, sys, os
import freetype

# ---------------------------------------------------------------- jeu de caractères
ASCII = [chr(c) for c in range(0x20, 0x7F)]
FRENCH = list("ÀÂÄÇÈÉÊËÎÏÔÖÙÛÜŸàâäçèéêëîïôöùûüÿŒœ«»°·–—’…€²³")
CHARSET = ASCII + FRENCH
assert len(CHARSET) == len(set(CHARSET))

VLW_VERSION = 11


def render_font(ttf, px, wght, opsz=None):
    """Rend tous les glyphes et renvoie la liste des métriques + bitmaps."""
    face = freetype.Face(ttf)
    axes = [a.tag for a in face.get_variation_info().axes] if face.is_scalable else []
    try:
        info = face.get_variation_info()
        coords = []
        for a in info.axes:
            tag = a.tag.decode() if isinstance(a.tag, bytes) else a.tag
            if tag == 'wght':
                coords.append(max(a.minimum, min(a.maximum, wght)))
            elif tag == 'opsz':
                v = opsz if opsz is not None else px
                coords.append(max(a.minimum, min(a.maximum, v)))
            else:
                coords.append(a.default)
        face.set_var_design_coords(coords)
    except Exception:
        pass  # fonte statique

    face.set_pixel_sizes(0, px)
    flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL

    glyphs = []
    for ch in CHARSET:
        cp = ord(ch)
        gi = face.get_char_index(ch)
        if gi == 0 and ch != ' ':
            raise RuntimeError("glyphe absent de la fonte : %r U+%04X" % (ch, cp))
        face.load_char(ch, flags)
        g = face.glyph
        bm = g.bitmap
        w, h, pitch = bm.width, bm.rows, bm.pitch
        buf = bm.buffer
        data = bytearray(w * h)
        for y in range(h):
            row = y * pitch
            data[y * w:(y + 1) * w] = bytes(buf[row:row + w])
        if cp == 0x20:                      # l'espace n'a jamais de bitmap
            w = h = 0
            data = bytearray()
        adv = (g.advance.x + 32) >> 6
        glyphs.append(dict(cp=cp, ch=ch, w=w, h=h,
                           adv=adv, dY=g.bitmap_top, dX=g.bitmap_left,
                           bitmap=bytes(data)))
    glyphs.sort(key=lambda g: g['cp'])
    return glyphs


def build_vlw(glyphs, px):
    # ascent/descent de l'en-tête : TFT_eSPI s'en sert pour maxAscent (position
    # de la ligne de base sous cursor_y) et pour deviner la largeur de l'espace
    # ((ascent+descent)*2/7). On les cale sur la boîte réelle du jeu de glyphes.
    asc = max(g['dY'] for g in glyphs if g['h'])
    desc = max(g['h'] - g['dY'] for g in glyphs if g['h'])
    desc = max(desc, 0)

    for g in glyphs:
        assert 0 <= g['w'] <= 255 and 0 <= g['h'] <= 255, g
        assert 0 <= g['adv'] <= 255, g
        assert -128 <= g['dX'] <= 127, g
        assert -32768 <= g['dY'] <= 32767, g

    out = bytearray()
    out += struct.pack('>6I', len(glyphs), VLW_VERSION, px, 0, asc, desc)
    for g in glyphs:
        out += struct.pack('>7i', g['cp'], g['h'], g['w'], g['adv'],
                           g['dY'], g['dX'], 0)
    for g in glyphs:
        out += g['bitmap']
    return bytes(out), asc, desc


def emit_c(path, symbol, blob, meta):
    lines = []
    lines.append('// ' + '-' * 74)
    lines.append('// %s — police VLW GÉNÉRÉE, NE PAS ÉDITER À LA MAIN.' % os.path.basename(path))
    lines.append('// %s' % meta)
    lines.append('// Fonte Inter, Copyright 2020 The Inter Project Authors,')
    lines.append('// SIL Open Font License 1.1 — voir fonts.h.')
    lines.append('// Générateur : gen_vlw.py + check_vlw.py (voir fonts.h).')
    lines.append('// ' + '-' * 74)
    lines.append('#include "fonts.h"')
    lines.append('')
    lines.append('const uint8_t %s[] PROGMEM = {' % symbol)
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        lines.append('  ' + ''.join('0x%02X,' % b for b in chunk))
    lines.append('};')
    lines.append('const uint32_t %s_LEN = %d;' % (symbol, len(blob)))
    lines.append('')
    with open(path, 'w') as f:
        f.write('\n'.join(lines))


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--ttf', required=True)
    p.add_argument('--out-dir', required=True)
    p.add_argument('--spec', action='append', required=True,
                   help='SYMBOLE:px:wght[:opsz]  ex. FONT_UI_20:20:600')
    a = p.parse_args()

    total = 0
    for spec in a.spec:
        parts = spec.split(':')
        sym, px, wght = parts[0], int(parts[1]), float(parts[2])
        opsz = float(parts[3]) if len(parts) > 3 else None
        gl = render_font(a.ttf, px, wght, opsz)
        blob, asc, desc = build_vlw(gl, px)
        fn = os.path.join(a.out_dir, sym.lower() + '.cpp')
        meta = ('%s %dpx wght=%g opsz=%s — %d glyphes, ascent=%d descent=%d, %d octets'
                % (os.path.basename(a.ttf), px, wght, opsz, len(gl), asc, desc, len(blob)))
        emit_c(fn, sym, blob, meta)
        open(os.path.join(a.out_dir, sym.lower() + '.vlw'), 'wb').write(blob)
        total += len(blob)
        print('%-12s %6d octets  %3d glyphes  ascent=%2d descent=%2d  -> %s'
              % (sym, len(blob), len(gl), asc, desc, fn))
    print('TOTAL %d octets (%.1f ko)' % (total, total / 1024.0))
