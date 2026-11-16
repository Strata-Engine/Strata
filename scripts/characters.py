#!/usr/bin/env python3
"""Write the twelve characters as MagicaVoxel files, viewer/assets/characters/<id>.vox.

Voxels of 1/32 m (the file says so: the root transform's `_strata_voxel`).
Each character is a recipe: a body of the shared fifteen parts in its own
proportions, modelled with round shapes (tapered limbs, ellipsoid heads,
rounded boxes), then a face, kit, trim and extras. The files follow the
convention Strata.Character reads (lib/strata/character.march): one model
per part, named, placed by its transform; file axes x (the character's
left), y (forward), z (up); feet on z 0. Only each part's surface voxels are
written (its inside is never seen).

Palette: 1-15 a character's own colours, 241-244 trim (241 the full shade,
242 darker, 243 glowing, 244 glowing and darker), 250-253 eyes and lights.

A file someone has edited by hand is not overwritten: generated.txt keeps
the SHA-256 of what this script last wrote, and a file that no longer
matches is skipped with a message, unless --force.

  python3 scripts/characters.py [--force] [--only id,id]
"""
import hashlib
import math
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "viewer", "assets", "characters")
MANIFEST = os.path.join(OUT, "generated.txt")
ROSTER = ["corvin", "vesper", "rook", "unit7", "mother", "skrell", "gorrum", "nyx", "sabine", "kade", "hex", "torvald"]
VOXEL = 1.0 / 32.0

PARTS = ["pelvis", "spine", "head", "arm_l_upper", "arm_l_fore", "arm_l_hand", "arm_r_upper", "arm_r_fore", "arm_r_hand",
         "leg_l_thigh", "leg_l_shin", "leg_l_foot", "leg_r_thigh", "leg_r_shin", "leg_r_foot"]

SKIN, MAIN, SECOND, DARK, METAL, LEATHER, HAIR, ACCENT, PALE, FUR, LIP, WHITE, SHADE = range(1, 14)
TRIM, TRIM_DARK, TRIM_GLOW, TRIM_GLOW_DARK = 241, 242, 243, 244
EYE, VISOR, LIGHT, IRIS = 250, 251, 252, 253


class Figure:
    """A character in global voxels: part name -> {(x, y, z): colour}."""

    def __init__(self):
        self.parts = {}
        self.order = []

    def part(self, name):
        if name not in self.parts:
            self.parts[name] = {}
            self.order.append(name)
        return self.parts[name]

    def put(self, name, x, y, z, c):
        p = self.part(name)
        if c:
            p[(x, y, z)] = c
        else:
            p.pop((x, y, z), None)

    def fill(self, name, test, x0, y0, z0, x1, y1, z1, c):
        for x in range(x0, x1):
            for y in range(y0, y1):
                for z in range(z0, z1):
                    if test(x + 0.5, y + 0.5, z + 0.5):
                        self.put(name, x, y, z, c)

    def box(self, name, x0, y0, z0, x1, y1, z1, c):
        self.fill(name, lambda x, y, z: True, x0, y0, z0, x1, y1, z1, c)

    def rbox(self, name, x0, y0, z0, x1, y1, z1, c, r=1.5):
        """A box with its edges rounded to radius r."""
        def t(x, y, z):
            dx = max(x0 + r - x, 0, x - (x1 - r)); dy = max(y0 + r - y, 0, y - (y1 - r)); dz = max(z0 + r - z, 0, z - (z1 - r))
            return dx * dx + dy * dy + dz * dz <= r * r
        self.fill(name, t, x0, y0, z0, x1, y1, z1, c)

    def ball(self, name, cx, cy, cz, rx, ry, rz, c, keep=None):
        """An ellipsoid; keep(x, y, z) limits it (a half, a slice)."""
        def t(x, y, z):
            inside = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 + ((z - cz) / rz) ** 2 <= 1.0
            return inside and (keep is None or keep(x, y, z))
        self.fill(name, t, int(math.floor(cx - rx)), int(math.floor(cy - ry)), int(math.floor(cz - rz)),
                  int(math.ceil(cx + rx)), int(math.ceil(cy + ry)), int(math.ceil(cz + rz)), c)

    def tube(self, name, cx, cy, z0, z1, rx0, ry0, rx1, ry1, c, bulge=0.0, dy=0.0):
        """An upright tube of elliptical section from z0 (radii rx0, ry0) to z1 (rx1, ry1), swelling by bulge in the middle, its centre drifting dy forward."""
        h = max(z1 - z0, 1)
        for z in range(z0, z1):
            t = (z + 0.5 - z0) / h
            s = bulge * math.sin(math.pi * t)
            rx = rx0 + (rx1 - rx0) * t + s
            ry = ry0 + (ry1 - ry0) * t + s
            yc = cy + dy * t
            for x in range(int(math.floor(cx - rx)), int(math.ceil(cx + rx))):
                for y in range(int(math.floor(yc - ry)), int(math.ceil(yc + ry))):
                    if ((x + 0.5 - cx) / rx) ** 2 + ((y + 0.5 - yc) / ry) ** 2 <= 1.0:
                        self.put(name, x, y, z, c)

    def paint(self, name, test, c):
        p = self.parts[name]
        for k in list(p):
            if test(*k):
                p[k] = c

    def front(self, name, x, z):
        """The forward-most voxel of a part in column (x, z), or None."""
        ys = [k[1] for k in self.parts[name] if k[0] == x and k[2] == z]
        return max(ys) if ys else None

    def back(self, name, x, z):
        ys = [k[1] for k in self.parts[name] if k[0] == x and k[2] == z]
        return min(ys) if ys else None

    def stamp(self, name, x, z, c, out=0):
        """Colour the front surface of a part at (x, z), or raise it by out voxels."""
        y = self.front(name, x, z)
        if y is None:
            return
        for k in range(out + 1):
            self.put(name, x, y + k, z, c)

    def bounds(self, name):
        vs = self.parts[name]
        xs = [k[0] for k in vs]; ys = [k[1] for k in vs]; zs = [k[2] for k in vs]
        return min(xs), min(ys), min(zs), max(xs) + 1, max(ys) + 1, max(zs) + 1

    def mirror_x(self, name, src):
        for (x, y, z), c in list(self.parts[src].items()):
            self.put(name, -1 - x, y, z, c)


class B(dict):
    __getattr__ = dict.__getitem__


def body(f, p):
    """The fifteen parts, centred on x 0 (the character's left is +x), facing +y, feet on z 0. Returns where things are.

    p (voxels of 1/32 m): foot (length), ankle, knee, thigh_r (radii), shin, thigh (lengths); hip_w, hip_d, hip_h (pelvis);
    waist, chest, depth, torso (torso radii and height); neck; head (rx, ry, rz); arm_r, elbow, wrist, upper, fore, hand;
    and colours c_* for each part."""
    g = p.get
    foot_h = 3
    # realistic proportions: a head about 1/7.5 of the height, slimmer hands, shorter feet
    hs = p.get("head_scale", 0.8)
    p["head"] = tuple(r * hs for r in p["head"])
    p["hand"] = max(3, int(round(p["hand"] * 0.85)))
    p["foot"] = max(7, int(round(p["foot"] * 0.85)))
    if "height" in p:
        # scale the legs and torso so the head's top is at height (metres)
        want = p["height"] / VOXEL
        rest = foot_h + p["neck"] + 2 * p["head"][2]
        k = (want - rest) / float(p["shin"] + p["thigh"] + p["hip_h"] + p["torso"])
        for key in ("shin", "thigh", "hip_h", "torso", "upper", "fore"):
            p[key] = int(round(p[key] * k))
    shin, thigh = p["shin"], p["thigh"]
    z_shin = foot_h
    z_thigh = z_shin + shin
    z_pelvis = z_thigh + thigh
    z_spine = z_pelvis + p["hip_h"]
    z_neck = z_spine + p["torso"]
    z_head = z_neck + p["neck"]
    hw, hd = p["hip_w"], p["hip_d"]
    f.rbox("pelvis", -hw // 2, -hd // 2, z_pelvis, hw - hw // 2, hd - hd // 2, z_spine, p["c_pelvis"], r=2.0)
    lean = g("lean", 0)
    f.tube("spine", 0, 0.0, z_spine, z_neck, p["waist"], p["depth"] * 0.85, p["chest"], p["depth"], p["c_spine"], bulge=g("belly", 0.0), dy=lean * 0.5)
    # shoulders: round off the torso's top
    f.ball("spine", 0, lean * 0.5, z_neck - 2, p["chest"], p["depth"], 4, p["c_spine"], keep=lambda x, y, z: z < z_neck)
    f.tube("spine", 0, lean * 0.5, z_neck, z_head, p["neck_r"], p["neck_r"], p["neck_r"], p["neck_r"], g("c_neck", SKIN))
    hx, hy, hz = p["head"]
    hc = (0, lean * 0.5 + g("head_fwd", 0), z_head + hz)
    f.ball("head", hc[0], hc[1], hc[2], hx, hy, hz, SKIN)
    # the jaw: a little narrower, a little forward
    f.ball("head", hc[0], hc[1] + 1, z_head + hz * 0.7, hx * 0.82, hy * 0.92, hz * 0.7, SKIN)
    # legs: thigh, shin with a calf, a shoe
    lx = hw / 2.0 - p["thigh_r"] + g("leg_out", 0)
    for side, cx in (("l", lx), ("r", -lx)):
        f.tube("leg_%s_thigh" % side, cx, 0, z_thigh, z_pelvis, p["knee"], p["knee"], p["thigh_r"], p["thigh_r"], p["c_leg"])
        f.tube("leg_%s_shin" % side, cx, 0, z_shin, z_thigh, p["ankle"], p["ankle"], p["knee"], p["knee"], p["c_shin"], bulge=g("calf", 1.0))
        a = p["ankle"] + 1.0
        f.rbox("leg_%s_foot" % side, int(math.floor(cx - a)), int(math.floor(-a)), 0, int(math.ceil(cx + a)), int(math.ceil(-a)) + p["foot"], foot_h, p["c_foot"], r=1.5)
    # arms hang beside the chest
    ax = p["chest"] + p["arm_r"] - g("arm_in", 0.5)
    top = z_neck - g("shoulder_drop", 2)
    for side, cx in (("l", ax), ("r", -ax)):
        f.tube("arm_%s_upper" % side, cx, 0, top - p["upper"], top, p["elbow"], p["elbow"], p["arm_r"], p["arm_r"], p["c_upper"])
        f.ball("arm_%s_upper" % side, cx, 0, top - 1, p["arm_r"], p["arm_r"], 2.5, p["c_upper"], keep=lambda x, y, z: z < top)
        f.tube("arm_%s_fore" % side, cx, 0, top - p["upper"] - p["fore"], top - p["upper"], p["wrist"], p["wrist"], p["elbow"], p["elbow"], p["c_fore"], bulge=0.5)
        hz0 = top - p["upper"] - p["fore"] - p["hand"]
        w = p["wrist"] + 0.35
        f.rbox("arm_%s_hand" % side, int(math.floor(cx - w)), int(math.floor(-w - 0.5)), hz0, int(math.ceil(cx + w)), int(math.ceil(w + 0.5)), hz0 + p["hand"], g("c_hand", SKIN), r=1.5)
    return B(z_pelvis=z_pelvis, z_spine=z_spine, z_neck=z_neck, z_head=z_head, hc=hc, hr=p["head"], top=top, ax=ax,
             upper=p["upper"], fore=p["fore"], hand=p["hand"], z_thigh=z_thigh, z_shin=z_shin, chest=p["chest"], depth=p["depth"],
             hip_w=hw, hip_d=hd, lx=lx, lean=lean, waist=p["waist"])


def face(f, b, eye=WHITE, iris=IRIS, brow=None, nose=SKIN, mouth=LIP, spread=None, ez=None, ears=True):
    """Eyes (white and a dark iris), brows, a nose and a mouth on the head's front."""
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    ez = int(round(cz + (ez if ez is not None else 0)))
    sp = spread if spread is not None else max(2, int(rx * 0.4))
    for sgn in (1, -1):
        x0 = int(round(cx + sgn * sp)) - (1 if sgn < 0 else 0)
        for dx in (0, 1):
            x = x0 + (dx if sgn > 0 else -dx)
            f.stamp("head", x, ez, iris if dx == 0 else eye)
            if eye != iris:
                f.stamp("head", x, ez + 1, eye)
        # a brow ridge: the row over the eyes stands out, so the eyes sit in its shadow
    for x in range(int(cx - sp - 2), int(cx + sp + 2)):
        f.stamp("head", x, ez + 2, brow or SKIN, out=1)
    if nose:
        for dz in (0, -1, -2):
            f.stamp("head", 0, ez + dz, nose, out=1)
            f.stamp("head", -1, ez + dz, nose, out=1)
        f.stamp("head", 0, ez - 2, nose, out=2)
        f.stamp("head", -1, ez - 2, nose, out=2)
    if mouth:
        for x in range(-2, 2):
            f.stamp("head", x, ez - 4, mouth)
    if ears:
        for sgn in (1, -1):
            x = int(round(cx + sgn * (rx + 0.2))) - (1 if sgn < 0 else 0)
            for dz in (-1, 0, 1):
                f.put("head", x, int(cy), ez + dz, SKIN)


def hair(f, b, c, top=1.5, back=True, fringe=True):
    """A shell of hair over the crown and the back of the head."""
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    f.ball("head", cx, cy - 0.5, cz + 0.5, rx + 0.9, ry + 0.9, rz + top,
           c, keep=lambda x, y, z: z > cz + rz * 0.35 - (0 if fringe else 2) or (back and y < cy - ry * 0.25 and z > cz - rz * 0.5))


def helmet(f, b, c, low=0.1, thick=1.2, open_face=True):
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    f.ball("head", cx, cy, cz, rx + thick, ry + thick, rz + thick, c,
           keep=lambda x, y, z: z > cz - rz * low and (not open_face or y < cy + ry * 0.35 or z > cz + rz * 0.45))


def band(f, name, z0, z1, c):
    f.paint(name, lambda x, y, z: z0 <= z < z1, c)


# ── the twelve ──────────────────────────────────────────────────────────────

def std(**kw):
    p = dict(foot=11, ankle=2.2, knee=3.0, thigh_r=4.2, shin=15, thigh=15, hip_w=14, hip_d=9, hip_h=6,
             waist=6.0, chest=8.0, depth=4.5, torso=17, neck=3, neck_r=2.2, head=(4.6, 5.0, 5.4),
             arm_r=2.5, elbow=2.1, wrist=1.6, upper=11, fore=10, hand=5,
             c_pelvis=MAIN, c_spine=MAIN, c_leg=SECOND, c_shin=SECOND, c_foot=DARK, c_upper=MAIN, c_fore=MAIN)
    p.update(kw)
    return p


def corvin():
    """Gladiator: a scarred veteran legionnaire, broad, in worn segmented plate, open helmet, short cloak, greaves."""
    f = Figure()
    b = body(f, std(height=1.84, foot=13, ankle=2.8, knee=3.8, thigh_r=5.2, shin=16, thigh=16, hip_w=18, hip_d=11, waist=7.8, chest=11.0,
                    depth=6.2, torso=19, neck_r=3.2, head=(5.0, 5.4, 5.6), arm_r=3.5, elbow=3.0, wrist=2.2, upper=12, fore=12, hand=6,
                    c_pelvis=SECOND, c_spine=MAIN, c_leg=SECOND, c_shin=METAL, c_foot=LEATHER, c_upper=SECOND, c_fore=SKIN))
    face(f, b, brow=SKIN, ez=0.5)
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    # a scar across the left brow and cheek; stubble on the jaw
    for i in range(5):
        f.stamp("head", int(cx + 1 + i * 0.5), int(cz) + 2 - i, LIP)
    f.paint("head", lambda x, y, z: z < cz - 1.5 and y > cy and (x + z) % 2 == 0, HAIR)
    # the helmet: a bowl with a neck guard behind and hinged cheek guards, a brow band
    helmet(f, b, METAL, low=0.3, thick=1.1)
    f.ball("head", cx, cy - 1, cz - 1, rx + 1.8, ry + 0.8, 2.0, METAL, keep=lambda x, y, z: y < cy - ry * 0.4)
    for sgn in (1, -1):
        f.fill("head", lambda x, y, z, sgn=sgn: (x - cx) * sgn > rx - 1.2 and y > cy - 1 and y < cy + ry * 0.7 and z > cz - rz * 0.75 and z < cz + 1,
               int(cx - rx - 2), int(cy - 2), int(cz - rz), int(cx + rx + 2), int(cy + ry), int(cz + 2), METAL)
    f.paint("head", lambda x, y, z: abs(z - (cz + rz * 0.3 + 0.8)) < 0.6 and (x * x + (y - cy) ** 2) > (rx - 0.5) ** 2, DARK)
    # segmented plate over the torso: bands of metal with dark gaps, a padded tunic showing below
    f.tube("spine", 0, 0.3, b.z_spine + 4, b.z_neck - 1, b.waist + 1.0, b.depth + 0.6, b.chest + 0.8, b.depth + 0.9, METAL)
    f.paint("spine", lambda x, y, z: z >= b.z_spine + 4 and (z - b.z_spine) % 3 == 0 and (x * x / (b.chest + 1) ** 2 + y * y / (b.depth + 1) ** 2) > 0.7, DARK)
    f.ball("spine", 0, 0, b.z_neck - 1, b.chest + 1.2, b.depth + 1.0, 3.0, METAL, keep=lambda x, y, z: z >= b.z_neck - 2)
    f.paint("pelvis", lambda x, y, z: z < b.z_pelvis + 2, TRIM)
    f.paint("pelvis", lambda x, y, z: (x % 3 == 0) and z < b.z_spine - 1, LEATHER)
    band(f, "pelvis", b.z_spine - 2, b.z_spine, LEATHER)
    # layered plates over the shoulders
    for s_, sgn in (("l", 1), ("r", -1)):
        for i, z in enumerate((b.top + 1, b.top - 2)):
            f.ball("extra:arm_%s_upper:plate_%d" % (s_, i), sgn * b.ax, 0, z, 4.8 - i * 0.4, 4.4, 2.2, METAL, keep=lambda x, y, zz, z=z: zz >= z - 1)
    # greaves already metal on the shins; leather wraps on the forearms
    for s_ in ("l", "r"):
        lo = b.top - b.upper - b.fore
        band(f, "arm_%s_fore" % s_, lo, lo + 6, LEATHER)
    # the short cloak from the shoulders behind, its edge in trim
    for z in range(b.z_spine - 3, b.z_neck + 1):
        w = b.chest + 0.5 + (b.z_neck - z) * 0.1
        for x in range(int(-w), int(w) + 1):
            edge = z < b.z_spine - 1 or abs(x) >= int(w) - 0
            f.put("extra:spine:cloak", x, -int(b.depth) - 2, z, TRIM if edge else TRIM_DARK)
    return f, {SKIN: (176, 124, 96), MAIN: (120, 60, 46), SECOND: (110, 80, 60), METAL: (128, 124, 116), LEATHER: (78, 54, 36),
               HAIR: (90, 70, 60), DARK: (34, 30, 28), WHITE: (226, 222, 212), IRIS: (70, 60, 50), LIP: (140, 84, 70),
               TRIM: (150, 40, 34), TRIM_DARK: (100, 30, 26)}, {}


def vesper():
    """Merc: lean, long coat tails, goggles."""
    f = Figure()
    b = body(f, std(height=1.78, thigh_r=3.6, knee=2.6, ankle=2.0, shin=16, thigh=15, hip_w=12, hip_d=8, waist=5.2, chest=7.0, depth=4.0,
                    head=(4.3, 4.8, 5.4), arm_r=2.2, elbow=1.9, wrist=1.5, upper=11, fore=10,
                    c_pelvis=MAIN, c_spine=MAIN, c_leg=DARK, c_shin=DARK, c_foot=LEATHER, c_upper=MAIN, c_fore=MAIN, c_hand=LEATHER))
    face(f, b, brow=HAIR, ez=0)
    hair(f, b, HAIR, top=1.2)
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    # goggles on the brow: a strap round the head, two lenses
    f.ball("head", cx, cy, cz + 2.5, rx + 0.8, ry + 0.8, 1.2, DARK, keep=lambda x, y, z: True)
    for sgn in (1, -1):
        lx = int(round(cx + sgn * 2)) - (1 if sgn < 0 else 0)
        for dx in (-1, 0, 1):
            for dz in (2, 3):
                f.stamp("head", lx + dx, int(cz) + dz, VISOR if dx == 0 else METAL, out=1)
    # high collar of trim, coat buttons, a sleeve band
    f.fill("spine", lambda x, y, z: (x * x) / 30.0 + (y * y) / 14.0 <= 1.2, -7, -5, b.z_neck - 2, 7, 6, b.z_neck + 3, TRIM)
    for z in range(b.z_spine + 3, b.z_neck - 3, 4):
        f.stamp("spine", 0, z, METAL)
    band(f, "arm_l_upper", b.top - 6, b.top - 3, TRIM)
    band(f, "spine", b.z_spine, b.z_spine + 2, LEATHER)
    # coat tails: two panels hanging from the belt behind, lined with trim
    for k, (x0, x1) in enumerate(((-7, 0), (0, 7))):
        name = "extra:pelvis:tail_%d" % k
        for z in range(b.z_pelvis - 20, b.z_pelvis + 3):
            flare = (b.z_pelvis + 3 - z) * 0.12
            for x in range(int(x0 - (flare if x0 < 0 else 0)), int(x1 + (flare if x1 > 0 else 0))):
                f.put(name, x, -b.hip_d // 2 - 1, z, MAIN)
                f.put(name, x, -b.hip_d // 2, z, TRIM_DARK)
    return f, {SKIN: (204, 160, 128), MAIN: (58, 66, 54), DARK: (34, 34, 38), LEATHER: (80, 52, 32), HAIR: (30, 22, 18),
               METAL: (90, 90, 96), VISOR: (255, 170, 60), WHITE: (236, 232, 226), IRIS: (40, 70, 60), LIP: (170, 110, 96),
               TRIM: (200, 200, 190), TRIM_DARK: (120, 120, 112)}, {VISOR: 0.8}


def rook():
    """Merc: squat, armoured, ammo backpack, short antenna."""
    f = Figure()
    b = body(f, std(height=1.80, foot=13, ankle=3.0, knee=4.0, thigh_r=5.0, shin=12, thigh=12, hip_w=19, hip_d=12, waist=8.0, chest=10.5, depth=6.5,
                    torso=16, head=(5.0, 5.3, 5.4), arm_r=3.5, elbow=3.0, wrist=2.2, upper=10, fore=10, neck_r=3.0,
                    c_pelvis=DARK, c_spine=MAIN, c_leg=SECOND, c_shin=MAIN, c_foot=DARK, c_upper=MAIN, c_fore=SECOND, c_hand=DARK))
    face(f, b, ez=-1, brow=None)
    helmet(f, b, MAIN, low=0.15, thick=1.5)
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    # a visor band across the helmet's brow
    for x in range(-5, 5):
        f.stamp("head", x, int(cz) + 2, VISOR, out=0)
        f.stamp("head", x, int(cz) + 3, VISOR, out=0)
    # the chest plate: raised, with two stripes of trim
    f.fill("spine", lambda x, y, z: y > 2 and abs(x) < 8, -9, 2, b.z_spine + 3, 9, 8, b.z_neck - 2, MAIN)
    f.tube("spine", 0, 1.0, b.z_spine + 3, b.z_neck - 3, 8.6, 6.0, 9.8, 6.8, MAIN)
    for z in (b.z_spine + 6, b.z_spine + 10):
        f.paint("spine", lambda x, y, zz, z=z: zz in (z, z + 1) and y > 2, TRIM)
    # pouches on the belt, knee pads
    for x0 in (-8, 4):
        f.rbox("pelvis", x0, b.hip_d // 2 - 1, b.z_pelvis + 1, x0 + 4, b.hip_d // 2 + 2, b.z_pelvis + 5, SECOND, r=1.0)
    for s in ("l", "r"):
        x0, y0, z0, x1, y1, z1 = f.bounds("leg_%s_shin" % s)
        f.ball("leg_%s_shin" % s, (x0 + x1) / 2.0, y1 - 1.5, z1 - 3, 3.6, 2.2, 3.0, DARK)
    # the pack and its drums; the antenna on the pack
    by = -b.depth - 6
    f.rbox("extra:spine:pack", -8, int(by), b.z_spine + 1, 8, int(-b.depth) + 1, b.z_neck - 1, SECOND, r=2.0)
    for x in (-5, 5):
        f.tube("extra:spine:pack", x, by - 1.5, b.z_spine + 2, b.z_spine + 9, 3.0, 2.0, 3.0, 2.0, METAL)
    band(f, "extra:spine:pack", b.z_neck - 4, b.z_neck - 2, TRIM_DARK)
    for z in range(b.z_neck - 1, b.z_neck + 14):
        f.put("extra:pack:antenna", 6, int(by) + 2, z, DARK)
    f.ball("extra:pack:antenna", 6.5, by + 2.5, b.z_neck + 14, 1.2, 1.2, 1.2, TRIM_GLOW)
    return f, {SKIN: (150, 104, 74), MAIN: (96, 104, 70), SECOND: (70, 64, 50), DARK: (36, 38, 34), METAL: (140, 120, 70),
               WHITE: (230, 226, 216), IRIS: (40, 30, 24), LIP: (120, 80, 60), VISOR: (120, 220, 255),
               TRIM: (230, 230, 220), TRIM_DARK: (150, 150, 140), TRIM_GLOW: (255, 255, 255)}, {VISOR: 0.6, TRIM_GLOW: 1.0}


def unit7():
    """Cyborg: thin, chrome, glowing visor, spine cables."""
    f = Figure()
    b = body(f, std(height=1.80, thigh_r=3.0, knee=2.2, ankle=1.6, shin=18, thigh=17, hip_w=11, hip_d=8, waist=4.4, chest=6.2, depth=3.8,
                    head=(3.8, 4.6, 5.2), arm_r=1.9, elbow=1.5, wrist=1.3, upper=13, fore=12, leg_out=0.5,
                    c_pelvis=DARK, c_spine=METAL, c_leg=METAL, c_shin=METAL, c_foot=DARK, c_upper=METAL, c_fore=METAL, c_hand=DARK,
                    c_neck=DARK))
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    f.paint("head", lambda x, y, z: True, METAL)
    # the visor: a glowing slit wrapping the front of the head; ear housings
    f.paint("head", lambda x, y, z: abs(z - (cz + 1)) < 1.0 and y > cy, TRIM_GLOW)
    for sgn in (1, -1):
        f.ball("head", cx + sgn * rx, cy, cz, 1.4, 2.4, 2.4, DARK)
    # seams: dark lines down the chest, joint lights at elbows and knees, a chest light
    f.paint("spine", lambda x, y, z: abs(x + 0.5) < 0.6 and y > 0, DARK)
    f.paint("spine", lambda x, y, z: (z - b.z_spine) % 5 == 0 and y > 0, DARK)
    for s in ("l", "r"):
        band(f, "arm_%s_fore" % s, b.top - b.upper - 2, b.top - b.upper, TRIM_GLOW)
        x0, y0, z0, x1, y1, z1 = f.bounds("leg_%s_shin" % s)
        band(f, "leg_%s_shin" % s, z1 - 2, z1, TRIM_GLOW)
    f.ball("spine", 0, b.depth - 0.3, b.z_neck - 6, 1.6, 1.0, 1.6, LIGHT)
    # cables down the back, lit at intervals
    for x in (-3, 2):
        for z in range(b.z_spine - 1, b.z_neck + 2):
            c = TRIM_GLOW_DARK if (z - b.z_spine) % 5 == 0 else DARK
            f.put("extra:spine:cables", x, -int(b.depth) - 1, z, c)
            f.put("extra:spine:cables", x + 1, -int(b.depth) - 1, z, c)
    return f, {METAL: (196, 200, 210), DARK: (40, 44, 52), SKIN: (196, 200, 210), LIGHT: (255, 255, 255),
               TRIM_GLOW: (230, 230, 255), TRIM_GLOW_DARK: (140, 140, 170)}, {LIGHT: 1.0, TRIM_GLOW: 1.0, TRIM_GLOW_DARK: 0.8}


def mother():
    """Cyborg: tall, heavy, a power pack on her back."""
    f = Figure()
    b = body(f, std(height=1.86, foot=14, ankle=3.0, knee=4.2, thigh_r=5.6, shin=17, thigh=17, hip_w=20, hip_d=13, waist=9.0, chest=12.0, depth=7.0,
                    torso=18, head=(4.4, 4.8, 4.8), arm_r=3.8, elbow=3.2, wrist=2.6, upper=12, fore=12, hand=6, neck_r=3.0,
                    c_pelvis=DARK, c_spine=MAIN, c_leg=MAIN, c_shin=MAIN, c_foot=DARK, c_upper=MAIN, c_fore=METAL, c_hand=DARK, c_neck=DARK))
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    f.paint("head", lambda x, y, z: True, MAIN)
    f.paint("head", lambda x, y, z: y > cy + 1 and abs(z - cz) < 1.6 and abs(x) < rx - 0.5, VISOR)
    # a collar of armour round the neck
    f.tube("spine", 0, 0, b.z_neck - 1, b.z_neck + 4, 7.5, 6.0, 6.5, 5.5, METAL)
    f.tube("spine", 0, 0, b.z_neck - 1, b.z_neck + 4, 4.5, 4.0, 4.5, 4.0, 0)
    f.tube("spine", 0, 0, b.z_neck - 1, b.z_neck + 3, 3.0, 3.0, 3.0, 3.0, DARK)
    # the chest core and hazard stripes on the thighs
    f.ball("spine", 0, b.depth - 0.5, b.z_spine + 9, 3.0, 1.6, 3.0, METAL)
    f.ball("spine", 0, b.depth + 0.5, b.z_spine + 9, 1.8, 1.0, 1.8, TRIM_GLOW)
    for s in ("l", "r"):
        f.paint("leg_%s_thigh" % s, lambda x, y, z: (z + x) % 6 < 2 and y > 1 and z > b.z_thigh + 4 and z < b.z_thigh + 9, ACCENT)
    # armour plates on the forearms
    for s in ("l", "r"):
        x0, y0, z0, x1, y1, z1 = f.bounds("arm_%s_fore" % s)
        f.rbox("arm_%s_fore" % s, x0 - 1, y0 - 1, z0 + 3, x1 + 1, y1 + 1, z1 - 2, METAL, r=1.0)
    # the power pack on her back: an armoured reactor, vents glowing faintly, heavy cables over both shoulders
    py0 = -int(b.depth) - 7
    f.rbox("extra:spine:pack", -7, py0, b.z_spine + 3, 7, -int(b.depth) + 1, b.z_neck + 3, METAL, r=2.0)
    for z in range(b.z_spine + 6, b.z_neck, 3):
        f.paint("extra:spine:pack", lambda x, y, zz, z=z: zz == z and y == py0 and abs(x) < 5, TRIM_GLOW_DARK)
    f.paint("extra:spine:pack", lambda x, y, z: z == b.z_neck + 2 and abs(x) < 4, DARK)
    for sgn in (1, -1):
        for i in range(16):
            t = i / 15.0
            x = sgn * (5 + (b.ax - 5) * t)
            y = py0 + 3 + (-py0 - 3) * t * 0.8
            z = b.z_neck + 3 + 3 * math.sin(math.pi * t) - 4 * t
            f.ball("extra:spine:pack", x, y, z, 1.3, 1.3, 1.3, DARK)
    return f, {MAIN: (70, 74, 84), METAL: (130, 134, 140), DARK: (28, 30, 34), ACCENT: (230, 180, 30), SKIN: (70, 74, 84),
               VISOR: (255, 60, 40), TRIM_GLOW: (255, 250, 230), TRIM_GLOW_DARK: (200, 190, 160)}, {VISOR: 0.9, TRIM_GLOW: 1.0, TRIM_GLOW_DARK: 0.5}


def skrell():
    """Alien: tall, thin, big skull, long tail."""
    f = Figure()
    b = body(f, std(height=1.78, foot=12, thigh_r=3.0, knee=2.2, ankle=1.5, shin=19, thigh=18, hip_w=11, hip_d=7, hip_h=5, waist=4.0, chest=6.0,
                    depth=3.6, torso=18, neck=5, neck_r=1.8, head=(5.0, 6.2, 6.4), head_fwd=1.0,
                    arm_r=1.8, elbow=1.4, wrist=1.2, upper=15, fore=14, hand=7, leg_out=0.5,
                    c_pelvis=SECOND, c_spine=SKIN, c_leg=SKIN, c_shin=SKIN, c_foot=SECOND, c_upper=SKIN, c_fore=SKIN))
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    # the skull swells up and back
    f.ball("head", cx, cy - 3.0, cz + 3.5, rx * 0.95, ry * 0.9, rz * 0.85, SKIN)
    # big dark almond eyes with a glow, a slit mouth, no nose
    for sgn in (1, -1):
        for dx in range(0, 3):
            for dz in (0, 1):
                x = int(round(cx + sgn * (1.5 + dx))) - (1 if sgn < 0 else 0)
                f.stamp("head", x, int(cz) + dz - (1 if dx == 2 else 0), EYE if dx == 1 else DARK)
    for x in range(-2, 2):
        f.stamp("head", x, int(cz) - 4, DARK)
    # pale belly, glowing gill stripes on the neck and ribs
    f.paint("spine", lambda x, y, z: y > 1.5 and abs(x) < 4, PALE)
    for z in (b.z_spine + 6, b.z_spine + 9, b.z_spine + 12, b.z_neck + 1, b.z_neck + 3):
        f.paint("spine", lambda x, y, zz, z=z: zz == z and abs(x) > 2.5 and y > -1, TRIM_GLOW)
    # spots down the back
    f.paint("spine", lambda x, y, z: y < -2 and (x * 3 + z * 5) % 11 == 0, SECOND)
    # the tail: a tapering curve down and back, banded
    for i in range(30):
        t = i / 29.0
        y = -b.hip_d / 2.0 - 1 - i * 0.9
        z = b.z_pelvis + 1 - 14 * t * t
        r = 2.6 - 1.9 * t
        f.ball("extra:pelvis:tail", 0, y, z, r, 1.2, r, TRIM_GLOW_DARK if i % 6 == 3 else SKIN)
    return f, {SKIN: (90, 150, 110), SECOND: (50, 80, 70), PALE: (190, 210, 170), DARK: (20, 30, 26), EYE: (250, 240, 120),
               TRIM_GLOW: (230, 255, 240), TRIM_GLOW_DARK: (150, 175, 160)}, {EYE: 0.9, TRIM_GLOW: 1.0, TRIM_GLOW_DARK: 0.6}


def gorrum():
    """Alien: wide hunchback, short legs, spined back ridge."""
    f = Figure()
    b = body(f, std(height=1.83, foot=14, ankle=3.0, knee=4.0, thigh_r=5.5, shin=10, thigh=10, hip_w=22, hip_d=14, waist=10.0, chest=13.0, depth=8.0,
                    torso=18, neck=1, neck_r=4.0, head=(5.4, 5.4, 4.6), head_fwd=4.0, lean=4, belly=1.0,
                    arm_r=4.2, elbow=3.6, wrist=3.0, upper=13, fore=14, hand=7, shoulder_drop=4,
                    c_pelvis=SECOND, c_spine=SKIN, c_leg=SKIN, c_shin=SKIN, c_foot=DARK, c_upper=SKIN, c_fore=SKIN, c_hand=DARK))
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    face(f, b, eye=EYE, iris=EYE, brow=SECOND, nose=None, mouth=DARK, spread=3, ez=1, ears=False)
    # tusks from the lower jaw, a heavy brow ridge
    for sgn in (1, -1):
        x = int(round(cx + sgn * 3)) - (1 if sgn < 0 else 0)
        y = f.front("head", x, int(cz) - 3)
        if y is not None:
            for k in range(3):
                f.put("head", x, y + 1, int(cz) - 3 + k, PALE)
    f.paint("head", lambda x, y, z: y > cy + 2 and abs(z - cz - 3) < 1, SECOND)
    f.paint("spine", lambda x, y, z: y > 3 and abs(x) < 7 and z < b.z_neck - 4, PALE)
    # the hump and its spines, each tipped with trim
    f.ball("extra:spine:hump", 0, -b.depth + 1, b.z_neck - 4, 9.5, 6.0, 9.0, SKIN, keep=lambda x, y, z: y < -b.depth + 3)
    for i, z in enumerate(range(b.z_neck - 12, b.z_neck + 4, 3)):
        yb = f.back("extra:spine:hump", 0, z)
        if yb is None:
            continue
        for k in range(4):
            for x in (-1, 0):
                f.put("extra:spine:hump", x, yb - 1 - k, z + (k // 2), TRIM_GLOW if k == 3 else SECOND)
    # wrist bones
    for s in ("l", "r"):
        lo = b.top - b.upper - b.fore
        band(f, "arm_%s_fore" % s, lo, lo + 2, PALE)
    return f, {SKIN: (128, 96, 120), SECOND: (80, 56, 76), PALE: (200, 180, 150), DARK: (40, 30, 36), EYE: (255, 120, 40),
               TRIM_GLOW: (255, 255, 230)}, {EYE: 0.8, TRIM_GLOW: 0.9}


def nyx():
    """Alien: a lean hunter, the smallest; dark chitin over grey skin, a swept crest, mandibles, folded forearm blades."""
    f = Figure()
    b = body(f, std(height=1.74, foot=10, ankle=1.6, knee=2.3, thigh_r=3.2, shin=15, thigh=13, hip_w=10, hip_d=7, hip_h=5, waist=3.8, chest=5.6,
                    depth=3.4, torso=14, neck=3, neck_r=1.6, head=(3.8, 5.6, 4.4), head_scale=0.9, head_fwd=0.5,
                    arm_r=1.8, elbow=1.5, wrist=1.2, upper=11, fore=11, hand=6,
                    c_pelvis=MAIN, c_spine=SKIN, c_leg=SKIN, c_shin=MAIN, c_foot=MAIN, c_upper=SKIN, c_fore=MAIN, c_hand=SKIN))
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    # a narrow skull, the crest swept back over it
    f.paint("head", lambda x, y, z: z > cz, MAIN)
    for i in range(12):
        t = i / 11.0
        f.ball("head", cx, cy + ry * 0.4 - i * 0.9, cz + rz * 0.8 + 2.5 * math.sin(math.pi * t * 0.8), 0.9, 1.0, 1.4 + 1.2 * (1 - t), MAIN)
    f.paint("head", lambda x, y, z: z > cz + rz + 1.8 and abs(x) < 1, TRIM_GLOW_DARK)
    # small deep-set eyes that glow, mandibles at the jaw
    for sgn in (1, -1):
        x = int(round(cx + sgn * 1.6)) - (1 if sgn < 0 else 0)
        f.stamp("head", x, int(cz) + 1, EYE)
        f.stamp("head", x + sgn, int(cz) + 1, EYE)
        for k in range(4):
            f.put("head", int(round(cx + sgn * (1.5 + 0.3 * k))) - (1 if sgn < 0 else 0), int(cy + ry * 0.7 + k * 0.6), int(cz) - 3 - (k // 2), MAIN)
    # chitin plates over chest, back and limbs, seams between them glowing (the team trim)
    f.tube("spine", 0, 0.2, b.z_spine + 3, b.z_neck - 1, b.waist + 0.6, b.depth + 0.5, b.chest + 0.6, b.depth + 0.7, MAIN)
    f.paint("spine", lambda x, y, z: z > b.z_spine + 3 and ((z - b.z_spine) % 4 == 0 or abs(x + 0.5) < 0.6) and (x * x / (b.chest + 1) ** 2 + y * y / (b.depth + 1) ** 2) > 0.65, TRIM_GLOW)
    for s_ in ("l", "r"):
        f.paint("leg_%s_thigh" % s_, lambda x, y, z: y > 0.5, MAIN)
        f.paint("arm_%s_upper" % s_, lambda x, y, z: (b.top - z) % 4 == 0, TRIM_GLOW)
    # forearm blades folded back along the forearms, past the elbows
    for s_, sgn in (("l", 1), ("r", -1)):
        lo = b.top - b.upper - b.fore
        for i in range(b.fore + 6):
            z = lo + 1 + i
            f.put("extra:arm_%s_fore:blade" % s_, int(round(sgn * (b.ax + 1.6))) - (1 if sgn < 0 else 0), -2 - (i // 6), z, PALE)
            if i > 2:
                f.put("extra:arm_%s_fore:blade" % s_, int(round(sgn * (b.ax + 1.6))) - (1 if sgn < 0 else 0), -3 - (i // 6), z, METAL)
    return f, {SKIN: (120, 124, 120), MAIN: (40, 40, 46), PALE: (196, 196, 186), METAL: (90, 92, 100), DARK: (20, 20, 24),
               EYE: (230, 120, 40), TRIM_GLOW: (200, 240, 230), TRIM_GLOW_DARK: (120, 150, 146)}, {EYE: 0.9, TRIM_GLOW: 0.8, TRIM_GLOW_DARK: 0.5}


def sabine():
    """Gladiator: athletic, net-fighter's cape, one bare arm."""
    f = Figure()
    b = body(f, std(height=1.80, thigh_r=3.8, knee=2.6, ankle=1.8, shin=16, thigh=16, hip_w=13, waist=5.0, chest=7.2, depth=4.2,
                    head=(4.3, 4.8, 5.3), arm_r=2.3, elbow=1.9, wrist=1.4, upper=11, fore=11, calf=1.3,
                    c_pelvis=LEATHER, c_spine=MAIN, c_leg=SKIN, c_shin=LEATHER, c_foot=LEATHER, c_upper=METAL, c_fore=METAL))
    face(f, b, brow=HAIR, ez=0)
    hair(f, b, HAIR, top=1.0)
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    # a braid down the back
    for i in range(14):
        f.ball("head", cx, cy - ry - 0.5, cz - i * 0.9, 1.4, 1.2, 1.0, HAIR)
    # the right arm bare with a leather wrap; the left armoured, banded in trim
    for part in ("arm_r_upper", "arm_r_fore"):
        f.paint(part, lambda x, y, z: True, SKIN)
    band(f, "arm_r_fore", b.top - b.upper - b.fore, b.top - b.upper - b.fore + 5, LEATHER)
    f.paint("arm_l_fore", lambda x, y, z: (z - b.top) % 3 == 0, TRIM_DARK)
    # a tunic with a belt, a shoulder strap
    band(f, "spine", b.z_spine, b.z_spine + 2, LEATHER)
    f.fill("spine", lambda x, y, z: abs((x - (z - b.z_spine) * 0.45) + 3) < 1.2, -10, -6, b.z_spine, 10, 6, b.z_neck, LEATHER)
    # the cape from the shoulders behind: the net-fighter's cloak, wider at the hem
    for z in range(b.z_spine - 14, b.z_neck + 1):
        w = 7.5 + (b.z_neck - z) * 0.12
        for x in range(int(-w), int(w) + 1):
            f.put("extra:spine:cape", x, -int(b.depth) - 1, z, TRIM_DARK if (z % 4 == 0 or x % 4 == 0) else TRIM)
    return f, {SKIN: (160, 104, 70), MAIN: (200, 190, 160), LEATHER: (110, 70, 40), METAL: (170, 170, 176), HAIR: (60, 30, 20),
               WHITE: (240, 236, 228), IRIS: (60, 90, 50), LIP: (150, 80, 60), TRIM: (40, 120, 160), TRIM_DARK: (24, 80, 110)}, {}


def kade():
    """Merc: hood, scarf over the face."""
    f = Figure()
    b = body(f, std(height=1.78, waist=5.6, chest=7.6, depth=4.5, arm_r=2.4, elbow=2.0, wrist=1.5,
                    c_pelvis=DARK, c_spine=MAIN, c_leg=SECOND, c_shin=SECOND, c_foot=DARK, c_upper=MAIN, c_fore=MAIN, c_hand=DARK, c_neck=MAIN))
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    f.paint("head", lambda x, y, z: True, SHADE)
    face(f, b, eye=WHITE, iris=IRIS, nose=None, mouth=None, ez=1, ears=False)
    # the hood: a shell over the head, open at the face, its peak over the brow
    f.ball("head", cx, cy - 0.5, cz + 0.5, rx + 1.6, ry + 1.6, rz + 1.6, MAIN,
           keep=lambda x, y, z: not (y > cy + 1 and abs(x) < rx - 0.5 and z < cz + 3.5 and z > cz - rz))
    f.paint("head", lambda x, y, z: y > cy + ry - 0.5 and z > cz + 3, SECOND)
    # the scarf over mouth and nose, its end hanging down the chest
    f.ball("head", cx, cy + 0.5, cz - 2.5, rx + 1.0, ry + 1.0, 2.6, TRIM, keep=lambda x, y, z: y > cy - ry * 0.4)
    for z in range(b.z_spine + 4, b.z_neck + 1):
        for x in (-4, -3, -2):
            y = f.front("spine", x, z)
            if y is not None:
                f.put("spine", x, y + 1, z, TRIM_DARK)
    # belts, a holster, pouches
    band(f, "spine", b.z_spine, b.z_spine + 2, LEATHER)
    f.fill("spine", lambda x, y, z: abs((x + (z - b.z_spine) * 0.5) - 1) < 1.0, -10, -6, b.z_spine, 10, 7, b.z_neck, LEATHER)
    f.rbox("pelvis", 5, -2, b.z_pelvis - 3, 8, 3, b.z_pelvis + 4, LEATHER, r=1.0)
    for x0 in (-6, -2):
        f.rbox("pelvis", x0, b.hip_d // 2 - 1, b.z_pelvis + 1, x0 + 3, b.hip_d // 2 + 1, b.z_pelvis + 4, LEATHER, r=0.8)
    return f, {SKIN: (180, 130, 100), MAIN: (110, 90, 70), SECOND: (70, 60, 50), DARK: (24, 22, 22), LEATHER: (80, 50, 30),
               SHADE: (60, 46, 38), WHITE: (230, 230, 210), IRIS: (120, 160, 120), TRIM: (200, 60, 50), TRIM_DARK: (130, 40, 34)}, {}


def hex_():
    """Cyborg: half human, half machine; one mechanical arm."""
    f = Figure()
    b = body(f, std(height=1.78, waist=5.8, chest=7.8, depth=4.6, shin=16,
                    c_pelvis=DARK, c_spine=MAIN, c_leg=SECOND, c_shin=SECOND, c_foot=DARK, c_upper=MAIN, c_fore=SKIN))
    face(f, b, brow=HAIR, ez=0)
    hair(f, b, HAIR, top=1.0)
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    # the right half of the face metal, its eye a light, a seam between
    f.paint("head", lambda x, y, z: x < 0 and z < cz + rz * 0.5, METAL)
    f.paint("head", lambda x, y, z: x == -1 and y > cy - 1, DARK)
    for x in (-3, -4):
        f.stamp("head", x, int(cz), TRIM_GLOW)
        f.stamp("head", x, int(cz) + 1, TRIM_GLOW)
    # the mechanical right arm: plated, thicker at the shoulder, lit seams; pistons on the forearm
    for part in ("arm_r_upper", "arm_r_fore", "arm_r_hand"):
        f.paint(part, lambda x, y, z: True, METAL)
    f.ball("arm_r_upper", -b.ax, 0, b.top - 2, 3.8, 3.4, 3.5, METAL)
    f.paint("arm_r_upper", lambda x, y, z: (b.top - z) % 4 == 0, TRIM_GLOW)
    f.paint("arm_r_fore", lambda x, y, z: (b.top - b.upper - z) % 4 == 2, TRIM_GLOW_DARK)
    x0, y0, z0, x1, y1, z1 = f.bounds("arm_r_fore")
    for z in range(z0 + 1, z1 - 1):
        f.put("arm_r_fore", x0 - 1, (y0 + y1) // 2, z, DARK)
    f.paint("spine", lambda x, y, z: x < -3 and z > b.z_neck - 6, METAL)
    # a jacket collar and zip
    f.paint("spine", lambda x, y, z: abs(x + 0.5) < 0.6 and y > 2, DARK)
    return f, {SKIN: (200, 150, 120), MAIN: (40, 50, 64), SECOND: (60, 60, 64), DARK: (24, 24, 28), HAIR: (220, 220, 220),
               METAL: (150, 156, 166), WHITE: (236, 236, 236), IRIS: (60, 80, 120), LIP: (160, 100, 90),
               TRIM_GLOW: (240, 250, 255), TRIM_GLOW_DARK: (150, 160, 170)}, {TRIM_GLOW: 1.0, TRIM_GLOW_DARK: 0.8}


def torvald():
    """Gladiator: bearded giant, helmet, fur cape."""
    f = Figure()
    b = body(f, std(height=1.86, foot=13, ankle=3.0, knee=4.0, thigh_r=5.4, shin=16, thigh=17, hip_w=19, hip_d=12, waist=8.5, chest=11.5, depth=6.8,
                    torso=20, neck_r=3.4, head=(5.0, 5.4, 5.8), arm_r=3.6, elbow=3.0, wrist=2.3, upper=13, fore=12, hand=6, belly=0.8,
                    c_pelvis=LEATHER, c_spine=MAIN, c_leg=SECOND, c_shin=LEATHER, c_foot=LEATHER, c_upper=MAIN, c_fore=SKIN))
    face(f, b, brow=HAIR, ez=0.5, mouth=None)
    cx, cy, cz = b.hc
    rx, ry, rz = b.hr
    # the beard: full, down onto the chest; a moustache
    f.ball("head", cx, cy + 1.5, cz - 3.0, rx * 0.95, ry * 0.75, 4.0, HAIR, keep=lambda x, y, z: y > cy - 1 and z < cz - 1)
    for x in range(-3, 3):
        f.stamp("head", x, int(cz) - 2, HAIR, out=0)
    f.ball("spine", 0, b.depth - 1.0, b.z_neck - 3, 4.5, 2.0, 4.0, HAIR, keep=lambda x, y, z: y > b.depth - 2)
    # the helmet: a round cap with a nasal guard and a band of trim
    helmet(f, b, METAL, low=0.25, thick=1.2, open_face=False)
    f.paint("head", lambda x, y, z: abs(z - (cz + rz * 0.25 + 1.0)) < 1.0 and (x * x + (y - cy) ** 2) > (rx - 0.5) ** 2, TRIM)
    for z in range(int(cz) - 1, int(cz + rz * 0.3) + 1):
        f.stamp("head", 0, z, METAL, out=0)
        f.stamp("head", -1, z, METAL, out=0)
    # a belt with a round buckle
    band(f, "pelvis", b.z_spine - 2, b.z_spine, METAL)
    f.ball("pelvis", 0, b.hip_d / 2.0, b.z_spine - 1, 1.8, 1.0, 1.8, TRIM_DARK)
    # the fur cape: heavy over the shoulders, ragged at the hem, trimmed at the collar
    for z in range(b.z_spine - 14, b.z_neck + 3):
        w = b.chest + 1.5 + (b.z_neck - z) * 0.08
        for x in range(int(-w), int(w) + 1):
            ragged = z < b.z_spine - 11 and (x * 7) % 5 == 0
            if ragged:
                continue
            d = 2 if (x + z) % 3 else 1
            for k in range(d):
                f.put("extra:spine:cape", x, -int(b.depth) - 1 - k, z, FUR if (x * 5 + z * 3) % 7 else SHADE)
    for x in range(int(-b.chest - 3), int(b.chest + 4)):
        for y in range(-int(b.depth) - 2, int(b.depth) - 1):
            if x * x / (b.chest + 3) ** 2 + (y + 1) ** 2 / (b.depth + 1) ** 2 <= 1.0 and x * x / (b.chest - 2) ** 2 + (y + 1) ** 2 / (b.depth - 2) ** 2 > 1.0:
                for z in (b.z_neck + 1, b.z_neck + 2):
                    f.put("extra:spine:cape", x, y, z, TRIM if z == b.z_neck + 2 else FUR)
    return f, {SKIN: (214, 160, 130), MAIN: (110, 40, 34), SECOND: (70, 56, 44), LEATHER: (90, 60, 36), HAIR: (200, 120, 50),
               METAL: (120, 120, 126), PALE: (230, 220, 190), FUR: (110, 90, 70), SHADE: (80, 64, 50), WHITE: (236, 232, 226),
               IRIS: (60, 90, 140), TRIM: (220, 200, 120), TRIM_DARK: (150, 136, 80)}, {}


RECIPES = {"corvin": corvin, "vesper": vesper, "rook": rook, "unit7": unit7, "mother": mother, "skrell": skrell,
           "gorrum": gorrum, "nyx": nyx, "sabine": sabine, "kade": kade, "hex": hex_, "torvald": torvald}


# ── writing .vox ────────────────────────────────────────────────────────────

def i32(v):
    return struct.pack("<i", v)


def string(s):
    b = s.encode()
    return i32(len(b)) + b


def dct(d):
    return i32(len(d)) + b"".join(string(k) + string(v) for k, v in d.items())


def chunk(tag, content, children=b""):
    return tag.encode() + i32(len(content)) + i32(len(children)) + content + children


def surface(vs):
    """The voxels with an empty neighbour (the rest are never seen)."""
    out = {}
    for (x, y, z), c in vs.items():
        for d in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
            if (x + d[0], y + d[1], z + d[2]) not in vs:
                out[(x, y, z)] = c
                break
    return out


def vox_bytes(f, colours, glow):
    """The figure as a MagicaVoxel file: each part a model, named and placed (low corner + floor(size / 2))."""
    names = PARTS + [n for n in f.order if n not in PARTS]
    models, nodes = [], []
    kids = [2 + 2 * k for k in range(len(names))]
    nodes.append(chunk("nTRN", i32(0) + dct({"_strata_voxel": "%g" % VOXEL}) + i32(1) + i32(-1) + i32(-1) + i32(1) + dct({})))
    nodes.append(chunk("nGRP", i32(1) + dct({}) + i32(len(kids)) + b"".join(i32(c) for c in kids)))
    for k, name in enumerate(names):
        vs = f.parts[name]
        x0, y0, z0, x1, y1, z1 = f.bounds(name)
        sx, sy, sz = x1 - x0, y1 - y0, z1 - z0
        if max(sx, sy, sz) > 128:
            raise ValueError("%s is over 128 voxels a side" % name)
        shell = surface(vs)
        body = b"".join(bytes((x - x0, y - y0, z - z0, c)) for (x, y, z), c in sorted(shell.items()))
        models.append(chunk("SIZE", i32(sx) + i32(sy) + i32(sz)) + chunk("XYZI", i32(len(shell)) + body))
        t = "%d %d %d" % (x0 + sx // 2, y0 + sy // 2, z0 + sz // 2)
        nodes.append(chunk("nTRN", i32(2 + 2 * k) + dct({"_name": name}) + i32(3 + 2 * k) + i32(-1) + i32(0) + i32(1) + dct({"_t": t})))
        nodes.append(chunk("nSHP", i32(3 + 2 * k) + dct({}) + i32(1) + i32(k) + dct({})))
    rgba = bytearray(1024)
    for i, (r, g, b) in colours.items():
        rgba[4 * (i - 1): 4 * i] = bytes((r, g, b, 255))
    mats = b"".join(chunk("MATL", i32(i) + dct({"_type": "_emit", "_emit": "%.2f" % e, "_rough": "0.4"})) for i, e in sorted(glow.items()))
    body = b"".join(models) + b"".join(nodes) + chunk("RGBA", bytes(rgba)) + mats
    return b"VOX " + i32(150) + chunk("MAIN", b"", body)


def hash3(x, y, z, k=0):
    h = (x * 73856093) ^ (y * 19349663) ^ (z * 83492791) ^ (k * 2654435761)
    h = (h ^ (h >> 13)) * 1274126177
    return ((h ^ (h >> 16)) & 0xffff) / 65535.0


DARKER, DARK_, LIGHT_ = 16, 32, 48   # shade variants: base index + offset (bases 1-15)


def mute(c, sat=0.78, gain=0.95):
    r, g, b = c
    grey = 0.3 * r + 0.59 * g + 0.11 * b
    return tuple(int(max(0, min(255, (grey + (v - grey) * sat) * gain))) for v in (r, g, b))


def bake(f):
    """Shade every voxel of a base colour (1-15) by what surrounds it: creases and undersides darker, upper edges lighter,
    grain and wear scattered; metal scratched. Trim and lights stay flat."""
    solid = set()
    for p in f.parts.values():
        solid.update(p)
    R = 2
    span = [(dx, dy, dz) for dx in range(-R, R + 1) for dy in range(-R, R + 1) for dz in range(-R, R + 1)]
    n = float(len(span))
    for name, p in f.parts.items():
        for (x, y, z), c in list(p.items()):
            if not (1 <= c <= 15):
                continue
            occ = sum(1 for d in span if (x + d[0], y + d[1], z + d[2]) in solid) / n
            up = (x, y, z + 1) not in solid
            down = (x, y, z - 1) not in solid
            v = 0
            if occ > 0.66 or down:
                v = 1
            if occ > 0.78:
                v = 2
            if up and occ < 0.5:
                v = 3
            r = hash3(x, y, z)
            if c == METAL:
                if r < 0.07:
                    v = 3
                elif r > 0.95:
                    v = 1
            elif r < 0.06:
                v = 1 if v != 2 else 2
            elif r > 0.97 and v == 0:
                v = 3
            if v:
                p[(x, y, z)] = c + (DARKER if v == 1 else DARK_ if v == 2 else LIGHT_)


def build(cid):
    f, colours, glow = RECIPES[cid]()
    bake(f)
    for i in range(1, 16):
        if i in colours:
            base = mute(colours[i])
            colours[i] = base
            colours[i + DARKER] = tuple(int(v * 0.74) for v in base)
            colours[i + DARK_] = tuple(int(v * 0.56) for v in base)
            colours[i + LIGHT_] = tuple(min(255, int(v * 1.16 + 6)) for v in base)
    for i in (TRIM, TRIM_DARK):
        if i in colours:
            colours[i] = mute(colours[i], sat=0.75, gain=0.92)
    for name in list(f.parts):
        if not f.parts[name]:
            raise ValueError("%s: part %s is empty" % (cid, name))
    used = {c for p in f.parts.values() for c in p.values()}
    for c in used:
        colours.setdefault(c, (255, 0, 255))   # a colour a recipe forgot shows magenta
    return f, colours, glow


def read_manifest():
    m = {}
    if os.path.exists(MANIFEST):
        for line in open(MANIFEST):
            w = line.split()
            if len(w) == 2:
                m[w[0]] = w[1]
    return m


def main(argv):
    force = "--force" in argv
    only = None
    if "--only" in argv:
        only = argv[argv.index("--only") + 1].split(",")
    os.makedirs(OUT, exist_ok=True)
    manifest = read_manifest()
    for cid in ROSTER:
        if only and cid not in only:
            continue
        path = os.path.join(OUT, cid + ".vox")
        if os.path.exists(path) and not force:
            have = hashlib.sha256(open(path, "rb").read()).hexdigest()
            if manifest.get(cid) != have:
                print("characters: %s was edited by hand; not overwritten (use --force)" % cid)
                continue
        f, colours, glow = build(cid)
        data = vox_bytes(f, colours, glow)
        open(path, "wb").write(data)
        manifest[cid] = hashlib.sha256(data).hexdigest()
        z = max(k[2] for p in f.parts.values() for k in p) + 1
        print("characters: wrote %s (%d parts, %.2f m tall)" % (os.path.relpath(path, ROOT), len(f.parts), z * VOXEL))
    with open(MANIFEST, "w") as m:
        for cid in ROSTER:
            if cid in manifest:
                m.write("%s %s\n" % (cid, manifest[cid]))


if __name__ == "__main__":
    main(sys.argv[1:])
