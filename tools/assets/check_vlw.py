#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Relit un fichier VLW EXACTEMENT comme TFT_eSPI::loadFont()/loadMetrics() le
font (gros-boutiste, troncature des champs aux types C), vérifie sa cohérence
et en rend un échantillon PNG avec la même arithmétique que drawGlyph().
"""
import struct, sys, os
from PIL import Image, ImageDraw

ASCII = [chr(c) for c in range(0x20, 0x7F)]
FRENCH = list("ÀÂÄÇÈÉÊËÎÏÔÖÙÛÜŸàâäçèéêëîïôöùûüÿŒœ«»°·–—’…€²³")
EXPECTED = ASCII + FRENCH


def u8(v):  return v & 0xFF
def i8(v):
    v &= 0xFF
    return v - 256 if v > 127 else v
def i16(v):
    v &= 0xFFFF
    return v - 65536 if v > 32767 else v


class Vlw:
    def __init__(self, blob):
        self.blob = blob
        self.errors = []
        self.warn = []
        p = 0
        rd = lambda o: struct.unpack_from('>I', blob, o)[0]
        self.gCount = rd(0) & 0xFFFF          # TFT_eSPI : (uint16_t)
        self.version = rd(4)
        self.pointSize = rd(8)
        self.mboxY = rd(12)
        self.ascent = rd(16) & 0xFFFF
        self.descent = rd(20) & 0xFFFF
        self.g = []
        headerPtr, bitmapPtr = 24, 24 + self.gCount * 28
        self.bitmapStart = bitmapPtr
        for i in range(self.gCount):
            o = headerPtr + i * 28
            f = struct.unpack_from('>7I', blob, o)
            gl = dict(cp=f[0] & 0xFFFF, h=u8(f[1]), w=u8(f[2]), adv=u8(f[3]),
                      dY=i16(f[4]), dX=i8(f[5]), raw=f, off=bitmapPtr)
            bitmapPtr += gl['w'] * gl['h']
            self.g.append(gl)
        self.bitmapEnd = bitmapPtr
        # maxAscent/maxDescent comme loadMetrics
        self.maxAscent = self.ascent
        self.maxDescent = self.descent
        for gl in self.g:
            if gl['h'] - gl['dY'] > self.maxDescent:
                cp = gl['cp']
                if (0x20 < cp < 0xA0 and cp != 0x7F) or cp > 0xFF:
                    self.maxDescent = gl['h'] - gl['dY']
        self.yAdvance = self.maxAscent + self.maxDescent
        self.spaceWidth = (self.ascent + self.descent) * 2 // 7

    def check(self):
        e = self.errors
        if self.version != 11:
            e.append("version %d (attendu 11)" % self.version)
        if self.gCount != len(self.g):
            e.append("gCount incohérent")
        if self.bitmapEnd != len(self.blob):
            e.append("somme des bitmaps : fin calculée %d != taille fichier %d"
                     % (self.bitmapEnd, len(self.blob)))
        # les champs 32 bits ne doivent pas déborder des types C de TFT_eSPI
        for gl in self.g:
            r = gl['raw']
            if r[0] > 0xFFFF: e.append("U+%X hors BMP" % r[0])
            for name, idx, lim in (('h', 1, 255), ('w', 2, 255), ('adv', 3, 255)):
                if r[idx] > lim:
                    e.append("U+%04X %s=%d tronqué en uint8" % (gl['cp'], name, r[idx]))
            dx = struct.unpack('>i', struct.pack('>I', r[5]))[0]
            if not (-128 <= dx <= 127):
                e.append("U+%04X dX=%d tronqué en int8" % (gl['cp'], dx))
        present = {gl['cp'] for gl in self.g}
        missing = [c for c in EXPECTED if ord(c) not in present]
        if missing:
            e.append("codepoints manquants : " + " ".join(
                "%r/U+%04X" % (c, ord(c)) for c in missing))
        extra = present - {ord(c) for c in EXPECTED}
        if extra:
            self.warn.append("glyphes en trop : " + " ".join("U+%04X" % c for c in sorted(extra)))
        cps = [gl['cp'] for gl in self.g]
        if cps != sorted(cps):
            self.warn.append("glyphes non triés par codepoint")
        # métriques plausibles
        for gl in self.g:
            if gl['cp'] == 0x20:
                if gl['w'] or gl['h']:
                    self.warn.append("l'espace porte un bitmap (inutile)")
                continue
            if gl['w'] == 0 or gl['h'] == 0:
                self.warn.append("U+%04X bitmap vide" % gl['cp'])
            if gl['dY'] > self.ascent:
                e.append("U+%04X dY=%d > ascent=%d : le glyphe déborderait en haut"
                         % (gl['cp'], gl['dY'], self.ascent))
            if gl['adv'] == 0:
                self.warn.append("U+%04X gxAdvance=0" % gl['cp'])
            if gl['w'] > 4 * self.pointSize or gl['h'] > 3 * self.pointSize:
                self.warn.append("U+%04X bitmap %dx%d suspect" % (gl['cp'], gl['w'], gl['h']))
        if not (0 < self.spaceWidth < self.pointSize):
            self.warn.append("largeur d'espace déduite = %d px" % self.spaceWidth)
        return not e

    def bitmap(self, gl):
        return self.blob[gl['off']:gl['off'] + gl['w'] * gl['h']]

    def index(self, cp):
        for gl in self.g:
            if gl['cp'] == cp:
                return gl
        return None

    # --- même arithmétique que TFT_eSPI::drawGlyph -------------------------
    def draw(self, img, x, y, text, color=(255, 255, 255)):
        px = img.load()
        cx = x
        for ch in text:
            cp = ord(ch)
            if cp == 0x20:
                cx += self.spaceWidth
                continue
            gl = self.index(cp)
            if gl is None:
                cx += self.spaceWidth + 1
                continue
            bm = self.bitmap(gl)
            gy = y + self.maxAscent - gl['dY']
            gx = cx + gl['dX']
            for j in range(gl['h']):
                for i in range(gl['w']):
                    a = bm[j * gl['w'] + i]
                    if not a:
                        continue
                    X, Y = gx + i, gy + j
                    if 0 <= X < img.width and 0 <= Y < img.height:
                        b = px[X, Y]
                        px[X, Y] = tuple(
                            (color[k] * a + b[k] * (255 - a)) // 255 for k in range(3))
            cx += gl['adv']
        return cx - x

    def width(self, text):
        w = 0
        for ch in text:
            cp = ord(ch)
            gl = self.index(cp)
            w += self.spaceWidth if cp == 0x20 or gl is None else gl['adv']
        return w


def main(paths, png):
    fonts = []
    ok = True
    for p in paths:
        blob = open(p, 'rb').read()
        f = Vlw(blob)
        good = f.check()
        ok &= good
        print("=== %s (%d octets)" % (os.path.basename(p), len(blob)))
        print("    gCount=%d version=%d taille=%dpt ascent=%d descent=%d "
              "maxDescent=%d yAdvance=%d espace=%dpx"
              % (f.gCount, f.version, f.pointSize, f.ascent, f.descent,
                 f.maxDescent, f.yAdvance, f.spaceWidth))
        print("    en-tête 24 + métriques %d + bitmaps %d = %d %s"
              % (28 * f.gCount, f.bitmapEnd - f.bitmapStart, f.bitmapEnd,
                 "OK" if f.bitmapEnd == len(blob) else "MISMATCH"))
        print("    « Cinéma » = %d px de large, hauteur de ligne %d px"
              % (f.width("Cinéma"), f.yAdvance))
        for w in f.warn: print("    [avert] " + w)
        for e in f.errors: print("    [ERREUR] " + e)
        if good and not f.warn: print("    tout est cohérent.")
        fonts.append((os.path.basename(p), f))

    # planche d'échantillon, fond sombre comme l'écran
    SAMPLES = ["Cinéma — Salon « Œil » 12°C",
               "ÀÂÄÇÈÉÊËÎÏÔÖÙÛÜŸŒ àâäçèéêëîïôöùûüÿœ",
               "Enrôlement… 87 % · 3,5 m² · 12 € · ABCDEFGHIJ",
               "abcdefghijklmnopqrstuvwxyz 0123456789 @#$%&*()"]
    W = 900
    H = 24 + sum(20 + len(SAMPLES) * (f.yAdvance + 6) for _, f in fonts)
    img = Image.new('RGB', (W, H), (16, 18, 22))
    d = ImageDraw.Draw(img)
    y = 12
    for name, f in fonts:
        d.text((8, y), "%s  ascent=%d descent=%d yAdv=%d" % (name, f.ascent, f.descent, f.yAdvance),
               fill=(120, 190, 255))
        y += 16
        for s in SAMPLES:
            f.draw(img, 10, y, s, (235, 238, 242))
            y += f.yAdvance + 6
        y += 8
        d.line([(0, y - 6), (W, y - 6)], fill=(50, 55, 62))
    img.save(png)
    print("\naperçu : %s" % png)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:-1], sys.argv[-1]))
