#!/usr/bin/env python3
"""Write strata_kit's test parts as MagicaVoxel files, test/fixtures/<name>.vox,
with their sidecars, test/fixtures/<name>.part.

The files follow the part convention (docs/2026-11-20-mech-construction-design.md):
a model named `part`, pieces `piece:<name>:<parent>` with markers `joint:<name>`,
one `attach`, mounts `mount:<slot>:<size>:<name>`, effects `fx:<name>`, a facing
and up suffix `@<axis>^<axis>`. A marker is a one-voxel model; its centre is
the point. The writer functions are copied from scripts/characters.py.

  python3 plugins/strata_kit/tools/make_fixtures.py
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "test", "fixtures")


def i32(v):
    return struct.pack("<i", v)


def string(s):
    b = s.encode()
    return i32(len(b)) + b


def dct(d):
    return i32(len(d)) + b"".join(string(k) + string(v) for k, v in d.items())


def chunk(tag, content, children=b""):
    return tag.encode() + i32(len(content)) + i32(len(children)) + content + children


def box(sx, sy, sz, c):
    return {(x, y, z): c for x in range(sx) for y in range(sy) for z in range(sz)}


def vox_bytes(models):
    """models: [(name, (x0, y0, z0) low corner in the file's axes, (sx, sy, sz), {(x, y, z): colour} local)]"""
    blobs, nodes = [], []
    kids = [2 + 2 * k for k in range(len(models))]
    nodes.append(chunk("nTRN", i32(0) + dct({}) + i32(1) + i32(-1) + i32(-1) + i32(1) + dct({})))
    nodes.append(chunk("nGRP", i32(1) + dct({}) + i32(len(kids)) + b"".join(i32(c) for c in kids)))
    for k, (name, (x0, y0, z0), (sx, sy, sz), vs) in enumerate(models):
        body = b"".join(bytes((x, y, z, c)) for (x, y, z), c in sorted(vs.items()))
        blobs.append(chunk("SIZE", i32(sx) + i32(sy) + i32(sz)) + chunk("XYZI", i32(len(vs)) + body))
        t = "%d %d %d" % (x0 + sx // 2, y0 + sy // 2, z0 + sz // 2)
        nodes.append(chunk("nTRN", i32(2 + 2 * k) + dct({"_name": name}) + i32(3 + 2 * k) + i32(-1) + i32(0) + i32(1) + dct({"_t": t})))
        nodes.append(chunk("nSHP", i32(3 + 2 * k) + dct({}) + i32(1) + i32(k) + dct({})))
    rgba = bytearray(1024)
    for i in range(1, 16):
        rgba[4 * (i - 1): 4 * i] = bytes((16 * i, 100, 200, 255))
    return b"VOX " + i32(150) + chunk("MAIN", b"", b"".join(blobs) + b"".join(nodes) + chunk("RGBA", bytes(rgba)))


def mark(name, x, y, z):
    return (name, (x, y, z), (1, 1, 1), {(0, 0, 0): 1})


def write(name, models, sidecar):
    with open(os.path.join(OUT, name + ".vox"), "wb") as f:
        f.write(vox_bytes(models))
    with open(os.path.join(OUT, name + ".part"), "w") as f:
        f.write(sidecar)


def main():
    os.makedirs(OUT, exist_ok=True)
    # an arm: a shoulder (the root), an upper arm, a forearm; a hand mount under it facing down the arm, a forearm hardpoint
    write("arm_m", [
        ("part", (0, 0, 18), (4, 4, 4), box(4, 4, 4, 3)),
        ("piece:upper:part", (0, 0, 10), (4, 4, 8), box(4, 4, 8, 4)),
        mark("joint:upper", 2, 2, 18),
        ("piece:fore:upper", (0, 0, 2), (4, 4, 8), box(4, 4, 8, 5)),
        mark("joint:fore", 2, 2, 10),
        mark("attach", 2, 2, 21),
        mark("mount:hand:L:hand_r@+z^+y", 2, 2, 2),
        mark("mount:forearm:L:forearm_r", 2, 0, 6),
    ], "# a medium arm\nslots = arm\nsize = M\nside = r\nweight = 9\npower = 2\n")
    # a light gun: a barrel along +y with a muzzle at its end
    write("gun_l", [
        ("part", (0, 0, 0), (2, 8, 2), box(2, 8, 2, 6)),
        mark("attach", 1, 1, 1),
        mark("fx:muzzle@+y^+z", 1, 7, 1),
    ], "slots = hand shoulder forearm\nsize = L\nweight = 4\n")
    # legs: a hip, and each leg a thigh, a shin and a foot
    models = [("part", (0, 0, 12), (8, 4, 2), box(8, 4, 2, 7)), mark("attach", 4, 2, 13)]
    for s, x in (("l", 0), ("r", 5)):
        models += [
            ("piece:thigh_%s:part" % s, (x, 0, 7), (3, 4, 5), box(3, 4, 5, 8)), mark("joint:thigh_%s" % s, x + 1, 2, 12),
            ("piece:shin_%s:thigh_%s" % (s, s), (x, 0, 1), (3, 4, 6), box(3, 4, 6, 8)), mark("joint:shin_%s" % s, x + 1, 2, 7),
            ("piece:foot_%s:shin_%s" % (s, s), (x, 0, 0), (3, 6, 1), box(3, 6, 1, 9)), mark("joint:foot_%s" % s, x + 1, 2, 1),
        ]
    write("legs_m", models, "slots = hip\nsize = M\nweight = 20\n")
    write("bad_two_attach", [("part", (0, 0, 0), (2, 2, 2), box(2, 2, 2, 1)), mark("attach", 0, 0, 0), mark("attach", 1, 1, 1)], "slots = x\nsize = L\n")
    write("bad_parent", [("part", (0, 0, 0), (2, 2, 2), box(2, 2, 2, 1)), mark("attach", 0, 0, 0),
                         ("piece:a:nope", (0, 0, 2), (2, 2, 2), box(2, 2, 2, 1)), mark("joint:a", 0, 0, 2)], "slots = x\nsize = L\n")
    print("make_fixtures: wrote 5 parts to %s" % os.path.relpath(OUT))


if __name__ == "__main__":
    main()
