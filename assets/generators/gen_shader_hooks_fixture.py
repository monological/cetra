#!/usr/bin/env python3
"""Generate the shader-hook instrument (spec 13.29): one scene with a piece at every place an
app's own GLSL can run, each built so the gate can tell from the frame that the shader ran
where it says.

    floor, wall   the ground and a back wall, far enough that a depth-of-field focused on the
                  row of pieces blurs the wall
    stripes       a box whose albedo is a surface hook's stripes          (main pass, surface)
    key_a, key_b  two boxes, two hooks, one feature mask: each keeps its own pattern
    param_a, _b   two boxes sharing one hook, told apart only by a param
    card          a card standing on the floor, holed by its hook's alpha; the sun throws the
                  holes onto the floor behind it                           (surface alpha)
    dome          a flat grid raised into a dome by its hook's offset      (vertex offset)
    static        a quad on the wall drawn in the late lane, a field of new noise every frame,
                  with a post standing in front of part of it             (late lane)
    marks         a flat rectangle painted by a post pass at each of the three locations, side
                  by side over the wall                                    (post passes)

Every shader includes an engine chunk, so the runtime include resolver is what compiles them.

Regenerate with: python3 assets/generators/gen_shader_hooks_fixture.py
"""

import base64
import json
import math
import struct
from fixture_paths import asset_path, asset_ref

# ------------------------------------------------------------------------------------------------
# Geometry: three shapes, each packed once and drawn by as many nodes as use it.

def box_shape():
    """A unit cube centred on the origin, a quad per face so each face carries its own normal,
    and UVs spanning each face."""
    faces = [
        ((1, 0, 0), [(0.5, -0.5, 0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (0.5, 0.5, 0.5)]),
        ((-1, 0, 0), [(-0.5, -0.5, -0.5), (-0.5, -0.5, 0.5), (-0.5, 0.5, 0.5), (-0.5, 0.5, -0.5)]),
        ((0, 1, 0), [(-0.5, 0.5, 0.5), (0.5, 0.5, 0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5)]),
        ((0, -1, 0), [(-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, -0.5, 0.5), (-0.5, -0.5, 0.5)]),
        ((0, 0, 1), [(-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5)]),
        ((0, 0, -1), [(0.5, -0.5, -0.5), (-0.5, -0.5, -0.5), (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5)]),
    ]
    pos, nrm, uv, idx = [], [], [], []
    for n, quad in faces:
        base = len(pos)
        pos += quad
        nrm += [n] * 4
        uv += [(0, 1), (1, 1), (1, 0), (0, 0)]
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, nrm, uv, idx


def grid_shape(cells):
    """A unit square in XZ, facing +Y, centred on the origin, `cells` by `cells`: the vertices
    an offset hook has to move."""
    pos, nrm, uv, idx = [], [], [], []
    for j in range(cells + 1):
        for i in range(cells + 1):
            u, v = i / cells, j / cells
            pos.append((u - 0.5, 0.0, v - 0.5))
            nrm.append((0, 1, 0))
            uv.append((u, v))
    for j in range(cells):
        for i in range(cells):
            a = j * (cells + 1) + i
            b, c, d = a + 1, a + cells + 2, a + cells + 1
            idx += [a, d, c, a, c, b]
    return pos, nrm, uv, idx


def quad_shape():
    """A unit square in XY facing +Z, centred on the origin, UV 0..1 across it."""
    pos = [(-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, 0.5, 0)]
    return pos, [(0, 0, 1)] * 4, [(0, 1), (1, 1), (1, 0), (0, 0)], [0, 1, 2, 0, 2, 3]


SHAPES = {"box": box_shape(), "grid": grid_shape(48), "quad": quad_shape()}

# ------------------------------------------------------------------------------------------------
# The scene, in metres. The camera looks down -z at a row of pieces standing near z = 0, with the
# wall 13 m away; a focus distance of 8.6 m keeps the row sharp and the wall soft.

CAMERA = {"eye": [0.0, 2.6, 8.5], "target": [0.0, 1.0, 0.0], "fov": 45.0}

# Name -> base colour, roughness, and (for the card) a mask cutoff.
MATERIALS = {
    "hooks_floor": ([0.45, 0.45, 0.45, 1.0], 0.8),
    "hooks_wall": ([0.35, 0.36, 0.40, 1.0], 0.9),
    "hooks_stripes": ([0.7, 0.7, 0.7, 1.0], 0.6),
    "hooks_key_a": ([0.7, 0.7, 0.7, 1.0], 0.6),
    "hooks_key_b": ([0.7, 0.7, 0.7, 1.0], 0.6),
    "hooks_param_a": ([0.7, 0.7, 0.7, 1.0], 0.6),
    "hooks_param_b": ([0.7, 0.7, 0.7, 1.0], 0.6),
    "hooks_card": ([0.75, 0.6, 0.35, 1.0], 0.7, 0.5),
    "hooks_dome": ([0.3, 0.5, 0.7, 1.0], 0.5),
    "hooks_static": ([0.02, 0.02, 0.02, 1.0], 0.3),
    "hooks_post": ([0.25, 0.22, 0.2, 1.0], 0.8),
}


def piece(name, shape, material, centre, size):
    return {"name": name, "shape": shape, "material": material, "translation": list(centre),
            "scale": list(size)}


def box(name, material, lo, hi):
    centre = [(a + b) / 2 for a, b in zip(lo, hi)]
    size = [b - a for a, b in zip(lo, hi)]
    return piece(name, "box", material, centre, size)


# The wall's face, and the static quad's place on it.
WALL_Z = -4.8
STATIC_LO, STATIC_HI = (-5.2, 2.4), (-2.6, 4.2)

PIECES = [
    box("floor", "hooks_floor", (-7.0, -0.1, -5.0), (7.0, 0.0, 4.0)),
    box("wall", "hooks_wall", (-7.0, 0.0, -5.0), (7.0, 5.0, WALL_Z)),
    box("stripes", "hooks_stripes", (-4.6, 0.0, -1.0), (-3.4, 1.2, 0.2)),
    box("key_a", "hooks_key_a", (-2.9, 0.0, -0.5), (-2.1, 0.8, 0.3)),
    box("key_b", "hooks_key_b", (-1.9, 0.0, -0.5), (-1.1, 0.8, 0.3)),
    box("param_a", "hooks_param_a", (-0.8, 0.0, -0.5), (0.0, 0.8, 0.3)),
    box("param_b", "hooks_param_b", (0.2, 0.0, -0.5), (1.0, 0.8, 0.3)),
    box("card", "hooks_card", (1.6, 0.0, -0.62), (2.8, 1.4, -0.58)),
    piece("dome", "grid", "hooks_dome", (4.2, 0.005, -0.4), (1.6, 1.0, 1.6)),
    piece("static", "quad", "hooks_static",
          ((STATIC_LO[0] + STATIC_HI[0]) / 2, (STATIC_LO[1] + STATIC_HI[1]) / 2, WALL_Z + 0.01),
          (STATIC_HI[0] - STATIC_LO[0], STATIC_HI[1] - STATIC_LO[1], 1.0)),
    box("post", "hooks_post", (-4.2, 0.0, -3.4), (-3.7, 3.6, -2.9)),
]

# Where each post pass paints its mark, in the frame's 0..1 (GL's, so y runs up), over the
# wall's top right: far enough that the depth of field blurs anything drawn before it.
MARKS = {
    "beforeDof": [0.55, 0.80, 0.62, 0.88],
    "beforeBloom": [0.68, 0.80, 0.75, 0.88],
    "afterTonemap": [0.81, 0.80, 0.88, 0.88],
}
# Scene-referred light for the two HDR marks, bright enough to bloom; display codes for the one
# after the tone map, which reaches the window as written -- whole codes, so an undithered frame
# holds them exactly.
MARK_HDR = [6.0, 6.0, 6.0, 0.0]
MARK_DISPLAY = [round(64 / 255, 6), round(128 / 255, 6), round(192 / 255, 6), 0.0]

# ------------------------------------------------------------------------------------------------
# Shaders, each written beside the scene in assets/shaders/.

SHADERS = {
    "hooks_post_mark.glsl": """#version 330 core

// A post pass that paints a flat rectangle over the frame (spec 13.29's fixture): `markRect` is
// x0, y0, x1, y1 in the frame's 0..1, `markColor` what goes inside it. Everything outside is the
// frame, copied texel for texel, so a pass whose rectangle is empty is the identity.

in vec2 TexCoords;
out vec4 FragColor;

// Unused here; included so the runtime resolver is what this shader compiles through.
#include "color.glsl"

uniform sampler2D sceneColor;
uniform vec4 markRect;
uniform vec4 markColor;

void main()
{
    vec3 color = texelFetch(sceneColor, ivec2(gl_FragCoord.xy), 0).rgb;
    bool inside = all(greaterThanEqual(TexCoords, markRect.xy)) && all(lessThan(TexCoords, markRect.zw));
    FragColor = vec4(inside ? markColor.rgb : color, 1.0);
}
""",
}

# ------------------------------------------------------------------------------------------------


def pack(fmt, rows):
    return b"".join(struct.pack(fmt, *row) for row in rows)


def bounds(points):
    return ([min(p[i] for p in points) for i in range(3)],
            [max(p[i] for p in points) for i in range(3)])


def gltf():
    data, views, accessors, meshes = b"", [], [], []

    def add(blob, kind, count, component, target, b=None):
        nonlocal data
        while len(data) % 4:
            data += b"\0"
        views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(blob),
                      "target": target})
        data += blob
        acc = {"bufferView": len(views) - 1, "componentType": component, "count": count,
               "type": kind}
        if b:
            acc["min"], acc["max"] = b
        accessors.append(acc)
        return len(accessors) - 1

    shape_attrs = {}
    for name, (pos, nrm, uv, idx) in SHAPES.items():
        attrs = {
            "POSITION": add(pack("<3f", pos), "VEC3", len(pos), 5126, 34962, bounds(pos)),
            "NORMAL": add(pack("<3f", [tuple(map(float, n)) for n in nrm]), "VEC3", len(nrm),
                          5126, 34962),
            "TEXCOORD_0": add(pack("<2f", [tuple(map(float, t)) for t in uv]), "VEC2", len(uv),
                              5126, 34962),
        }
        index = add(pack("<I", [(i,) for i in idx]), "SCALAR", len(idx), 5125, 34963)
        shape_attrs[name] = (attrs, index)

    materials = list(MATERIALS)
    nodes = []
    for p in PIECES:
        attrs, index = shape_attrs[p["shape"]]
        meshes.append({"name": p["name"], "primitives": [{
            "attributes": attrs, "indices": index, "material": materials.index(p["material"])}]})
        nodes.append({"name": p["name"], "mesh": len(meshes) - 1,
                      "translation": p["translation"], "scale": p["scale"]})

    materials_out = []
    for m in materials:
        spec = MATERIALS[m]
        out = {"name": m, "pbrMetallicRoughness": {
            "baseColorFactor": spec[0], "metallicFactor": 0.0, "roughnessFactor": spec[1]}}
        if len(spec) > 2:
            out["alphaMode"] = "MASK"
            out["alphaCutoff"] = spec[2]
            out["doubleSided"] = True
        materials_out.append(out)

    return {
        "asset": {"version": "2.0", "generator": "gen_shader_hooks_fixture.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials_out,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"uri": "data:application/octet-stream;base64,"
                     + base64.b64encode(data).decode("ascii"), "byteLength": len(data)}],
    }


def normalized(v):
    n = math.sqrt(sum(c * c for c in v))
    return [round(c / n, 6) for c in v]


def scene():
    passes = []
    for at, rect in MARKS.items():
        colour = MARK_DISPLAY if at == "afterTonemap" else MARK_HDR
        passes.append({"at": at, "shader": asset_ref("hooks_post_mark.glsl"),
                       "params": {"markRect": rect, "markColor": colour}})
    return {
        "version": 1,
        "_comment": [
            "The shader-hook instrument (spec 13.29): a piece at every place an app's own GLSL",
            "can run, each built so a gate can read from the frame that it ran where it says.",
            "",
            "Regenerate with: python3 assets/generators/gen_shader_hooks_fixture.py",
        ],
        "models": [{"path": asset_ref("shader_hooks_fixture.gltf")}],
        "environment": {"ambient": [0.12, 0.12, 0.14]},
        "lights": [{
            "name": "sun",
            "type": "directional",
            "direction": normalized([-0.3, -1.0, -0.7]),
            "color": [1.0, 0.97, 0.92],
            "intensity": 3.0,
            "cast_shadows": True,
        }],
        "post": {"exposure": 1.0, "passes": passes},
        "camera": CAMERA,
    }


def main():
    with open(asset_path("shader_hooks_fixture.gltf"), "w") as f:
        json.dump(gltf(), f, indent=1)
        f.write("\n")
    with open(asset_path("shader_hooks_fixture.cscn"), "w") as f:
        json.dump(scene(), f, indent=1)
        f.write("\n")
    for name, text in SHADERS.items():
        with open(asset_path(name), "w") as f:
            f.write(text)


if __name__ == "__main__":
    main()
