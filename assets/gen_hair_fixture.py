#!/usr/bin/env python3
"""Generate assets/hair_fixture.*, the strand-map test asset (spec 11.20 / roadmap B8).

    python3 assets/gen_hair_fixture.py

Writes the atlas, derives the strand map from it with tools/gen_hair_flow.py,
checks the derivation against the angles it painted, and emits the glTF and the
four scene files the gate renders.

WHAT THIS IS FOR

Hair cards carry ONE tangent per quad, so an anisotropic lobe keyed on it fires
across the whole card. The fix is a per-texel strand map. The question a gate
has to answer is therefore not "does hair look right" -- it is "is the shader
reading the map at all, or is it inventing strand data?", because an invention
that correlates with nothing still produces a plausible-looking frame. An
earlier revision hashed the texel coordinate and looked merely bad, not wrong.

THE ASSERTION, AND WHY IT CANNOT BE PASSED BY ACCIDENT

One quad. One material. One draw call. One tangent. The atlas paints strands
running ALONG the card tangent in the left half and ACROSS it in the right
half, and the key light sits off to the side in X:

    left  strands ~parallel to L  ->  sin(T,L) small  ->  lobe dark
    right strands perpendicular   ->  sin(T,L) = 1    ->  lobe bright

Geometry, normal, material, light and tangent are IDENTICAL in the two halves,
so nothing but the map can separate them. Switching hairShading off is the
control: GGX sees one flat quad and both halves must match.

That falsifies the coordinate hash too. A hash keyed on the texel coordinate
varies WITHIN each half but has the same distribution in both, so it moves the
variance and leaves the ratio at 1 -- which is exactly the wrong answer this
fixture exists to catch.

WHY THE STRANDS ARE IN LUMINANCE AND THE CARD IS OPAQUE

Two reasons. Alpha stays 1 everywhere so the quad is opaque and the measurement
is pure shading, with no transparency to confound it. And the raiden groom
carries its strands in ALPHA, so putting these in luminance exercises the other
half of the tool's summed structure tensor -- between them the two cover both
kinds of atlas.
"""

import base64
import importlib.util
import io
import json
import os
import struct

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ATLAS = os.path.join(HERE, "hair_fixture_atlas.png")
FLOW = os.path.join(HERE, "hair_fixture_flow.png")
GLTF = os.path.join(HERE, "hair_fixture.gltf")

SIZE = 512
SPACING = 7.0  # texels between strand centres
WIDTH = 1.6    # gaussian sigma across a strand, in texels
SEAM = 24      # texels either side of the half boundary the gate must ignore


def paint_atlas():
    """Strands along u in the left half, along v in the right half."""
    rng = np.random.default_rng(11)
    yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64)

    def bands(across):
        """Gaussian ridges repeating every SPACING along `across`."""
        index = np.floor(across / SPACING)
        centre = (index + 0.5) * SPACING
        ridge = np.exp(-0.5 * ((across - centre) / WIDTH) ** 2)
        # Per-strand brightness variation. Not decoration: a perfectly uniform
        # set of ridges gives the structure tensor no reason to prefer one
        # strand over another, and real hair is not uniform either.
        strand_gain = rng.uniform(0.75, 1.0, size=int(SIZE / SPACING) + 2)
        return ridge * strand_gain[index.astype(int)]

    left = bands(yy)   # ridges stacked along v  -> strands run along u
    right = bands(xx)  # ridges stacked along u  -> strands run along v
    lum = np.where(xx < SIZE / 2, left, right) * 0.40 + 0.15

    rgba = np.zeros((SIZE, SIZE, 4), dtype=np.uint8)
    rgba[..., :3] = np.round(np.clip(lum, 0.0, 1.0) * 255.0)[..., None]
    rgba[..., 3] = 255  # opaque card: this fixture measures shading, not coverage
    Image.fromarray(rgba, "RGBA").save(ATLAS)


def derive_flow():
    """Run the real tool, then check it against the angles we painted."""
    spec = importlib.util.spec_from_file_location(
        "gen_hair_flow", os.path.join(HERE, os.pardir, "tools", "gen_hair_flow.py"))
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    tool.main([ATLAS, "-o", FLOW])

    # Ground truth is known because we painted it, so the fixture verifies the
    # tool rather than trusting it. Without this the gate could pass on a map
    # that is confidently wrong in both halves.
    m = np.asarray(Image.open(FLOW).convert("RGBA")).astype(np.float64) / 255.0
    doubled = m[..., :2] * 2.0 - 1.0
    angle = 0.5 * np.arctan2(doubled[..., 1], doubled[..., 0])
    coherence = np.hypot(doubled[..., 0], doubled[..., 1])

    half = SIZE // 2
    checks = (("left", np.s_[:, SEAM:half - SEAM], 0.0),
              ("right", np.s_[:, half + SEAM:SIZE - SEAM], np.pi / 2))
    for name, sel, want in checks:
        sub, coh = angle[sel], coherence[sel]
        good = coh > 0.5
        if good.mean() < 0.5:
            raise SystemExit("%s half: only %.0f%% of texels are confident" %
                             (name, 100 * good.mean()))
        # Compare as ORIENTATIONS (mod pi), the only thing a strand defines.
        err = np.abs(np.angle(np.exp(2j * (sub[good] - want)))) * 0.5
        if np.degrees(np.median(err)) > 5.0:
            raise SystemExit("%s half: derived orientation is %.1f deg off the painted %.0f deg" %
                             (name, np.degrees(np.median(err)), np.degrees(want)))
        print("  %-5s half: %.1f deg from painted, %.0f%% confident"
              % (name, np.degrees(np.median(err)), 100 * good.mean()))


def write_gltf():
    # A 2x2 quad facing +Z. UV origin top-left (v down), tangent along +X, so
    # the card tangent is the atlas's u axis and "left half" means u < 0.5.
    positions = [(-1.0, 1.0, 0.0), (1.0, 1.0, 0.0), (1.0, -1.0, 0.0), (-1.0, -1.0, 0.0)]
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    normals = [(0.0, 0.0, 1.0)] * 4
    tangents = [(1.0, 0.0, 0.0, 1.0)] * 4
    indices = [0, 3, 2, 0, 2, 1]  # CCW seen from +Z

    pos_b = b"".join(struct.pack("<3f", *p) for p in positions)
    nrm_b = b"".join(struct.pack("<3f", *n) for n in normals)
    uv_b = b"".join(struct.pack("<2f", *u) for u in uvs)
    tan_b = b"".join(struct.pack("<4f", *t) for t in tangents)
    idx_b = b"".join(struct.pack("<H", i) for i in indices)
    blob = pos_b + nrm_b + uv_b + tan_b + idx_b

    views, offset = [], 0
    for data in (pos_b, nrm_b, uv_b, tan_b, idx_b):
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data)})
        offset += len(data)
    views[4]["target"] = 34963

    gltf = {
        "asset": {"version": "2.0", "generator": "gen_hair_fixture.py"},
        "samplers": [{"wrapS": 33071, "wrapT": 33071}],
        "images": [{"uri": os.path.basename(ATLAS)}],
        "textures": [{"source": 0, "sampler": 0}],
        "materials": [{
            "name": "hair_card",
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": 0},
                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.30,
            },
            "doubleSided": True,
        }],
        "meshes": [{"name": "card", "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "TANGENT": 3},
            "indices": 4, "material": 0}]}],
        "nodes": [{"name": "card", "mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
             "min": [-1.0, -1.0, 0.0], "max": [1.0, 1.0, 0.0]},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC4"},
            {"bufferView": 4, "componentType": 5123, "count": 6, "type": "SCALAR"},
        ],
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob),
                     "uri": "data:application/octet-stream;base64," +
                            base64.b64encode(blob).decode("ascii")}],
    }
    with open(GLTF, "w") as f:
        json.dump(gltf, f, indent=1)
        f.write("\n")


# The key sits off to the side in X so the two halves' strand directions make
# different angles with L. Head-on light would give sin(T,L) = 1 for every
# in-plane tangent and the halves would match however well the map worked.
LIGHT = {"name": "key", "type": "directional",
         "direction": [-0.7071, 0.0, -0.7071],
         "color": [1.0, 1.0, 1.0], "intensity": 3.0}

BASE_HAIR = {"hairShading": 1.0, "hairRoughness": 0.30, "hairShift": 0.05,
             "hairTint": [0.80, 0.78, 0.72], "hairBacklit": 0.35,
             "hairMap": "hair_fixture_flow.png"}


def write_cscn(name, comment, drop=(), **hair):
    material = dict(BASE_HAIR)
    material.update(hair)
    for key in drop:
        material.pop(key, None)
    doc = {
        "version": 1,
        "_comment": comment,
        "models": [{"path": "hair_fixture.gltf"}],
        "materials": {"hair_card": material},
        "lights": [LIGHT],
        "camera": {"eye": [0.0, 0.0, 3.2], "target": [0.0, 0.0, 0.0], "fov": 45},
        "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
    }
    with open(os.path.join(HERE, name), "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    paint_atlas()
    derive_flow()
    write_gltf()
    write_cscn("hair_fixture.cscn",
               "Hair lobes driven by the derived strand map. The reference arm.")
    write_cscn("hair_fixture_ggx.cscn",
               "Control: the same card with hair shading off. GGX sees one flat "
               "quad, so the two halves must match -- which is what makes a "
               "difference in the hair arm attributable to the map.",
               hairShading=0.0)
    write_cscn("hair_fixture_nomap.cscn",
               "The same lobes with NO strand map, which is the state the "
               "feature is worthless in: one tangent for the whole quad, so the "
               "halves match however the atlas was painted. This arm is what "
               "makes the reference arm's split attributable to the map rather "
               "than to the lobes, and it is the shape the coordinate hash this "
               "replaced would have measured as.",
               drop=("hairMap",))
    write_cscn("hair_fixture_nojitter.cscn",
               "Strand identity disabled. Isolates what the per-strand shift "
               "offset contributes from what the orientation contributes.",
               hairJitter=0.0)
    write_cscn("hair_fixture_noshift.cscn",
               "Cuticle tilt zeroed, which collapses R and TRT onto each other. "
               "Separates the two-lobe behaviour from the single-lobe one.",
               hairShift=0.0)
    print("wrote hair_fixture atlas, flow map, gltf and 4 scene files")
