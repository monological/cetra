#!/usr/bin/env python3
"""Generate assets/emissive_fixture.gltf -- the derived-area-light instrument (spec 11.49).

Three emitters, each present because the probe has a different question to answer
about it and nothing else in the corpus asks it:

  emissive_quad   A flat quad carrying an emissive TEXTURE that is exactly half
                  black and half white. Its linear mean is 0.5 by construction,
                  so the 1x1-top-mip readback has an answer known in advance
                  rather than one measured off itself. Nothing else in the corpus
                  has an emissive texture at all, so without this the mean path
                  ships unexercised.

  emissive_box    A closed cube. Its faces cancel, so the area-weighted normal is
                  ~0 and planarity rejects it. The v1 limit -- one rectangle per
                  mesh -- stated as a test rather than as a promise.

  emissive_strip  A long thin quad rotated 30 degrees IN ITS OWN PLANE, which is
                  the case a covariance principal axis gets right and an
                  axis-aligned bound does not. It pins the minimum-area search:
                  the true rectangle is 2.0 x 0.25, and a bound taken on the
                  reference frame instead reads 1.8571 x 1.2165.

The quad's base colour is black, which is the unlit-flat-colour idiom this spec
found is how most emissive in the wild is authored -- so the fixture also carries
one honest instance of the thing that moved the default.
"""

import base64
import json
import math
import os
import struct
import zlib


def png_rgb(width, height, rows):
    """A minimal 8-bit RGB PNG. rows[y] is a list of (r, g, b)."""
    raw = b""
    for y in range(height):
        raw += b"\x00"  # filter type 0
        for (r, g, b) in rows[y]:
            raw += bytes((r, g, b))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def half_black_half_white(size=64):
    """Left half black, right half white. Linear mean is exactly 0.5."""
    rows = []
    for _ in range(size):
        rows.append([(0, 0, 0)] * (size // 2) + [(255, 255, 255)] * (size // 2))
    return png_rgb(size, size, rows)


class Geo:
    def __init__(self):
        self.pos = []
        self.nrm = []
        self.uv = []
        self.idx = []
        self.groups = []

    def begin(self, name):
        self._name = name
        self._first = len(self.idx)

    def end(self):
        self.groups.append((self._name, self._first, len(self.idx) - self._first))

    def quad(self, a, b, c, d, uvs=None):
        base = len(self.pos)
        ux = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        vx = (d[0] - a[0], d[1] - a[1], d[2] - a[2])
        n = (ux[1] * vx[2] - ux[2] * vx[1],
             ux[2] * vx[0] - ux[0] * vx[2],
             ux[0] * vx[1] - ux[1] * vx[0])
        ln = max((n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5, 1e-9)
        n = (n[0] / ln, n[1] / ln, n[2] / ln)
        if uvs is None:
            uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
        for p, t in zip((a, b, c, d), uvs):
            self.pos.append(p)
            self.nrm.append(n)
            self.uv.append(t)
        self.idx += [base, base + 1, base + 2, base, base + 2, base + 3]

    def box(self, cx, cy, cz, hx, hy, hz):
        x0, x1 = cx - hx, cx + hx
        y0, y1 = cy - hy, cy + hy
        z0, z1 = cz - hz, cz + hz
        # Wound outward on all six faces: the sum of n_i * A_i is then zero,
        # which is exactly what the planarity test must reject.
        self.quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))  # +Z
        self.quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0))  # -Z
        self.quad((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1))  # +X
        self.quad((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0))  # -X
        self.quad((x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0))  # +Y
        self.quad((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1))  # -Y


STRIP_ANGLE_DEG = 30.0
STRIP_HALF_LONG = 1.0
STRIP_HALF_SHORT = 0.125


def build():
    g = Geo()

    g.begin("emissive_quad")
    g.quad((-0.6, 0.4, 0.0), (0.6, 0.4, 0.0), (0.6, 1.6, 0.0), (-0.6, 1.6, 0.0))
    g.end()

    g.begin("emissive_box")
    g.box(2.0, 1.0, 0.0, 0.3, 0.3, 0.3)
    g.end()

    # Rotated in its own plane (z = 0), so no axis of the reference frame lines
    # up with either side. Corners written out rather than rotated by a matrix,
    # so the file states the answer the gate checks.
    th = math.radians(STRIP_ANGLE_DEG)
    ct, st = math.cos(th), math.sin(th)

    def rot(u, v):
        return (-2.0 + u * ct - v * st, 1.0 + u * st + v * ct, 0.0)

    g.begin("emissive_strip")
    g.quad(rot(-STRIP_HALF_LONG, -STRIP_HALF_SHORT), rot(STRIP_HALF_LONG, -STRIP_HALF_SHORT),
           rot(STRIP_HALF_LONG, STRIP_HALF_SHORT), rot(-STRIP_HALF_LONG, STRIP_HALF_SHORT))
    g.end()
    return g


def emit(path, g):
    pos = b"".join(struct.pack("<3f", *p) for p in g.pos)
    nrm = b"".join(struct.pack("<3f", *n) for n in g.nrm)
    uv = b"".join(struct.pack("<2f", *t) for t in g.uv)
    idx = b"".join(struct.pack("<I", i) for i in g.idx)
    png = half_black_half_white()

    buf = pos + nrm + uv + idx
    mn = [min(p[i] for p in g.pos) for i in range(3)]
    mx = [max(p[i] for p in g.pos) for i in range(3)]

    views = [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos), "byteLength": len(nrm), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos) + len(nrm), "byteLength": len(uv), "target": 34962},
    ]
    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": len(g.pos), "type": "VEC3",
         "min": mn, "max": mx},
        {"bufferView": 1, "componentType": 5126, "count": len(g.nrm), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(g.uv), "type": "VEC2"},
    ]
    off = len(pos) + len(nrm) + len(uv)
    for gi, (_, first, count) in enumerate(g.groups):
        views.append({"buffer": 0, "byteOffset": off + first * 4,
                      "byteLength": count * 4, "target": 34963})
        accessors.append({"bufferView": 3 + gi, "componentType": 5125,
                          "count": count, "type": "SCALAR"})

    # The quad is textured; the other two carry the factor alone, so a reader can
    # tell the mean path's contribution from the factor's without a second file.
    materials = [
        {"name": "emissive_textured",
         "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 1.0},
         "emissiveFactor": [1.0, 1.0, 1.0],
         "emissiveTexture": {"index": 0}},
        {"name": "emissive_solid",
         "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 1.0},
         "emissiveFactor": [1.0, 0.9, 0.8]},
        {"name": "emissive_strip_mat",
         "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 1.0},
         "emissiveFactor": [0.8, 0.85, 1.0]},
    ]

    doc = {
        "asset": {"version": "2.0", "generator": "gen_emissive_fixture.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(g.groups)))}],
        "nodes": [{"name": n, "mesh": i} for i, (n, _, _) in enumerate(g.groups)],
        "meshes": [
            {"name": n, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                                        "indices": 3 + i, "material": i}]}
            for i, (n, _, _) in enumerate(g.groups)
        ],
        "materials": materials,
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "images": [{"uri": "data:image/png;base64," + base64.b64encode(png).decode(),
                    "mimeType": "image/png"}],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(buf),
                     "uri": "data:application/octet-stream;base64," +
                            base64.b64encode(buf).decode()}],
    }
    with open(path, "w") as f:
        json.dump(doc, f, indent=1)
    print("wrote", path)


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    emit(os.path.join(here, "emissive_fixture.gltf"), build())
