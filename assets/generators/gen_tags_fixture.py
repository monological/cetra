#!/usr/bin/env python3
"""Generate the tags fixture (spec 14.39): surfaces to tag, by vertex, by node and by instance.

    floor   a light grey floor, its top at y = 0, to catch shadows and show what turns see-through.
    slab    a grey slab 1.2 x 0.1 x 1.0 m on the floor, standing in for a board's laminate.
    strips  ONE mesh of four separate copper strips along z on the slab, 0.1 m wide, 0.6 m long,
            0.2 m apart: the render app's --piece-tags gives each piece a tag of its own, so one
            mesh carries four objects by vertex.
    part0-3 four blue boxes 0.12 m on a side along x at z = 0.3, one mesh placed four times, so
            they draw as one instanced run: each node's tag is an instance's.
    cap     a small red box, a child of part0, which takes its parent's tag.
    key     a sun from the front left, casting; the ambient fills the shadows.

The strips are metal, so a matte paint over them reads as the paint and not as tinted metal.

Regenerate with: python3 assets/generators/gen_tags_fixture.py
"""

from fixture_paths import asset_ref
from gltf_pieces import CUBE_FACES, cube, gltf_of, write_json

STRIP_XS = (-0.3, -0.1, 0.1, 0.3)
STRIP_HALF = (0.05, 0.01, 0.3)  # half-extents: 0.1 wide, 0.02 tall, 0.6 long
STRIP_Y, STRIP_Z = 0.11, -0.15


def strips():
    """The four strips as one shape, at their own places, so the node's scale is 1."""
    positions, normals, indices = [], [], []
    for x in STRIP_XS:
        for n, quad in CUBE_FACES:
            base = len(positions)
            positions += [(x + 2.0 * STRIP_HALF[0] * px, STRIP_Y + 2.0 * STRIP_HALF[1] * py,
                           STRIP_Z + 2.0 * STRIP_HALF[2] * pz) for px, py, pz in quad]
            normals += [n] * 4
            indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, indices


SHAPES = {"cube": cube(), "strips": strips()}

# Name -> (base colour, metallic, roughness).
MATERIALS = {
    "tags_floor": ([0.6, 0.6, 0.6, 1.0], 0.0, 0.8),
    "tags_slab": ([0.35, 0.35, 0.33, 1.0], 0.0, 0.6),
    "tags_copper": ([0.95, 0.64, 0.54, 1.0], 1.0, 0.35),
    "tags_part": ([0.15, 0.25, 0.6, 1.0], 0.0, 0.5),
    "tags_cap": ([0.7, 0.12, 0.1, 1.0], 0.0, 0.5),
}

PART_XS = (-0.36, -0.12, 0.12, 0.36)
PART_SIZE, PART_Z = 0.12, 0.3

pieces = [
    ("floor", "cube", "tags_floor", (0.0, -0.05, 0.0), (4.0, 0.1, 4.0)),
    ("slab", "cube", "tags_slab", (0.0, 0.05, 0.0), (1.2, 0.1, 1.0)),
    ("strips", "strips", "tags_copper", (0.0, 0.0, 0.0), (1.0, 1.0, 1.0)),
]
pieces += [(f"part{k}", "cube", "tags_part", (x, 0.1 + PART_SIZE / 2, PART_Z),
            (PART_SIZE, PART_SIZE, PART_SIZE)) for k, x in enumerate(PART_XS)]
# The cap at its place in the world; made part0's child below.
CAP_CENTRE, CAP_SIZE = (PART_XS[0], 0.1 + PART_SIZE + 0.02, PART_Z), (0.06, 0.04, 0.06)
pieces.append(("cap", "cube", "tags_cap", CAP_CENTRE, CAP_SIZE))


def main():
    gltf = gltf_of("gen_tags_fixture.py", SHAPES, MATERIALS, pieces)
    # The cap under part0, its transform put in its parent's frame: the parent's scale divided out
    # of both its offset and its size.
    names = [n["name"] for n in gltf["nodes"]]
    parent, child = names.index("part0"), names.index("cap")
    p = gltf["nodes"][parent]
    c = gltf["nodes"][child]
    c["translation"] = [(c["translation"][i] - p["translation"][i]) / p["scale"][i]
                        for i in range(3)]
    c["scale"] = [c["scale"][i] / p["scale"][i] for i in range(3)]
    p["children"] = [child]
    gltf["scenes"][0]["nodes"] = [i for i in range(len(names)) if i != child]

    scene_desc = {
        "version": 1,
        "_comment": [
            "Surfaces to tag (spec 14.39): one mesh of four strips, one mesh placed four times, a",
            "child under a placed one, on a slab over a floor, under a casting sun.",
            "",
            "Regenerate with: python3 assets/generators/gen_tags_fixture.py",
        ],
        "models": [{"path": asset_ref("tags_fixture.gltf")}],
        "environment": {"ambient": [0.25, 0.25, 0.25]},
        "lights": [{"name": "key", "type": "directional", "direction": [0.45, -0.8, -0.4],
                    "color": [1.0, 1.0, 1.0], "intensity": 2.5, "cast_shadows": True}],
        "camera": {"eye": [0.0, 1.3, 1.25], "target": [0.0, 0.05, 0.05], "fov": 40.0},
        "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
    }

    write_json("tags_fixture.gltf", gltf)
    write_json("tags_fixture.cscn", scene_desc)
    print(f"wrote tags_fixture (.gltf + .cscn), {len(pieces)} pieces")


# The gate imports this for its constants, so the files are written only when it is run.
if __name__ == "__main__":
    main()
