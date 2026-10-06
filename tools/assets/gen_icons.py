#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Dessine les 39 pictogrammes du firmware et produit src/ui/icons.{h,cpp}.

FORMAT DE SORTIE — un octet d'ALPHA par pixel (0 = transparent, 255 = opaque),
rangée par rangée, sans en-tête. Deux tailles issues de la MÊME source
vectorielle (la 40 n'est jamais un agrandissement de la 24) :

    24x24 ->  576 octets par icône   (tuiles des grilles à 3 rangées)
    40x40 -> 1600 octets par icône   (grandes tuiles des grilles à 2 rangées)

POURQUOI L'ALPHA — les pictogrammes voisinent des libellés en VLW anti-crénelé.
En 1 bit par pixel ils faisaient « bricolage » à côté. Le firmware mélange
lui-même l'alpha entre la couleur d'avant-plan et le fond de la tuile.

STYLE — un seul registre pour les 39 : le TRAIT (« outline »), épaisseur 2
unités sur une toile de 24, extrémités et jointures arrondies. Rien n'est plein
sauf de petits points d'accent (bouton de porte, serrure, diodes) qui sont trop
petits pour être tracés. Mélanger du trait et de l'aplat est exactement ce qui
fait « dépareillé ».

RASTÉRISATION — les formes sont décrites en coordonnées réelles sur une toile
de 24 unités, puis rastérisées à l'échelle voulue avec sur-échantillonnage x8 et
moyenne exacte (réduction BOX 8:1). Les courbes de Bézier et les arcs sont
aplatis en polylignes denses ; le trait est produit par segments épais plus un
disque à chaque sommet, ce qui donne des jointures et des extrémités rondes par
construction.
"""
import math
import os
import sys

from PIL import Image, ImageDraw

# --------------------------------------------------------------------- réglages
UNITS = 24.0     # la toile logique fait toujours 24x24 unités
SS = 8           # sur-échantillonnage
STROKE = 2.0     # épaisseur de trait de référence, en unités

SIZES = (24, 40)  # tailles rastérisées

NAMES = """none bulb lamp ceiling strip plug power gate garage door window
shutter blind lock heat cool fan thermo water valve tv music speaker camera
alarm shield scene movie night sun moon coffee folder home grid car mower
vacuum bell clock""".split()
assert len(NAMES) == 40, len(NAMES)


# ------------------------------------------------------------------- géométrie
def _bezier(p0, p1, p2, p3, n=26):
    """Aplatit une Bézier cubique en n segments."""
    out = []
    for i in range(1, n + 1):
        t = i / n
        u = 1.0 - t
        out.append((u * u * u * p0[0] + 3 * u * u * t * p1[0]
                    + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                    u * u * u * p0[1] + 3 * u * u * t * p1[1]
                    + 3 * u * t * t * p2[1] + t * t * t * p3[1]))
    return out


class Path:
    """Chemin en coordonnées réelles. Les angles sont en degrés, mesurés depuis
    l'axe +x, et croissent dans le sens des aiguilles d'une montre puisque y
    pointe vers le bas (même convention que PIL et que l'ancien script)."""

    def __init__(self):
        self.pts = []
        self.closed = False

    # -- primitives
    def M(self, x, y):
        self.pts = [(x, y)]
        return self

    def L(self, x, y):
        self.pts.append((x, y))
        return self

    def C(self, x1, y1, x2, y2, x, y):
        self.pts += _bezier(self.pts[-1], (x1, y1), (x2, y2), (x, y))
        return self

    def A(self, cx, cy, r, a0, a1):
        """Arc de a0 à a1 ; le signe du balayage donne le sens."""
        sweep = a1 - a0
        n = max(6, int(abs(sweep) / 4.0))
        for i in range(n + 1):
            a = math.radians(a0 + sweep * i / n)
            p = (cx + r * math.cos(a), cy + r * math.sin(a))
            if not self.pts or p != self.pts[-1]:
                self.pts.append(p)
        return self

    def Z(self):
        self.closed = True
        return self


def rounded(pts, r, closed=True):
    """Polyligne à coins arrondis. Le coin est une quadratique tendue entre les
    deux points de tangence, visuellement indiscernable d'un arc à ces rayons."""
    n = len(pts)
    p = Path()
    idx = range(n) if closed else range(1, n - 1)
    out = []
    for i in range(n):
        if i not in idx:
            out.append(('pt', pts[i]))
            continue
        v = pts[i]
        a = pts[(i - 1) % n]
        b = pts[(i + 1) % n]
        da = (a[0] - v[0], a[1] - v[1])
        db = (b[0] - v[0], b[1] - v[1])
        la = math.hypot(*da) or 1e-9
        lb = math.hypot(*db) or 1e-9
        rr = min(r, la / 2.0, lb / 2.0)
        A = (v[0] + da[0] / la * rr, v[1] + da[1] / la * rr)
        B = (v[0] + db[0] / lb * rr, v[1] + db[1] / lb * rr)
        out.append(('corner', A, v, B))

    first = True
    for e in out:
        if e[0] == 'pt':
            (p.M if first else p.L)(*e[1])
        else:
            _, A, V, B = e
            if first:
                p.M(*A)
            else:
                p.L(*A)
            c1 = (A[0] + 2.0 / 3 * (V[0] - A[0]), A[1] + 2.0 / 3 * (V[1] - A[1]))
            c2 = (B[0] + 2.0 / 3 * (V[0] - B[0]), B[1] + 2.0 / 3 * (V[1] - B[1]))
            p.C(c1[0], c1[1], c2[0], c2[1], B[0], B[1])
        first = False
    if closed:
        p.Z()
    return p


# ---------------------------------------------------------------------- toile
class Canvas:
    """Accumule des ordres de tracé en unités ; rastérise à la demande."""

    def __init__(self):
        self.ops = []

    def stroke(self, path, w=STROKE):
        self.ops.append(('stroke', path, w))

    def fill(self, path):
        self.ops.append(('fill', path))

    # -- raccourcis
    def line(self, pts, w=STROKE):
        p = Path().M(*pts[0])
        for q in pts[1:]:
            p.L(*q)
        self.stroke(p, w)

    def poly(self, pts, w=STROKE, r=0.0):
        if r:
            self.stroke(rounded(pts, r, True), w)
            return
        p = Path().M(*pts[0])
        for q in pts[1:]:
            p.L(*q)
        self.stroke(p.Z(), w)

    def rrect(self, x0, y0, x1, y1, r=2.0, w=STROKE):
        self.poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], w, r)

    def circle(self, cx, cy, r, w=STROKE):
        self.stroke(Path().A(cx, cy, r, 0, 360).Z(), w)

    def arc(self, cx, cy, r, a0, a1, w=STROKE):
        self.stroke(Path().A(cx, cy, r, a0, a1), w)

    def dot(self, cx, cy, r):
        self.fill(Path().A(cx, cy, r, 0, 360).Z())

    # -- rastérisation
    def _raster(self, size, pad=0):
        """Rastérise sur `size + 2*pad` pixels, le contenu décalé de `pad`.
        `pad > 0` sert au contrôle de débordement : la couronne ajoutée doit
        rester vide, sinon une forme sort de la toile de 24 unités."""
        scale = size * SS / UNITS
        off = pad * SS
        dim = (size + 2 * pad) * SS
        img = Image.new('L', (dim, dim), 0)
        d = ImageDraw.Draw(img)
        T = lambda p: (p[0] * scale + off, p[1] * scale + off)
        for op in self.ops:
            if op[0] == 'fill':
                d.polygon([T(q) for q in op[1].pts], fill=255)
            else:
                _, path, w = op
                pts = [T(q) for q in path.pts]
                if path.closed and pts[0] != pts[-1]:
                    pts = pts + [pts[0]]
                pw = max(1, int(round(w * scale)))
                if len(pts) > 1:
                    d.line(pts, fill=255, width=pw, joint='curve')
                # Extrémités rondes : joint='curve' ne traite que les sommets
                # intérieurs. Les bornes d'ellipse de PIL sont INCLUSIVES, d'où
                # le -1 : sans lui le disque déborde d'un pixel suréchantillonné
                # et le trait finit ~3 % trop épais.
                rad = pw / 2.0
                for x, y in (pts[0], pts[-1]):
                    d.ellipse([x - rad, y - rad, x + rad - 1, y + rad - 1], fill=255)
        return img.resize((size + 2 * pad, size + 2 * pad), Image.BOX)

    def render(self, size):
        """Renvoie `size*size` octets d'alpha."""
        return self._raster(size).tobytes()

    def overflow(self, size=48, pad=4):
        """Alpha maximal tombant HORS de la toile de 24 unités."""
        img = self._raster(size, pad)
        px = img.load()
        n = size + 2 * pad
        worst = 0
        for y in range(n):
            for x in range(n):
                inside = (pad <= x < pad + size) and (pad <= y < pad + size)
                if not inside:
                    worst = max(worst, px[x, y])
        return worst


# ------------------------------------------------------------------- glyphes
# Toutes les coordonnées sont en unités sur une toile de 24, marge ~1,5 unité.

def i_none(c):
    pass


def i_bulb(c):
    c.circle(12, 9.4, 5.3)                                  # verre
    c.line([(9.3, 14.2), (9.3, 16.4), (14.7, 16.4), (14.7, 14.2)])  # collerette
    c.line([(10.0, 18.8), (14.0, 18.8)])                    # culot
    c.line([(10.9, 21.0), (13.1, 21.0)])


def i_lamp(c):
    c.poly([(6.4, 12.6), (9.4, 4.4), (14.6, 4.4), (17.6, 12.6)], r=1.0)
    c.line([(12, 12.6), (12, 19.4)])                        # pied
    c.line([(7.6, 19.8), (16.4, 19.8)])                     # socle


def i_ceiling(c):
    c.line([(12, 1.8), (12, 5.4)])                          # tige
    c.poly([(3.8, 13.4), (7.8, 5.4), (16.2, 5.4), (20.2, 13.4)], r=1.0)
    c.line([(6.8, 16.4), (5.8, 20.4)])                      # lumière
    c.line([(12, 16.8), (12, 21.2)])
    c.line([(17.2, 16.4), (18.2, 20.4)])


def i_strip(c):
    c.rrect(2.0, 5.8, 22.0, 11.4, r=2.0)                    # ruban
    for x in (5.9, 10.0, 14.0, 18.1):
        c.dot(x, 8.6, 1.0)                                  # diodes
    c.line([(6.8, 14.2), (5.8, 18.2)])                      # lumière
    c.line([(12, 14.6), (12, 19.0)])
    c.line([(17.2, 14.2), (18.2, 18.2)])


def i_plug(c):
    c.line([(9.0, 2.2), (9.0, 7.8)])                        # broches
    c.line([(15.0, 2.2), (15.0, 7.8)])
    c.rrect(5.4, 7.8, 18.6, 14.6, r=2.0)                    # corps
    c.line([(12, 14.6), (12, 21.6)])                        # câble


def i_power(c):
    c.arc(12, 13.0, 7.1, -55, 235)
    c.line([(12, 3.6), (12, 12.0)])


def i_gate(c):
    c.line([(3.0, 5.2), (3.0, 21.2)])                       # montants
    c.line([(21.0, 5.2), (21.0, 21.2)])
    c.line([(3.0, 8.0), (21.0, 8.0)])                       # traverses
    c.line([(3.0, 18.4), (21.0, 18.4)])
    c.line([(6.0, 18.4), (11.0, 8.0)])                      # croisillons
    c.line([(13.0, 18.4), (18.0, 8.0)])


def i_garage(c):
    c.line([(1.6, 11.4), (12, 3.4), (22.4, 11.4)])          # toit
    c.line([(3.6, 10.2), (3.6, 21.2), (20.4, 21.2), (20.4, 10.2)])
    c.line([(6.6, 14.8), (17.4, 14.8)])                     # lamelles
    c.line([(6.6, 18.0), (17.4, 18.0)])


def i_door(c):
    c.rrect(5.4, 2.6, 18.6, 20.8, r=1.4)
    c.dot(15.0, 12.0, 1.15)                                 # poignée
    c.line([(2.6, 20.8), (21.4, 20.8)])                     # sol


def i_window(c):
    c.rrect(3.0, 3.0, 21.0, 21.0, r=2.0)
    c.line([(12, 3.4), (12, 20.6)])
    c.line([(3.4, 12), (20.6, 12)])


def i_shutter(c):
    c.rrect(3.0, 2.8, 21.0, 21.2, r=1.8)
    c.line([(3.4, 7.4), (20.6, 7.4)], w=2.4)                # coffre
    c.line([(6.6, 12.0), (17.4, 12.0)])                     # tablier
    c.line([(6.6, 16.6), (17.4, 16.6)])


def i_blind(c):
    c.line([(2.2, 3.8), (21.8, 3.8)], w=2.6)                # bandeau
    c.line([(4.6, 8.4), (17.4, 8.4)])                       # lamelles relevées
    c.line([(4.6, 11.8), (17.4, 11.8)])
    c.line([(4.6, 15.2), (17.4, 15.2)])
    c.line([(20.8, 5.0), (20.8, 19.2)], w=1.7)              # cordon
    c.dot(20.8, 20.6, 1.25)


def i_lock(c):
    c.arc(12, 9.8, 4.5, 180, 360)                           # anse
    c.line([(7.5, 9.8), (7.5, 12.6)])
    c.line([(16.5, 9.8), (16.5, 12.6)])
    c.rrect(4.4, 12.6, 19.6, 21.6, r=2.2)                   # corps
    c.dot(12, 16.2, 1.2)                                    # serrure
    c.line([(12, 16.8), (12, 18.9)], w=1.8)


def i_heat(c):                                              # flamme
    p = Path().M(12.0, 1.6)
    p.C(15.0, 5.6, 18.6, 9.4, 18.6, 14.0)
    p.C(18.6, 18.6, 15.6, 22.2, 12.0, 22.2)
    p.C(8.4, 22.2, 5.4, 18.6, 5.4, 14.0)
    p.C(5.4, 10.6, 7.6, 8.0, 9.3, 6.3)
    p.C(9.9, 8.8, 10.6, 10.4, 11.8, 11.5)
    p.C(10.6, 8.2, 10.4, 4.6, 12.0, 1.6).Z()
    c.stroke(p)
    q = Path().M(12.0, 12.6)
    q.C(13.9, 14.8, 14.8, 16.3, 14.8, 17.9)
    q.C(14.8, 20.0, 13.5, 21.2, 12.0, 21.2)
    q.C(10.5, 21.2, 9.2, 20.0, 9.2, 17.9)
    q.C(9.2, 16.3, 10.1, 14.8, 12.0, 12.6).Z()
    c.stroke(q, 1.8)


def i_cool(c):                                              # flocon
    for a in (90, 150, 210, 270, 330, 30):
        r = math.radians(a)
        c.line([(12, 12), (12 + 9.2 * math.cos(r), 12 - 9.2 * math.sin(r))])
        bx, by = 12 + 5.4 * math.cos(r), 12 - 5.4 * math.sin(r)
        for da in (-52, 52):
            r2 = math.radians(a + da)
            c.line([(bx, by), (bx + 3.0 * math.cos(r2), by - 3.0 * math.sin(r2))], w=1.7)


def i_fan(c):
    for a in (0, 120, 240):
        t = math.radians(a)
        cs, sn = math.cos(t), math.sin(t)
        def R(x, y):
            return (12 + x * cs - y * sn, 12 + x * sn + y * cs)
        p = Path().M(*R(0, -2.4))
        p.C(*R(-3.1, -4.4), *R(-3.7, -8.9), *R(0, -9.5))
        p.C(*R(3.7, -8.9), *R(3.1, -4.4), *R(0, -2.4)).Z()
        c.stroke(p)
    c.circle(12, 12, 2.1)


def i_thermo(c):
    R, HW = 4.3, 2.3                                        # rayon du bulbe, demi-tube
    a = math.degrees(math.acos(HW / R))                     # tangence tube/bulbe
    p = Path().M(12 - HW, 14.6)
    p.L(12 - HW, 5.0)
    p.A(12, 5.0, HW, 180, 360)                              # capuchon
    p.L(12 + HW, 14.6)
    p.A(12, 18.0, R, -a, 180 + a)                           # reservoir
    p.Z()
    c.stroke(p)
    c.dot(12, 18.0, 1.9)
    c.line([(17.2, 8.4), (20.6, 8.4)], w=1.8)               # graduations
    c.line([(17.2, 12.0), (20.6, 12.0)], w=1.8)


def i_water(c):                                             # goutte
    p = Path().M(12.0, 1.8)
    p.C(15.2, 6.4, 18.8, 11.2, 18.8, 14.8)
    p.C(18.8, 18.9, 15.7, 22.0, 12.0, 22.0)
    p.C(8.3, 22.0, 5.2, 18.9, 5.2, 14.8)
    p.C(5.2, 11.2, 8.8, 6.4, 12.0, 1.8).Z()
    c.stroke(p)


def i_valve(c):
    c.circle(12, 13.6, 4.3)                                 # corps
    c.line([(1.8, 13.6), (7.7, 13.6)])                      # conduite
    c.line([(16.3, 13.6), (22.2, 13.6)])
    c.line([(1.8, 10.6), (1.8, 16.6)])                      # brides
    c.line([(22.2, 10.6), (22.2, 16.6)])
    c.line([(12, 9.3), (12, 4.4)])                          # tige
    c.line([(7.0, 3.6), (17.0, 3.6)])                       # volant


def i_tv(c):
    c.rrect(2.0, 4.0, 22.0, 17.6, r=2.4)
    c.line([(12, 17.6), (12, 20.8)])
    c.line([(8.0, 20.8), (16.0, 20.8)])


def i_music(c):
    c.circle(7.8, 18.0, 2.7)                                # têtes
    c.circle(16.4, 16.4, 2.7)
    c.line([(10.5, 18.0), (10.5, 4.4)])                     # hampes
    c.line([(19.1, 16.4), (19.1, 2.8)])
    c.line([(10.5, 4.4), (19.1, 2.8)], w=2.6)               # barre


def i_speaker(c):
    c.poly([(2.6, 9.6), (7.0, 9.6), (11.8, 4.8), (11.8, 19.2),
            (7.0, 14.4), (2.6, 14.4)], r=1.0)
    c.arc(12.6, 12.0, 4.6, -58, 58)                         # ondes
    c.arc(12.6, 12.0, 8.2, -52, 52)


def i_camera(c):
    c.poly([(1.6, 20.4), (1.6, 7.0), (7.4, 7.0), (8.8, 3.8),
            (13.4, 3.8), (14.8, 7.0), (22.4, 7.0), (22.4, 20.4)], r=1.8)
    c.circle(12, 13.8, 4.3)                                 # objectif
    c.dot(19.0, 10.4, 1.0)                                  # flash


def i_alarm(c):
    c.poly([(12, 2.6), (22.2, 20.8), (1.8, 20.8)], r=2.2)
    c.line([(12, 9.4), (12, 15.4)])
    c.dot(12, 18.2, 1.2)


def i_shield(c):
    c.poly([(12, 2.0), (20.8, 5.4), (20.8, 12.2), (12, 21.8),
            (3.2, 12.2), (3.2, 5.4)], r=1.8)
    c.line([(8.0, 12.0), (10.9, 15.1), (16.0, 8.8)])


def _spark(cx, cy, r):
    k = r * 0.17
    p = Path().M(cx, cy - r)
    p.C(cx + k, cy - k, cx + k, cy - k, cx + r, cy)
    p.C(cx + k, cy + k, cx + k, cy + k, cx, cy + r)
    p.C(cx - k, cy + k, cx - k, cy + k, cx - r, cy)
    p.C(cx - k, cy - k, cx - k, cy - k, cx, cy - r)
    return p.Z()


def i_scene(c):
    c.stroke(_spark(9.4, 9.6, 7.8))
    c.stroke(_spark(18.2, 18.0, 4.5), 1.8)
    c.dot(19.6, 5.0, 1.2)


def i_movie(c):
    top = ((2.0, 3.8), (20.4, 1.4))
    bot = ((3.8, 9.6), (22.2, 7.2))
    c.poly([top[0], top[1], bot[1], bot[0]], r=1.0)         # battant
    for t in (0.30, 0.55, 0.80):
        a = (top[0][0] + (top[1][0] - top[0][0]) * t,
             top[0][1] + (top[1][1] - top[0][1]) * t)
        b = (bot[0][0] + (bot[1][0] - bot[0][0]) * t,
             bot[0][1] + (bot[1][1] - bot[0][1]) * t)
        c.line([a, b], w=1.8)
    c.rrect(2.0, 10.8, 22.0, 21.8, r=2.0)                   # ardoise


def i_night(c):                                             # lit
    c.line([(2.2, 20.6), (2.2, 7.6)])                       # tête de lit
    c.rrect(2.2, 13.0, 21.8, 18.2, r=2.0)                   # matelas
    c.rrect(5.2, 9.4, 11.4, 13.0, r=1.4)                    # oreiller
    c.line([(21.8, 18.2), (21.8, 20.6)])                    # pied


def i_sun(c):
    c.circle(12, 12, 5.0)
    for a in range(0, 360, 45):
        r = math.radians(a)
        c.line([(12 + 7.9 * math.cos(r), 12 + 7.9 * math.sin(r)),
                (12 + 10.8 * math.cos(r), 12 + 10.8 * math.sin(r))])


def i_moon(c):
    R1, C1 = 9.2, (12.0, 12.0)
    a = 60.0
    A = (C1[0] + R1 * math.cos(math.radians(-a)), C1[1] + R1 * math.sin(math.radians(-a)))
    C2x = A[0] + 0.35
    half = abs(C1[1] - A[1])
    R2 = math.hypot(C2x - A[0], half)
    b = math.degrees(math.atan2(half, A[0] - C2x))
    p = Path().A(C1[0], C1[1], R1, -a, -360 + a)            # bord extérieur
    p.A(C2x, C1[1], R2, 180 - b, 180 + b)                   # bord intérieur
    p.Z()
    c.stroke(p)


def i_coffee(c):
    c.poly([(3.2, 9.6), (15.8, 9.6), (14.4, 19.4), (4.6, 19.4)], r=1.4)
    h = Path().M(15.6, 11.0)
    h.C(19.8, 11.0, 19.8, 16.6, 15.1, 16.6)
    c.stroke(h)                                             # anse
    c.line([(1.8, 21.8), (18.6, 21.8)])                     # soucoupe
    c.line([(7.2, 6.8), (7.2, 3.2)], w=1.8)                 # vapeur
    c.line([(11.8, 6.8), (11.8, 3.2)], w=1.8)


def i_folder(c):
    c.poly([(2.0, 19.8), (2.0, 5.4), (8.8, 5.4), (11.2, 8.4),
            (22.0, 8.4), (22.0, 19.8)], r=1.8)


def i_home(c):
    c.line([(1.6, 11.6), (12, 3.0), (22.4, 11.6)])          # toit
    c.line([(4.4, 9.4), (4.4, 21.2), (19.6, 21.2), (19.6, 9.4)])
    c.line([(9.4, 21.2), (9.4, 15.6), (14.6, 15.6), (14.6, 21.2)])


def i_grid(c):
    for x in (2.4, 13.0):
        for y in (2.4, 13.0):
            c.rrect(x, y, x + 8.6, y + 8.6, r=1.8)


def i_car(c):
    c.poly([(1.8, 17.6), (1.8, 12.4), (4.6, 12.4), (7.6, 6.6),
            (16.4, 6.6), (19.4, 12.4), (22.2, 12.4), (22.2, 17.6)], r=1.6)
    c.line([(4.9, 12.4), (19.1, 12.4)])                     # ceinture de caisse
    c.line([(12, 6.9), (12, 12.4)])                         # montant central
    c.circle(7.0, 17.6, 2.7)                                # roues
    c.circle(17.0, 17.6, 2.7)


def i_mower(c):                                             # autoportee, profil
    c.poly([(1.4, 17.6), (1.4, 14.6), (8.0, 14.6), (8.0, 9.8),
            (15.0, 9.8), (15.0, 5.0), (20.4, 5.0), (20.4, 17.6)], r=1.4)
    c.circle(16.2, 17.8, 3.5)                               # grande roue arriere
    c.circle(4.7, 18.2, 2.2)                                # petite roue avant


def i_vacuum(c):                                            # robot, vu de dessus
    R = 9.2
    y = 7.6
    dx = math.sqrt(R * R - (12 - y) ** 2)
    c.circle(12, 12, R)
    c.line([(12 - dx, y), (12 + dx, y)])                    # pare-chocs
    c.circle(12, 13.8, 3.0)                                 # brosse centrale


def i_bell(c):
    p = Path().M(5.0, 17.6)
    p.C(6.4, 15.2, 6.8, 12.6, 6.8, 10.0)
    p.C(6.8, 6.6, 9.1, 4.0, 12.0, 4.0)
    p.C(14.9, 4.0, 17.2, 6.6, 17.2, 10.0)
    p.C(17.2, 12.6, 17.6, 15.2, 19.0, 17.6).Z()
    c.stroke(p)
    c.arc(12, 17.8, 2.5, 0, 180)                            # battant
    c.circle(12, 2.5, 1.2, w=1.8)                           # attache


def i_clock(c):
    c.circle(12, 12, 9.2)
    c.line([(12, 12), (12, 6.4)])
    c.line([(12, 12), (16.2, 14.6)])


DRAW = {n: globals()['i_' + n] for n in NAMES}


# ------------------------------------------------------------------ production
def build(strict=True):
    """{nom: {taille: octets d'alpha}} pour les 39 icônes dessinées.

    `strict` vérifie en prime qu'aucune forme ne sort de la toile de 24 unités :
    un dépassement serait rogné à l'affichage sans que rien ne le signale."""
    out = {}
    bad = []
    for n in NAMES[1:]:
        c = Canvas()
        DRAW[n](c)
        out[n] = {s: c.render(s) for s in SIZES}
        if strict:
            o = c.overflow()
            if o > 2:
                bad.append((n, o))
    if bad:
        raise SystemExit('formes hors toile : '
                         + ', '.join('%s (alpha %d)' % b for b in bad))
    return out


def emit_header(path):
    up = lambda n: 'ICON_' + n.upper()
    L = []
    L.append('// ---------------------------------------------------------------------------')
    L.append('// Pictogrammes des tuiles : 39 glyphes en ALPHA 8 BITS, en flash.')
    L.append('//')
    L.append('// Le contrat d\'API transporte un NOM (« bulb », « shutter »…) pris dans un')
    L.append('// vocabulaire FERMÉ. Le firmware le résout UNE FOIS au parsing, vers un')
    L.append('// IconId tenant sur un octet, et ne conserve jamais la chaîne : Layout est')
    L.append('// recopié en NVS et chaque octet économisé l\'est 32 fois.')
    L.append('//')
    L.append('// FORMAT : un octet par pixel, rangée par rangée, sans en-tête. L\'octet est')
    L.append('// l\'OPACITÉ du pixel — 0 transparent, 255 opaque, et tout l\'intervalle sert.')
    L.append('// Le tracé se fait donc en mélangeant, pour chaque pixel :')
    L.append('//     couleur = (avant_plan * alpha + fond * (255 - alpha)) / 255')
    L.append('// exactement comme TFT_eSPI le fait pour ses polices lissées (alphaBlend).')
    L.append('// Un seuillage binaire redonnerait le crénelage qu\'on cherche à supprimer.')
    L.append('//')
    L.append('// DEUX TAILLES, issues de la même source vectorielle — la grande n\'est PAS')
    L.append('// un agrandissement de la petite, elle est rastérisée séparément :')
    L.append('//     24x24 ->  576 octets, tuiles des grilles à 3 rangées')
    L.append('//     40x40 -> 1600 octets, grandes tuiles des grilles à 2 rangées')
    L.append('// ---------------------------------------------------------------------------')
    L.append('#pragma once')
    L.append('')
    L.append('#include <Arduino.h>')
    L.append('')
    L.append('// ⚠️ L\'ORDRE EST CELUI DU CONTRAT D\'API : un IconId est écrit tel quel en')
    L.append('// NVS par layout_store. Ne jamais insérer au milieu ni réordonner — seulement')
    L.append('// ajouter avant ICON_COUNT, et de concert avec le plugin Jeedom.')
    L.append('enum IconId : uint8_t {')
    for i in range(0, len(NAMES), 5):
        chunk = NAMES[i:i + 5]
        if i == 0:
            L.append('  ICON_NONE = 0, ' + ' '.join('%s,' % up(n) for n in chunk[1:]))
        else:
            L.append('  ' + ' '.join('%s,' % up(n) for n in chunk))
    L.append('  ICON_COUNT')
    L.append('};')
    L.append('')
    L.append('static const int16_t ICON_W = 24;       // petite taille')
    L.append('static const int16_t ICON_H = 24;')
    L.append('static const int16_t ICON_BIG_W = 40;   // grande taille')
    L.append('static const int16_t ICON_BIG_H = 40;')
    L.append('')
    L.append('// Longueur des deux tampons, pour éviter de réécrire 24*24 à la main.')
    L.append('static const size_t ICON_A24_BYTES = 24u * 24u;   //  576')
    L.append('static const size_t ICON_A40_BYTES = 40u * 40u;   // 1600')
    L.append('')
    L.append('// Nom du contrat -> identifiant. Insensible à la casse. Nom inconnu,')
    L.append('// chaîne vide ou NULL -> ICON_NONE.')
    L.append('IconId iconFromName(const char *name);')
    L.append('')
    L.append('// Nom canonique d\'un identifiant (utile aux journaux). Jamais NULL.')
    L.append('const char *iconName(IconId id);')
    L.append('')
    L.append('// 576 octets d\'alpha (24*24), ou nullptr pour ICON_NONE et pour tout')
    L.append('// identifiant hors bornes : l\'appelant se rabat sur le seul libellé.')
    L.append('const uint8_t *iconAlpha24(IconId id);')
    L.append('')
    L.append('// 1600 octets d\'alpha (40*40), mêmes conventions.')
    L.append('const uint8_t *iconAlpha40(IconId id);')
    L.append('')
    open(path, 'w').write('\n'.join(L))


def _emit_table(L, name, rows, blobs, per_line=24):
    L.append('const uint8_t %s[%d][%s] PROGMEM = {' % (name, len(blobs), rows))
    for n, b in blobs:
        L.append('  { // %s' % n)
        for i in range(0, len(b), per_line):
            L.append('    ' + ''.join('0x%02X,' % x for x in b[i:i + per_line]))
        L.append('  },')
    L.append('};')
    L.append('')


def emit(bits, path):
    L = []
    L.append('// ---------------------------------------------------------------------------')
    L.append('// icons.cpp — 39 pictogrammes en alpha 8 bits, GÉNÉRÉ par tools/assets/gen_icons.py.')
    L.append('// Ne pas éditer à la main : modifier le script et régénérer.')
    L.append('//')
    L.append('// ICON_A24[id - 1] et ICON_A40[id - 1] : ICON_NONE n\'a pas de bitmap, les')
    L.append('// tables commencent donc à ICON_BULB. %d + %d octets de flash.'
             % (576 * 39, 1600 * 39))
    L.append('// ---------------------------------------------------------------------------')
    L.append('#include "icons.h"')
    L.append('')
    L.append('namespace {')
    L.append('')
    _emit_table(L, 'ICON_A24', 'ICON_A24_BYTES', [(n, bits[n][24]) for n in NAMES[1:]])
    _emit_table(L, 'ICON_A40', 'ICON_A40_BYTES', [(n, bits[n][40]) for n in NAMES[1:]], 40)
    L.append('// Noms du contrat d\'API, dans l\'ordre exact de IconId.')
    L.append('const char *const NAMES[ICON_COUNT] = {')
    for i in range(0, len(NAMES), 6):
        L.append('  ' + ' '.join('"%s",' % n for n in NAMES[i:i + 6]))
    L.append('};')
    L.append('')
    L.append('// Comparaison insensible à la casse, bornée : strcasecmp existe sur ESP32')
    L.append('// mais dépend de la locale ; ici on reste en ASCII pur.')
    L.append('bool sameName(const char *a, const char *b) {')
    L.append('  for (;; a++, b++) {')
    L.append('    char ca = *a, cb = *b;')
    L.append("    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);")
    L.append("    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);")
    L.append('    if (ca != cb) return false;')
    L.append("    if (ca == '\\0') return true;")
    L.append('  }')
    L.append('}')
    L.append('')
    L.append('}  // namespace')
    L.append('')
    L.append('IconId iconFromName(const char *name) {')
    L.append('  // Un nom absent du vocabulaire fermé du contrat retombe sur ICON_NONE :')
    L.append('  // la tuile s\'affiche alors avec son seul libellé, jamais un carré vide.')
    L.append("  if (!name || name[0] == '\\0') return ICON_NONE;")
    L.append('  for (uint8_t i = 0; i < (uint8_t)ICON_COUNT; i++) {')
    L.append('    if (sameName(name, NAMES[i])) return (IconId)i;')
    L.append('  }')
    L.append('  return ICON_NONE;')
    L.append('}')
    L.append('')
    L.append('const char *iconName(IconId id) {')
    L.append('  if ((uint8_t)id >= (uint8_t)ICON_COUNT) return NAMES[ICON_NONE];')
    L.append('  return NAMES[(uint8_t)id];')
    L.append('}')
    L.append('')
    L.append('const uint8_t *iconAlpha24(IconId id) {')
    L.append('  if (id == ICON_NONE || (uint8_t)id >= (uint8_t)ICON_COUNT) return nullptr;')
    L.append('  return ICON_A24[(uint8_t)id - 1];')
    L.append('}')
    L.append('')
    L.append('const uint8_t *iconAlpha40(IconId id) {')
    L.append('  if (id == ICON_NONE || (uint8_t)id >= (uint8_t)ICON_COUNT) return nullptr;')
    L.append('  return ICON_A40[(uint8_t)id - 1];')
    L.append('}')
    L.append('')
    open(path, 'w').write('\n'.join(L))


# -------------------------------------------------------------------- planches
def _blit(img, ox, oy, alpha, size, zoom, fg):
    px = img.load()
    for y in range(size):
        for x in range(size):
            a = alpha[y * size + x]
            if not a:
                continue
            for dy in range(zoom):
                for dx in range(zoom):
                    X, Y = ox + x * zoom + dx, oy + y * zoom + dy
                    if 0 <= X < img.width and 0 <= Y < img.height:
                        b = px[X, Y]
                        px[X, Y] = tuple((fg[k] * a + b[k] * (255 - a)) // 255
                                         for k in range(3))


def sheet_zoom(bits, path, bg, fg, cols=7):
    """Les deux tailles côte à côte, amenées à la même dimension à l'écran."""
    Z24, Z40 = 5, 3
    cw, ch = 24 * Z24 + 8 + 40 * Z40, 24 * Z24
    pad, lab = 14, 15
    rows = (39 + cols - 1) // cols
    W = cols * (cw + pad) + pad
    H = rows * (ch + pad + lab) + pad
    img = Image.new('RGB', (W, H), bg)
    d = ImageDraw.Draw(img)
    for i, n in enumerate(NAMES[1:]):
        ox = pad + (i % cols) * (cw + pad)
        oy = pad + (i // cols) * (ch + pad + lab)
        _blit(img, ox, oy, bits[n][24], 24, Z24, fg)
        _blit(img, ox + 24 * Z24 + 8, oy, bits[n][40], 40, Z40, fg)
        d.text((ox, oy + ch + 2), '%02d %s' % (i + 1, n),
               fill=tuple((fg[k] + bg[k]) // 2 for k in range(3)))
    img.save(path)
    return path


def sheet_actual(bits, path):
    """Taille réelle, fond sombre puis fond clair. La seule planche qui dise si
    c'est vraiment lisible."""
    rows = [(24, (18, 20, 24), (232, 238, 246)),
            (40, (18, 20, 24), (232, 238, 246)),
            (24, (236, 239, 244), (22, 26, 32)),
            (40, (236, 239, 244), (22, 26, 32))]
    gap = 10
    per = 20
    W = per * (40 + gap) + gap
    nrow = (39 + per - 1) // per
    hs = [nrow * (s + gap) + gap for s, _, _ in rows]
    img = Image.new('RGB', (W, sum(hs)), (0, 0, 0))
    d = ImageDraw.Draw(img)
    y = 0
    for (size, bg, fg), h in zip(rows, hs):
        d.rectangle([0, y, W, y + h], fill=bg)
        for i, n in enumerate(NAMES[1:]):
            ox = gap + (i % per) * (40 + gap) + (40 - size) // 2
            oy = y + gap + (i // per) * (size + gap)
            _blit(img, ox, oy, bits[n][size], size, 1, fg)
        y += h
    img.save(path)
    return path


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    src = sys.argv[2] if len(sys.argv) > 2 else None
    bits = build()
    p1 = sheet_zoom(bits, os.path.join(out, 'icons-dark.png'), (18, 20, 24), (233, 238, 246))
    p2 = sheet_zoom(bits, os.path.join(out, 'icons-light.png'), (237, 240, 245), (22, 26, 32))
    p3 = sheet_actual(bits, os.path.join(out, 'icons-actual.png'))
    if src:
        emit(bits, os.path.join(src, 'icons.cpp'))
        emit_header(os.path.join(src, 'icons.h'))
        print('écrit :', src)
    for p in (p1, p2, p3):
        print('planche :', p)
    print('39 icônes — %d octets en 24x24, %d octets en 40x40, total %d'
          % (576 * 39, 1600 * 39, (576 + 1600) * 39))
