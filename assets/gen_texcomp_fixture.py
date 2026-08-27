#!/usr/bin/env python3
"""Generate the block-compression instrument -- assets/texcomp_fixture.* (spec 11.85).

A LONG PLANE RECEDING FROM THE CAMERA, and the receding is the whole point.

Every other fixture in this corpus sits near the camera and never minifies, so
none of them samples a mip level above 0. That is not a guess: painting every
mip level of every texture SOLID BLACK leaves all 28 goldens at 0 px, and leaves
the raiden recipe this repo calls its best baseline at 0 px too. A compression
change lives in the mip chain as much as in level 0 -- the chain is where most of
the bytes are -- so a fixture that cannot see mips cannot see this feature.

This plane runs from 1 to 400 units out under a 40-degree camera set low and
looking just above it, so one frame sweeps from a few texels per pixel at the
bottom edge to hundreds at the vanishing point -- which IS the mip chain.

THE THREE MAPS ARE THE THREE FORMATS, one each, so an arm can attribute a change:

  albedo     a HUE WHEEL over a smooth luminance ramp, 3 channels.
             DXT1's endpoints quantise to RGB565 and its palette is four colours
             per 4x4 block, so its error is worst where a block spans several
             hues at similar luminance -- which a wheel guarantees and a photo
             does not. The smooth ramp underneath is the other half: a gradient
             is where RGB565 banding shows, and a fixture of flat patches would
             report DXT as free.
  normal     a RIDGE FIELD whose x and y slopes are INDEPENDENT.
             BC5 stores two channels with SEPARATE endpoints per block, so a map
             whose x and y are correlated -- a plain bump grid -- would read the
             same under a hypothetical shared-endpoint encoder and could not tell
             BC5 from a cheaper wrong thing. Amplitude is high enough that the
             reconstructed Z is well away from 1, since z = sqrt(1 - x^2 - y^2)
             is flat near the pole and a gentle map makes the rebuild untestable.
  roughness  a SINGLE-CHANNEL staircase, 1 channel on purpose.
             The loader only offers BC4 to a one-channel source, because a
             three-channel linear map is an ORM whose channels are different
             quantities. Writing this as grey RGB would silently exercise the
             DECLINE path while looking like it tested BC4.

Regenerate with: python3 assets/gen_texcomp_fixture.py
"""

import base64
import json
import os
import struct

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

TEX = 512          # power of two, so the chain halves cleanly to 1x1
NEAR, FAR = 1.0, 400.0
HALF_W = 200.0     # wide enough that the plane reaches both frame edges
# Texels per world unit, applied to BOTH axes so a texel stays square -- a UV
# that repeats the same count over a 400-long and 400-wide plane is fine, but one
# that repeats equally over unequal extents stretches the map and the far field
# then reports anisotropy rather than compression.
UV_PER_UNIT = 0.25


def _albedo(n):
    """Hue wheel over a luminance ramp -- DXT's worst case, by construction."""
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    hue = (x / n) * 6.0
    i = np.floor(hue).astype(int) % 6
    f = hue - np.floor(hue)
    ramp = 0.15 + 0.85 * (y / n)
    up, dn, on, off = f, 1.0 - f, np.ones_like(f), np.zeros_like(f)
    r = np.choose(i, [on, dn, off, off, up, on])
    g = np.choose(i, [up, on, on, dn, off, off])
    b = np.choose(i, [off, off, up, on, on, dn])
    rgb = np.stack([r, g, b], axis=-1) * ramp[..., None]
    return np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8)


def _normal(n):
    """Ridges whose x and y slopes are independent, at a real amplitude."""
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    # Different frequencies AND a phase offset, so no block sees dy as a function
    # of dx. Equal frequencies would correlate the two channels exactly.
    dx = np.sin(x / n * np.pi * 2.0 * 7.0) * 0.65
    dy = np.sin(y / n * np.pi * 2.0 * 11.0 + 1.3) * 0.65
    nz = np.sqrt(np.maximum(1.0 - dx * dx - dy * dy, 1e-6))
    v = np.stack([dx, dy, nz], axis=-1) * 0.5 + 0.5
    return np.clip(v * 255.0 + 0.5, 0, 255).astype(np.uint8)


def _roughness(n):
    """A staircase, single channel. Flat treads make BC4's endpoints exact."""
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    steps = np.floor(x / n * 8.0) + np.floor(y / n * 8.0)
    v = 0.1 + 0.8 * ((steps % 8.0) / 7.0)
    return np.clip(v * 255.0 + 0.5, 0, 255).astype(np.uint8)


albedo = _albedo(TEX)
normal = _normal(TEX)
rough = _roughness(TEX)

# The asserts are the fixture's own contract; each one failed at least once while
# this was being written.
assert albedo[..., 0].std() > 40 and albedo[..., 2].std() > 40, \
    "albedo must vary hard in more than one channel or DXT error cannot show"
nx = normal[..., 0].astype(float) / 255.0 * 2.0 - 1.0
ny = normal[..., 1].astype(float) / 255.0 * 2.0 - 1.0
assert abs(np.corrcoef(nx.ravel(), ny.ravel())[0, 1]) < 0.1, \
    "normal x and y must be independent or BC5's separate endpoints go untested"
assert np.abs(nx).max() > 0.5, "normal slope too gentle: the Z rebuild is flat near the pole"
assert rough.ndim == 2, "roughness must be single channel or the loader declines BC4"

Image.fromarray(albedo, "RGB").save(os.path.join(HERE, "texcomp_albedo.png"))
Image.fromarray(normal, "RGB").save(os.path.join(HERE, "texcomp_normal.png"))
Image.fromarray(rough, "L").save(os.path.join(HERE, "texcomp_rough.png"))

# --- geometry: one long quad, near edge at NEAR, far edge at FAR -------------
pos = [(-HALF_W, 0.0, -NEAR), (HALF_W, 0.0, -NEAR),
       (HALF_W, 0.0, -FAR), (-HALF_W, 0.0, -FAR)]
nrm = [(0.0, 1.0, 0.0)] * 4
tan = [(1.0, 0.0, 0.0, 1.0)] * 4
_u = 2.0 * HALF_W * UV_PER_UNIT
_v = (FAR - NEAR) * UV_PER_UNIT
uv = [(0.0, 0.0), (_u, 0.0), (_u, _v), (0.0, _v)]
idx = [0, 1, 2, 0, 2, 3]

_chunks = [
    (b"".join(struct.pack("<3f", *p) for p in pos), 34962),
    (b"".join(struct.pack("<3f", *n) for n in nrm), 34962),
    (b"".join(struct.pack("<4f", *t) for t in tan), 34962),
    (b"".join(struct.pack("<2f", *t) for t in uv), 34962),
    (b"".join(struct.pack("<H", i) for i in idx), 34963),
]
buf = b"".join(c for c, _ in _chunks)


def _views(chunks):
    views, off = [], 0
    for data, target in chunks:
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(data), "target": target})
        off += len(data)
    return views


mn = [min(p[i] for p in pos) for i in range(3)]
mx = [max(p[i] for p in pos) for i in range(3)]

GLTF = {
    "asset": {"version": "2.0", "generator": "gen_texcomp_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "texcomp_plane", "mesh": 0}],
    "meshes": [{"name": "texcomp_plane", "primitives": [
        {"attributes": {"POSITION": 0, "NORMAL": 1, "TANGENT": 2, "TEXCOORD_0": 3},
         "indices": 4, "material": 0}]}],
    "materials": [{
        "name": "texcomp_surface",
        "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0},
            "metallicRoughnessTexture": {"index": 2},
            "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
            "metallicFactor": 0.0, "roughnessFactor": 1.0},
        "normalTexture": {"index": 1},
    }],
    "textures": [{"source": 0}, {"source": 1}, {"source": 2}],
    "images": [{"uri": "texcomp_albedo.png"}, {"uri": "texcomp_normal.png"},
               {"uri": "texcomp_rough.png"}],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": mn, "max": mx},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC4"},
        {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 4, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": _views(_chunks),
    "buffers": [{"uri": "data:application/octet-stream;base64," +
                 base64.b64encode(buf).decode("ascii"), "byteLength": len(buf)}],
}

# Exposure is PINNED and the sun is fixed: every arm here is a comparison between
# two storage formats, so anything that could move the frame for a second reason
# has to be nailed down first.
CSCN = {
    "models": [{"path": "texcomp_fixture.gltf"}],
    # The key is "eye", NOT "position". A wrong key here parses, warns nowhere
    # useful and silently leaves the auto-framed camera in place -- which is how
    # the first two drafts of this fixture were read as badly framed rather than
    # as unapplied. The same trap decal_fixture records for "mode" and
    # beach_fixture for its wave model.
    #
    # 8 up looking at the surface 30 out is about 15 degrees of depression, so a
    # 50-degree field runs from roughly 10 units ahead to the vanishing point.
    # Aiming at the horizon instead leaves the plane a band across the middle and
    # most of the mip chain off screen.
    "camera": {"eye": [0.0, 8.0, 0.0], "target": [0.0, 0.0, -30.0], "fov": 50},
    # Pinned, and flat: every arm here compares two storage formats, so anything
    # that could move the frame for a second reason is nailed down first.
    "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
}

if __name__ == "__main__":
    with open(os.path.join(HERE, "texcomp_fixture.gltf"), "w") as f:
        json.dump(GLTF, f, indent=1)
    with open(os.path.join(HERE, "texcomp_fixture.cscn"), "w") as f:
        json.dump(CSCN, f, indent=1)
    print(f"wrote texcomp_fixture.gltf, texcomp_fixture.cscn and 3 maps at {TEX}x{TEX}")
