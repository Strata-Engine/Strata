#!/usr/bin/env python3
"""Write the twelve medieval characters as MagicaVoxel files, viewer/assets/characters/<id>.vox.

Blocky on purpose: voxels of 1/16 m (the size the root transform's
`_strata_voxel` gives), built from boxes, flat colour areas with a little
painted detail, and a light pass of shading at edges. Every body is the
same plan: feet on z 0, a head 5 voxels a side, the figure centred on
x 0.5 (the character's left is +x), facing +y. Heights 1.78 to 1.88 m.

Team trim is heraldry: tabards, surcoats, shields and bands are palette
241 (the shade) and 242 (darker), which a team match recolours.

The files follow the convention Strata.Character reads
(lib/strata/character.march): one model per part, named, placed by its
transform. Only each part's surface voxels are written.

Palette: 1-15 a character's own colours (17-31 darker, 49-63 lighter: the
shading pass), 241-242 trim, 250 eyes that catch the light.

A file someone has edited by hand is not overwritten: generated.txt keeps
the SHA-256 of what this script last wrote, and a file that no longer
matches is skipped with a message, unless --force.

  python3 scripts/characters.py [--force] [--only id,id]
"""
import hashlib
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "viewer", "assets", "characters")
MANIFEST = os.path.join(OUT, "generated.txt")
ROSTER = ["knight", "manatarms", "archer", "crossbowman", "norseman", "templar",
          "landsknecht", "ranger", "executioner", "friar", "squire", "lord"]
VOXEL = 1.0 / 16.0

PARTS = ["pelvis", "spine", "head", "arm_l_upper", "arm_l_fore", "arm_l_hand", "arm_r_upper", "arm_r_fore", "arm_r_hand",
         "leg_l_thigh", "leg_l_shin", "leg_l_foot", "leg_r_thigh", "leg_r_shin", "leg_r_foot"]

SKIN, CLOTH, CLOTH2, DARK, METAL, LEATHER, HAIR, GOLD, PALE, FUR, LIP, WHITE, WOOD, MAIL = range(1, 15)
TRIM, TRIM_DARK = 241, 242
EYE = 250

ANY = (-99, 99)


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

    def box(self, name, x0, y0, z0, x1, y1, z1, c):
        """Fill [x0, x1) x [y0, y1) x [z0, z1) with colour c (0 clears)."""
        p = self.part(name)
        for x in range(x0, x1):
            for y in range(y0, y1):
                for z in range(z0, z1):
                    if c:
                        p[(x, y, z)] = c
                    else:
                        p.pop((x, y, z), None)

    def paint(self, name, c, x=ANY, y=ANY, z=ANY, only=None):
        """Recolour a part's voxels in [x0, x1) x [y0, y1) x [z0, z1) (only those of colour `only`, if given)."""
        p = self.parts[name]
        for k in list(p):
            if x[0] <= k[0] < x[1] and y[0] <= k[1] < y[1] and z[0] <= k[2] < z[1] and (only is None or p[k] == only):
                p[k] = c

    def bounds(self, name):
        vs = self.parts[name]
        xs = [k[0] for k in vs]; ys = [k[1] for k in vs]; zs = [k[2] for k in vs]
        return min(xs), min(ys), min(zs), max(xs) + 1, max(ys) + 1, max(zs) + 1


class B(dict):
    __getattr__ = dict.__getitem__


def body(f, shin=7, thigh=6, torso=7, wide=False, deep=4, colours=None):
    """The fifteen parts, centred on x 0.5: legs 3 x 3 with a gap of one, a pelvis 7 wide, a torso 7 (9 if wide) wide and
    [deep] deep, a head 5 a side, arms 2 x 2 (upper 5, fore 5, hand 2) hanging beside the torso. Returns where things are."""
    c = dict(pelvis=CLOTH2, torso=CLOTH, thigh=CLOTH2, shin=CLOTH2, foot=LEATHER, upper=CLOTH, fore=CLOTH, hand=SKIN)
    c.update(colours or {})
    z_shin, z_thigh = 1, 1 + shin
    z_pelvis = z_thigh + thigh
    z_torso = z_pelvis + 3
    z_head = z_torso + torso
    tw = 9 if wide else 7
    tx0 = -4 if wide else -3
    ty0 = -((deep - 4) // 2)
    ty1 = ty0 + deep
    f.box("pelvis", -3, 0, z_pelvis, 4, 4, z_torso, c["pelvis"])
    f.box("spine", tx0, ty0, z_torso, tx0 + tw, ty1, z_head, c["torso"])
    f.box("head", -2, 0, z_head, 3, 5, z_head + 5, SKIN)
    for side, x0 in (("l", 1), ("r", -3)):
        f.box("leg_%s_thigh" % side, x0, 0, z_thigh, x0 + 3, 3, z_pelvis, c["thigh"])
        f.box("leg_%s_shin" % side, x0, 0, z_shin, x0 + 3, 3, z_thigh, c["shin"])
        f.box("leg_%s_foot" % side, x0, 0, 0, x0 + 3, 5, 1, c["foot"])
    top = z_head - 1
    ax = tx0 + tw
    for side, x0 in (("l", ax), ("r", 1 - ax - 2)):
        f.box("arm_%s_upper" % side, x0, 1, top - 5, x0 + 2, 3, top, c["upper"])
        f.box("arm_%s_fore" % side, x0, 1, top - 10, x0 + 2, 3, top - 5, c["fore"])
        f.box("arm_%s_hand" % side, x0, 1, top - 12, x0 + 2, 3, top - 10, c["hand"])
    return B(z_pelvis=z_pelvis, z_torso=z_torso, z_head=z_head, z_thigh=z_thigh, top=top, tx0=tx0, tw=tw, ax=ax, ty0=ty0, ty1=ty1)


def face(f, b, eye=DARK, brow=None, beard=None, mouth=None):
    """Two eyes (a column apart, the nose between, standing out a voxel), a brow, a mouth or a beard on the head's front (y 4)."""
    z = b.z_head + 2
    for x in (-1, 1):
        f.box("head", x, 4, z, x + 1, 5, z + 1, eye)
    f.box("head", 0, 5, z - 1, 1, 6, z, SKIN)
    if brow:
        f.box("head", -2, 4, z + 1, 3, 5, z + 2, brow)
    if mouth:
        f.box("head", -1, 4, z - 2, 2, 5, z - 1, mouth)
    if beard:
        f.box("head", -2, 4, b.z_head, 3, 5, z - 1, beard)
        f.box("head", -1, 5, b.z_head - 1, 2, 6, z - 1, beard)


def hair(f, b, c):
    z = b.z_head
    f.box("head", -2, 0, z + 5, 3, 5, z + 6, c)
    f.box("head", -2, -1, z + 1, 3, 0, z + 6, c)
    f.box("head", -3, 0, z + 3, -2, 4, z + 6, c)
    f.box("head", 3, 0, z + 3, 4, 4, z + 6, c)


def open_face(f, b, brow=HAIR, mouth=LIP, beard=None, lo=1, hi=5):
    """Clear rows [lo, hi) of the head's front back to skin (after a helmet or hood covered them) and draw the face."""
    z = b.z_head
    f.box("head", -2, 4, z + lo, 3, 6, z + hi, 0)
    f.box("head", -2, 0, z + lo, 3, 5, z + hi, SKIN)
    face(f, b, brow=brow, mouth=mouth, beard=beard)


# ── the twelve ──────────────────────────────────────────────────────────────

def knight():
    """Full plate, a flat-topped great helm with a slit visor; a tabard of the team's colour."""
    f = Figure()
    b = body(f, wide=True, deep=5,
             colours=dict(pelvis=METAL, torso=METAL, thigh=METAL, shin=METAL, foot=METAL, upper=METAL, fore=METAL, hand=METAL))
    z = b.z_head
    f.box("head", -3, -1, z, 4, 6, z + 6, METAL)                 # the great helm
    f.box("head", -3, 5, z + 3, 4, 6, z + 4, DARK)               # the eye slit
    f.box("head", 0, 5, z + 1, 1, 6, z + 3, DARK)                # breaths down the middle
    f.box("head", -3, -1, z + 5, 4, 6, z + 6, PALE)              # the flat top
    # the tabard over the plate, front and back, its hem darker
    f.box("spine", -3, b.ty1, b.z_torso, 4, b.ty1 + 1, b.z_head - 1, TRIM)
    f.box("spine", -3, b.ty0 - 1, b.z_torso, 4, b.ty0, b.z_head - 1, TRIM)
    f.box("pelvis", -3, 4, b.z_pelvis - 3, 4, 5, b.z_torso, TRIM)
    f.box("pelvis", -3, -1, b.z_pelvis - 3, 4, 0, b.z_torso, TRIM)
    f.paint("pelvis", TRIM_DARK, z=(b.z_pelvis - 3, b.z_pelvis - 2))
    f.box("spine", -4, b.ty0, b.z_torso, 5, b.ty1, b.z_torso + 1, LEATHER)   # the sword belt
    for s, x0 in (("l", b.ax - 1), ("r", 1 - b.ax - 2)):                    # pauldrons and couters
        f.box("arm_%s_upper" % s, x0, 0, b.top - 1, x0 + 3, 4, b.top + 1, METAL)
        f.paint("arm_%s_fore" % s, PALE, z=(b.top - 6, b.top - 5))
    for s in ("l", "r"):
        f.paint("leg_%s_shin" % s, PALE, z=(b.z_thigh - 1, b.z_thigh))
    return f, {METAL: (150, 152, 158), PALE: (196, 198, 204), DARK: (24, 24, 28), LEATHER: (80, 52, 32), SKIN: (190, 140, 110),
               TRIM: (40, 70, 150), TRIM_DARK: (28, 50, 110)}


def manatarms():
    """A mail shirt and a wide-brimmed kettle hat; a surcoat of the team's colour."""
    f = Figure()
    b = body(f, colours=dict(pelvis=MAIL, torso=MAIL, thigh=CLOTH2, shin=CLOTH2, foot=LEATHER, upper=MAIL, fore=MAIL, hand=LEATHER))
    face(f, b, beard=HAIR)
    z = b.z_head
    f.box("head", -2, 0, z + 5, 3, 5, z + 7, METAL)              # the kettle hat's crown
    f.box("head", -4, -2, z + 5, 5, 7, z + 6, METAL)             # its wide brim
    f.box("head", -2, -1, z + 1, 3, 0, z + 5, MAIL)              # a mail coif behind and at the sides
    f.box("head", -3, 0, z, -2, 5, z + 5, MAIL)
    f.box("head", 3, 0, z, 4, 5, z + 5, MAIL)
    # the surcoat: sleeveless, to the knee, split at the front below the belt
    f.box("spine", -3, 4, b.z_torso, 4, 5, b.z_head - 1, TRIM)
    f.box("spine", -3, -1, b.z_torso, 4, 0, b.z_head - 1, TRIM)
    f.box("pelvis", -3, 4, b.z_pelvis - 4, 0, 5, b.z_torso, TRIM)
    f.box("pelvis", 1, 4, b.z_pelvis - 4, 4, 5, b.z_torso, TRIM)
    f.box("pelvis", -3, -1, b.z_pelvis - 4, 4, 0, b.z_torso, TRIM)
    f.box("spine", -3, -1, b.z_torso + 1, 4, 5, b.z_torso + 2, LEATHER)
    for s in ("l", "r"):
        f.paint("leg_%s_thigh" % s, MAIL, z=(b.z_pelvis - 2, b.z_pelvis))
    return f, {MAIL: (128, 130, 134), METAL: (110, 112, 116), CLOTH2: (92, 80, 62), LEATHER: (74, 50, 32), SKIN: (196, 150, 118),
               HAIR: (84, 56, 36), DARK: (30, 24, 20), TRIM: (150, 40, 34), TRIM_DARK: (110, 30, 26)}


def archer():
    """A hood and a short cloak, a quiver on the back; the hood's lining and an arm band of the team's colour."""
    f = Figure()
    b = body(f, colours=dict(pelvis=LEATHER, torso=CLOTH, thigh=CLOTH2, shin=CLOTH2, foot=LEATHER, upper=CLOTH, fore=CLOTH, hand=LEATHER))
    z = b.z_head
    # the hood: over the top, the sides and the back, its edge lined, a liripipe hanging behind
    f.box("head", -3, -1, z + 1, 4, 6, z + 6, CLOTH2)
    f.box("head", -3, 5, z + 1, 4, 6, z + 6, TRIM)
    open_face(f, b, lo=1, hi=5)
    f.box("head", 0, -3, z - 2, 1, -1, z + 4, CLOTH2)
    # the short cloak over the shoulders, a belt, a band on the bow arm
    f.box("spine", -4, -1, b.z_head - 3, 5, 5, b.z_head, CLOTH2)
    f.paint("arm_l_upper", TRIM, z=(b.top - 4, b.top - 3))
    f.box("spine", -3, 4, b.z_torso, 4, 5, b.z_torso + 1, LEATHER)
    # the quiver slanting over the right shoulder, the fletching above it
    for i in range(9):
        f.box("extra:spine:quiver", -2 + i // 3, -2, b.z_torso - 1 + i, i // 3, -1, b.z_torso + i, LEATHER)
    f.box("extra:spine:quiver", 1, -2, b.z_torso + 8, 3, -1, b.z_torso + 10, PALE)
    return f, {CLOTH: (96, 108, 66), CLOTH2: (70, 78, 48), LEATHER: (98, 66, 38), SKIN: (200, 152, 118), HAIR: (110, 74, 40),
               LIP: (150, 96, 80), DARK: (30, 24, 20), PALE: (220, 216, 200), TRIM: (170, 40, 34), TRIM_DARK: (120, 30, 26)}


def crossbowman():
    """A studded brigandine and a round cap, a pavise on the back in the team's colour."""
    f = Figure()
    b = body(f, wide=True, colours=dict(pelvis=CLOTH2, torso=CLOTH, thigh=CLOTH2, shin=LEATHER, foot=LEATHER, upper=CLOTH2, fore=CLOTH2, hand=LEATHER))
    face(f, b, brow=HAIR, mouth=LIP)
    z = b.z_head
    f.box("head", -2, 0, z + 5, 3, 5, z + 6, METAL)              # a round iron cap
    f.box("head", -3, -1, z + 4, 4, 6, z + 5, METAL)
    for x in range(-3, 5, 2):                                    # rivets on the brigandine
        for zz in range(b.z_torso + 1, b.z_head - 1, 2):
            f.box("spine", x, b.ty1 - 1, zz, x + 1, b.ty1, zz + 1, GOLD)
    # the pavise: a tall shield on the back, the team's colour with a darker stripe, a wooden top
    f.box("extra:spine:pavise", -4, -2, b.z_pelvis - 3, 5, -1, b.z_head + 1, TRIM)
    f.box("extra:spine:pavise", 0, -2, b.z_pelvis - 3, 1, -1, b.z_head + 1, TRIM_DARK)
    f.box("extra:spine:pavise", -4, -2, b.z_head + 1, 5, -1, b.z_head + 2, WOOD)
    return f, {CLOTH: (110, 44, 38), CLOTH2: (76, 64, 50), LEATHER: (80, 54, 34), METAL: (120, 122, 126), GOLD: (190, 160, 90),
               SKIN: (190, 146, 116), HAIR: (60, 44, 30), LIP: (150, 96, 80), DARK: (30, 24, 20), WOOD: (110, 80, 50),
               TRIM: (40, 90, 150), TRIM_DARK: (28, 62, 110)}


def norseman():
    """Bare arms, fur, a braided beard, a round shield on the back of the team's colour."""
    f = Figure()
    b = body(f, torso=8, thigh=5, wide=True, deep=5,
             colours=dict(pelvis=CLOTH2, torso=CLOTH, thigh=CLOTH2, shin=LEATHER, foot=FUR, upper=SKIN, fore=SKIN, hand=SKIN))
    face(f, b, brow=HAIR, beard=HAIR)
    hair(f, b, HAIR)
    f.box("head", 0, 5, b.z_head - 3, 1, 6, b.z_head, HAIR)      # the braid of the beard
    f.box("spine", -5, -2, b.z_head - 2, 6, 5, b.z_head, FUR)    # fur over the shoulders
    f.box("spine", -4, -1, b.z_torso, 5, 5, b.z_torso + 1, LEATHER)
    for s in ("l", "r"):
        f.paint("arm_%s_fore" % s, LEATHER, z=(b.top - 10, b.top - 7))
    # the round shield on the back: planks of the team's colour, an iron boss and rim
    cz = b.z_torso + 3.5
    for x in range(-5, 7):
        for zz in range(b.z_torso - 3, b.z_torso + 10):
            d = (x + 0.5 - 0.5) ** 2 + (zz + 0.5 - cz) ** 2
            if d <= 36:
                c = METAL if d > 28 or d < 2 else (TRIM if (x + 50) % 4 < 2 else TRIM_DARK)
                f.box("extra:spine:shield", x, -3, zz, x + 1, -2, zz + 1, c)
    return f, {SKIN: (204, 156, 122), CLOTH: (90, 74, 56), CLOTH2: (70, 60, 48), LEATHER: (88, 60, 38), FUR: (120, 100, 78),
               HAIR: (190, 120, 60), METAL: (110, 112, 116), DARK: (40, 50, 70), TRIM: (160, 40, 34), TRIM_DARK: (120, 30, 26)}


def templar():
    """A white mantle over mail, a cylinder helm; the cross on the mantle is the team's colour."""
    f = Figure()
    b = body(f, colours=dict(pelvis=WHITE, torso=WHITE, thigh=MAIL, shin=MAIL, foot=METAL, upper=MAIL, fore=MAIL, hand=METAL))
    z = b.z_head
    f.box("head", -3, -1, z, 4, 6, z + 6, METAL)                 # the cylinder helm
    f.box("head", -3, 5, z + 3, 4, 6, z + 4, DARK)               # its cross-shaped vision slits
    f.box("head", 0, 5, z + 1, 1, 6, z + 5, DARK)
    # the mantle: long, front and back, the cross on chest and back
    f.box("pelvis", -3, 4, b.z_pelvis - 6, 4, 5, b.z_torso, WHITE)
    f.box("pelvis", -3, -1, b.z_pelvis - 6, 4, 0, b.z_torso, WHITE)
    for yy in (4, -1):
        f.box("spine", -3, yy, b.z_torso, 4, yy + 1, b.z_head - 1, WHITE)
        f.box("spine", 0, yy, b.z_torso + 1, 1, yy + 1, b.z_head - 1, TRIM)
        f.box("spine", -2, yy, b.z_torso + 4, 3, yy + 1, b.z_torso + 5, TRIM)
    f.box("spine", -3, 0, b.z_torso, 4, 4, b.z_torso + 1, LEATHER)
    return f, {WHITE: (214, 210, 196), MAIL: (128, 130, 134), METAL: (150, 152, 158), DARK: (24, 24, 28), LEATHER: (80, 52, 32),
               SKIN: (190, 146, 116), TRIM: (160, 30, 30), TRIM_DARK: (110, 20, 20)}


def landsknecht():
    """Puffed, slashed sleeves and a huge feathered beret; the stripes and the feather are the team's colour."""
    f = Figure()
    b = body(f, colours=dict(pelvis=CLOTH2, torso=CLOTH, thigh=CLOTH, shin=CLOTH2, foot=LEATHER, upper=CLOTH, fore=CLOTH, hand=SKIN))
    face(f, b, brow=HAIR, beard=HAIR)
    z = b.z_head
    # the beret: a broad slab tilted to the left, a feather sweeping back over it
    f.box("head", -3, -1, z + 5, 5, 6, z + 6, CLOTH2)
    f.box("head", -2, 0, z + 6, 5, 5, z + 7, CLOTH2)
    for i in range(6):
        f.box("head", 3 + i // 3, 3 - i, z + 5 + i // 4, 4 + i // 3, 4 - i, z + 6 + i // 4, TRIM)
    # puffed sleeves and breeches, slashed with stripes of the team's colour
    for s, x0 in (("l", b.ax), ("r", 1 - b.ax - 3)):
        f.box("arm_%s_upper" % s, x0, 0, b.top - 5, x0 + 3, 4, b.top, CLOTH)
        for zz in range(b.top - 5, b.top, 2):
            f.box("arm_%s_upper" % s, x0, 3, zz, x0 + 3, 4, zz + 1, TRIM)
    for s, x0, x1 in (("l", 1, 5), ("r", -4, 0)):
        f.box("leg_%s_thigh" % s, x0, -1, b.z_pelvis - 4, x1, 4, b.z_pelvis, CLOTH)
        f.paint("leg_%s_thigh" % s, TRIM, x=(x0 + 1, x0 + 2))
        f.paint("leg_%s_thigh" % s, TRIM, x=(x1 - 2, x1 - 1))
    f.box("spine", -3, 4, b.z_torso + 2, 4, 5, b.z_torso + 6, TRIM_DARK)
    f.box("spine", -3, 0, b.z_torso, 4, 4, b.z_torso + 1, LEATHER)
    return f, {CLOTH: (180, 160, 110), CLOTH2: (50, 46, 44), LEATHER: (86, 58, 36), SKIN: (204, 156, 122), HAIR: (130, 80, 40),
               DARK: (30, 24, 20), TRIM: (170, 40, 40), TRIM_DARK: (120, 30, 30)}


def ranger():
    """A long green cloak and a deep hood, leather bracers; the clasp and the lining are the team's colour."""
    f = Figure()
    b = body(f, colours=dict(pelvis=LEATHER, torso=CLOTH2, thigh=CLOTH2, shin=LEATHER, foot=LEATHER, upper=CLOTH2, fore=LEATHER, hand=SKIN))
    z = b.z_head
    # the deep hood, the face in its shadow, the eyes catching the light
    f.box("head", -3, -1, z, 4, 6, z + 6, CLOTH)
    f.box("head", -2, 5, z + 1, 3, 6, z + 5, 0)
    f.box("head", -2, 4, z + 1, 3, 5, z + 5, DARK)
    f.box("head", -1, 4, z + 3, 0, 5, z + 4, EYE); f.box("head", 1, 4, z + 3, 2, 5, z + 4, EYE)
    f.box("head", -3, 5, z + 5, 4, 6, z + 6, TRIM_DARK)
    # the cloak, to the shins behind, its lining of the team's colour, a clasp at the throat
    f.box("extra:spine:cloak", -4, -2, b.z_pelvis - 9, 5, -1, b.z_head, CLOTH)
    f.box("extra:spine:cloak", -4, -1, b.z_pelvis - 9, 5, 0, b.z_torso - 1, TRIM)
    f.box("spine", 0, 4, b.z_head - 2, 1, 5, b.z_head - 1, GOLD)
    f.box("spine", -3, 0, b.z_torso, 4, 4, b.z_torso + 1, LEATHER)
    return f, {CLOTH: (52, 78, 50), CLOTH2: (86, 70, 50), LEATHER: (90, 62, 38), SKIN: (190, 146, 116), DARK: (30, 34, 28),
               EYE: (210, 200, 160), GOLD: (190, 160, 90), TRIM: (50, 90, 150), TRIM_DARK: (36, 66, 110)}


def executioner():
    """Bare-chested, a black hood, a heavy apron; the apron's band is the team's colour."""
    f = Figure()
    b = body(f, torso=8, thigh=5, wide=True, deep=5,
             colours=dict(pelvis=DARK, torso=SKIN, thigh=DARK, shin=DARK, foot=LEATHER, upper=SKIN, fore=SKIN, hand=LEATHER))
    z = b.z_head
    f.box("head", -2, 0, z, 3, 5, z + 5, DARK)                   # the hood over the whole head
    f.box("head", -1, 2, z + 5, 2, 4, z + 7, DARK)               # its point
    f.box("head", -1, 4, z + 2, 0, 5, z + 3, PALE); f.box("head", 1, 4, z + 2, 2, 5, z + 3, PALE)
    f.box("spine", -4, -1, b.z_head - 1, 5, 5, b.z_head, DARK)   # the hood's cape on the shoulders
    # the apron: leather from chest to knee on straps, its band of the team's colour
    f.box("spine", -3, b.ty1, b.z_torso, 4, b.ty1 + 1, b.z_head - 3, LEATHER)
    f.box("pelvis", -3, 4, b.z_pelvis - 5, 4, 5, b.z_torso, LEATHER)
    f.box("spine", -3, b.ty1, b.z_torso + 1, 4, b.ty1 + 1, b.z_torso + 2, TRIM)
    f.box("spine", -2, b.ty1, b.z_head - 3, -1, b.ty1 + 1, b.z_head - 1, LEATHER)
    f.box("spine", 2, b.ty1, b.z_head - 3, 3, b.ty1 + 1, b.z_head - 1, LEATHER)
    return f, {SKIN: (188, 140, 108), DARK: (28, 26, 26), LEATHER: (76, 52, 34), PALE: (220, 210, 190),
               TRIM: (150, 30, 30), TRIM_DARK: (100, 20, 20)}


def friar():
    """A brown robe with a rope belt, a tonsure; the cord and the stole are the team's colour."""
    f = Figure()
    b = body(f, wide=True, colours=dict(pelvis=CLOTH, torso=CLOTH, thigh=CLOTH, shin=CLOTH, foot=LEATHER, upper=CLOTH, fore=CLOTH, hand=SKIN))
    face(f, b, brow=HAIR, mouth=LIP)
    z = b.z_head
    f.box("head", -2, -1, z + 2, 3, 0, z + 5, HAIR)              # the ring of the tonsure
    f.box("head", -3, 0, z + 3, -2, 5, z + 5, HAIR); f.box("head", 3, 0, z + 3, 4, 5, z + 5, HAIR)
    # the robe's skirt to the ankles, the hood fallen on the shoulders, wide sleeves
    f.box("extra:pelvis:robe", -4, -1, 2, 5, 5, b.z_pelvis + 1, CLOTH)
    f.box("extra:pelvis:robe", -3, 0, 2, 4, 4, b.z_pelvis + 1, 0)
    f.box("spine", -3, -2, b.z_head - 2, 4, 0, b.z_head, CLOTH2)
    for s, x0 in (("l", b.ax), ("r", 1 - b.ax - 3)):
        f.box("arm_%s_fore" % s, x0, 0, b.top - 10, x0 + 3, 4, b.top - 7, CLOTH)
    f.box("spine", -4, -1, b.z_torso, 5, 5, b.z_torso + 1, TRIM)    # the rope belt, its end hanging
    f.box("pelvis", 2, 4, b.z_pelvis - 5, 3, 5, b.z_torso, TRIM_DARK)
    f.box("spine", -2, 4, b.z_torso + 1, -1, 5, b.z_head - 1, TRIM); f.box("spine", 2, 4, b.z_torso + 1, 3, 5, b.z_head - 1, TRIM)
    return f, {CLOTH: (104, 76, 50), CLOTH2: (84, 60, 40), LEATHER: (70, 48, 30), SKIN: (204, 156, 122), HAIR: (120, 110, 100),
               LIP: (160, 100, 84), DARK: (30, 24, 20), TRIM: (200, 180, 120), TRIM_DARK: (150, 132, 84)}


def squire():
    """A padded gambeson and an open bascinet, slight of build; the gambeson's panels are the team's colour."""
    f = Figure()
    b = body(f, colours=dict(pelvis=CLOTH2, torso=CLOTH, thigh=CLOTH2, shin=LEATHER, foot=LEATHER, upper=CLOTH, fore=CLOTH, hand=SKIN))
    z = b.z_head
    # the bascinet over the top and sides, pointed, the face open; mail at the neck
    f.box("head", -3, -1, z + 2, 4, 6, z + 6, METAL)
    f.box("head", -1, 1, z + 6, 2, 4, z + 7, METAL)
    f.box("head", -3, -1, z, 4, 6, z + 2, MAIL)
    open_face(f, b, lo=0, hi=5)
    # the gambeson, quilted in rows, its side panels of the team's colour
    for zz in range(b.z_torso, b.z_head, 2):
        f.paint("spine", CLOTH2, z=(zz, zz + 1))
    f.box("spine", -3, 4, b.z_torso, -1, 5, b.z_head - 1, TRIM); f.box("spine", 2, 4, b.z_torso, 4, 5, b.z_head - 1, TRIM)
    f.box("pelvis", -3, 4, b.z_pelvis - 2, 4, 5, b.z_torso, TRIM_DARK)
    return f, {CLOTH: (196, 180, 140), CLOTH2: (170, 152, 112), LEATHER: (90, 62, 38), METAL: (150, 152, 158), MAIL: (128, 130, 134),
               SKIN: (210, 164, 132), HAIR: (150, 100, 50), LIP: (170, 104, 88), DARK: (30, 24, 20), TRIM: (40, 90, 150), TRIM_DARK: (28, 62, 110)}


def lord():
    """A fine doublet, a fur-trimmed half cloak, a chain of office and a hat; the doublet and the hat are the team's colour."""
    f = Figure()
    b = body(f, colours=dict(pelvis=DARK, torso=TRIM, thigh=DARK, shin=DARK, foot=LEATHER, upper=TRIM, fore=TRIM, hand=SKIN))
    face(f, b, brow=HAIR, beard=HAIR)
    z = b.z_head
    f.box("head", -2, 0, z + 5, 3, 5, z + 7, TRIM_DARK)          # a tall hat, its brim of fur
    f.box("head", -3, -1, z + 5, 4, 6, z + 6, FUR)
    for zz in range(b.z_torso + 1, b.z_head - 1, 2):             # the doublet's buttons
        f.box("spine", 0, 4, zz, 1, 5, zz + 1, GOLD)
    f.box("spine", -3, 4, b.z_head - 2, 4, 5, b.z_head - 1, GOLD)    # the chain of office
    f.box("spine", -2, 4, b.z_head - 3, -1, 5, b.z_head - 2, GOLD); f.box("spine", 2, 4, b.z_head - 3, 3, 5, b.z_head - 2, GOLD)
    f.box("spine", 0, 4, b.z_head - 4, 1, 5, b.z_head - 3, GOLD)
    # the half cloak behind, fur at its collar
    f.box("extra:spine:cloak", -4, -2, b.z_pelvis - 2, 5, -1, b.z_head, CLOTH)
    f.box("extra:spine:cloak", -5, -2, b.z_head - 2, 6, 5, b.z_head, FUR)
    f.box("spine", -3, 0, b.z_torso, 4, 4, b.z_torso + 1, LEATHER)
    return f, {DARK: (40, 34, 34), CLOTH: (60, 40, 70), LEATHER: (70, 48, 30), SKIN: (214, 170, 140), HAIR: (90, 70, 60),
               FUR: (220, 214, 200), GOLD: (210, 176, 90), TRIM: (120, 30, 40), TRIM_DARK: (84, 20, 28)}


RECIPES = {"knight": knight, "manatarms": manatarms, "archer": archer, "crossbowman": crossbowman, "norseman": norseman,
           "templar": templar, "landsknecht": landsknecht, "ranger": ranger, "executioner": executioner, "friar": friar,
           "squire": squire, "lord": lord}


# ── shading ─────────────────────────────────────────────────────────────────

def hash3(x, y, z):
    h = (x * 73856093) ^ (y * 19349663) ^ (z * 83492791)
    h = (h ^ (h >> 13)) * 1274126177
    return ((h ^ (h >> 16)) & 0xffff) / 65535.0


DARKER, LIGHTER = 16, 48


def mute(c, sat=0.85, gain=1.0):
    r, g, b = c
    grey = 0.3 * r + 0.59 * g + 0.11 * b
    return tuple(int(max(0, min(255, (grey + (v - grey) * sat) * gain))) for v in (r, g, b))


def shade(f):
    """A light pass at edges: undersides a shade darker, upper edges a shade lighter, a few voxels of grain. Trim and lights stay flat."""
    solid = set()
    for p in f.parts.values():
        solid.update(p)
    for p in f.parts.values():
        for (x, y, z), c in list(p.items()):
            if not (1 <= c <= 15):
                continue
            side_open = any((x + dx, y + dy, z) not in solid for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            if (x, y, z - 1) not in solid:
                p[(x, y, z)] = c + DARKER
            elif (x, y, z + 1) not in solid and side_open:
                p[(x, y, z)] = c + LIGHTER
            elif hash3(x, y, z) < 0.05:
                p[(x, y, z)] = c + DARKER


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
        body_ = b"".join(bytes((x - x0, y - y0, z - z0, c)) for (x, y, z), c in sorted(shell.items()))
        models.append(chunk("SIZE", i32(sx) + i32(sy) + i32(sz)) + chunk("XYZI", i32(len(shell)) + body_))
        t = "%d %d %d" % (x0 + sx // 2, y0 + sy // 2, z0 + sz // 2)
        nodes.append(chunk("nTRN", i32(2 + 2 * k) + dct({"_name": name}) + i32(3 + 2 * k) + i32(-1) + i32(0) + i32(1) + dct({"_t": t})))
        nodes.append(chunk("nSHP", i32(3 + 2 * k) + dct({}) + i32(1) + i32(k) + dct({})))
    rgba = bytearray(1024)
    for i, (r, g, b) in colours.items():
        rgba[4 * (i - 1): 4 * i] = bytes((r, g, b, 255))
    mats = b"".join(chunk("MATL", i32(i) + dct({"_type": "_emit", "_emit": "%.2f" % e, "_rough": "0.4"})) for i, e in sorted(glow.items()))
    body_ = b"".join(models) + b"".join(nodes) + chunk("RGBA", bytes(rgba)) + mats
    return b"VOX " + i32(150) + chunk("MAIN", b"", body_)


def build(cid):
    f, colours = RECIPES[cid]()
    for name in list(f.parts):
        if not f.parts[name]:
            raise ValueError("%s: part %s is empty" % (cid, name))
    shade(f)
    for i in range(1, 16):
        if i in colours:
            base = mute(colours[i])
            colours[i] = base
            colours[i + DARKER] = tuple(int(v * 0.78) for v in base)
            colours[i + LIGHTER] = tuple(min(255, int(v * 1.14 + 6)) for v in base)
    used = {c for p in f.parts.values() for c in p.values()}
    for c in used:
        colours.setdefault(c, (255, 0, 255))   # a colour a recipe forgot shows magenta
    glow = {EYE: 0.4} if EYE in used else {}
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
