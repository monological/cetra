#!/usr/bin/env python3
"""Generate the circuit board at true scale (spec 13.57): a board in metres, the way pcb draws one.

    board     100 x 60 mm of FR4, 1.6 mm thick, under a glossy green solder mask, its bottom at
              y = 0.
    passives  a row of eight 0402 parts along x at z = 12 mm, resistors (white alumina under a
              black overcoat) and capacitors (tan ceramic) in turn, 1.0 x 0.5 mm, on tin
              terminations over tin pads, under a few traces in the mask.
    qfn       a 5 x 5 mm QFN-32 at (22, 10) mm, eight tin lands a side at 0.5 mm pitch.
    bga       a 7 x 7 mm BGA at (22, -12) mm on an 8 x 8 grid of 0.5 mm balls at 0.8 mm pitch.
    table     a matte grey top 300 mm square under the board, its top at y = 0.
    lights    a tabletop studio: a key softbox to the front left, a larger fill to the right and a
              strip overhead behind, all area panels, and a point light 30 mm over the board.

Everything is in metres, nothing is scaled up: a 0402 is a millimetre long. The scene file's
camera is the three-quarter view; the other two are flags, at the views the twin gate and the
board's golden render:

    whole board  --cam-eye 0,0.11,0.075 --cam-target 0,0,0.004
    close-up     --cam-eye -0.00897,0.00292,0.01329 --cam-target -0.01,0.0018,0.012
                 (2 mm from the middle resistor's centre)

The studio is the scene file's own panels rather than an HDR, so the board needs nothing that is
not committed.

Regenerate with: python3 assets/generators/gen_board_fixture.py
"""

import math
from fixture_paths import asset_ref
from gltf_pieces import cube, gltf_of, sphere, write_json

MM = 0.001

SHAPES = {"cube": cube(), "sphere": sphere()}

# Name -> (base colour, metallic, roughness). The mask's coat and the scatter are the scene
# file's, since glTF carries neither the way the engine reads them.
MATERIALS = {
    "board_fr4": ([0.62, 0.55, 0.32, 1.0], 0.0, 0.7),
    "board_mask": ([0.04, 0.22, 0.07, 1.0], 0.0, 0.35),
    "board_mask_copper": ([0.10, 0.34, 0.11, 1.0], 0.0, 0.35),
    "board_tin": ([0.80, 0.80, 0.78, 1.0], 1.0, 0.3),
    "board_solder": ([0.78, 0.78, 0.76, 1.0], 1.0, 0.2),
    "board_alumina": ([0.85, 0.84, 0.80, 1.0], 0.0, 0.5),
    "board_overcoat": ([0.03, 0.03, 0.03, 1.0], 0.0, 0.4),
    "board_ceramic": ([0.55, 0.42, 0.28, 1.0], 0.0, 0.55),
    "board_epoxy": ([0.035, 0.035, 0.035, 1.0], 0.0, 0.55),
    "board_substrate": ([0.10, 0.12, 0.05, 1.0], 0.0, 0.4),
    "board_table": ([0.42, 0.42, 0.43, 1.0], 0.0, 0.8),
}

pieces = []  # (name, shape, material, centre, size)


def box(name, material, lo, hi, shape="cube"):
    """A piece spanning `lo` to `hi`, in millimetres."""
    centre = tuple((a + b) / 2 * MM for a, b in zip(lo, hi))
    size = tuple((b - a) * MM for a, b in zip(lo, hi))
    pieces.append((name, shape, material, centre, size))


# ---- the board -------------------------------------------------------------
HALF_X, HALF_Z, THICK, MASK = 50.0, 30.0, 1.6, 0.02
TOP = THICK
box("fr4", "board_fr4", (-HALF_X, MASK, -HALF_Z), (HALF_X, THICK - MASK, HALF_Z))
box("mask_top", "board_mask", (-HALF_X, THICK - MASK, -HALF_Z), (HALF_X, THICK, HALF_Z))
box("mask_bottom", "board_mask", (-HALF_X, 0.0, -HALF_Z), (HALF_X, MASK, HALF_Z))
box("table", "board_table", (-150.0, -10.0, -150.0), (150.0, 0.0, 150.0))

PAD = 0.03     # a pad's tin over the mask
SEAT = 0.04    # where a part's underside sits, over its pad and solder
TRACE = 0.03   # copper under the mask, as the mask's rise over it


def trace(name, a, b, width=0.2):
    """A straight trace from `a` to `b` (x, z), axis-aligned."""
    (x0, z0), (x1, z1) = a, b
    w = width / 2
    box(name, "board_mask_copper", (min(x0, x1) - w, TOP, min(z0, z1) - w),
        (max(x0, x1) + w, TOP + TRACE, max(z0, z1) + w))


# ---- 0402 passives ---------------------------------------------------------
ROW_Z = 12.0
ROW_X = [-26.0 + 4.0 * k for k in range(8)]
BUS_Z = 8.25  # the QFN's lowest left land, which the bus runs into
for i, x in enumerate(ROW_X):
    resistor = i % 2 == 0
    name = f"{'r' if resistor else 'c'}{i}"
    height = 0.35 if resistor else 0.5
    for side in (-1, 1):
        # A pad 0.45 x 0.55 centred 0.475 out, and a termination 0.2 long centred 0.4 out.
        px, ex = x + side * 0.475, x + side * 0.4
        box(f"{name}_pad{side:+d}", "board_tin", (px - 0.225, TOP, ROW_Z - 0.275),
            (px + 0.225, TOP + PAD, ROW_Z + 0.275))
        box(f"{name}_end{side:+d}", "board_tin", (ex - 0.1, TOP + SEAT, ROW_Z - 0.25),
            (ex + 0.1, TOP + SEAT + height, ROW_Z + 0.25))
    if resistor:
        box(f"{name}_body", "board_alumina", (x - 0.3, TOP + SEAT, ROW_Z - 0.25),
            (x + 0.3, TOP + SEAT + height - 0.03, ROW_Z + 0.25))
        box(f"{name}_overcoat", "board_overcoat", (x - 0.28, TOP + SEAT + height - 0.03,
                                                   ROW_Z - 0.23),
            (x + 0.28, TOP + SEAT + height, ROW_Z + 0.23))
    else:
        box(f"{name}_body", "board_ceramic", (x - 0.3, TOP + SEAT, ROW_Z - 0.25),
            (x + 0.3, TOP + SEAT + height, ROW_Z + 0.25))
    trace(f"{name}_trace", (x + 0.6, ROW_Z - 0.275), (x + 0.6, BUS_Z))

# ---- QFN-32 ----------------------------------------------------------------
QX, QZ, QHALF, QH = 22.0, 10.0, 2.5, 0.85
box("qfn_body", "board_epoxy", (QX - QHALF, TOP + SEAT, QZ - QHALF),
    (QX + QHALF, TOP + SEAT + QH, QZ + QHALF))
for k in range(8):
    t = -1.75 + 0.5 * k
    for axis, side in ((0, -1), (0, 1), (1, -1), (1, 1)):
        # A land 0.25 wide and 0.4 deep at the body's foot, standing 0.01 proud of its side.
        out = side * (QHALF + 0.01)
        inn = side * (QHALF - 0.39)
        lo_n, hi_n = min(out, inn), max(out, inn)
        pad_out = side * (QHALF + 0.3)
        pad_inn = side * (QHALF - 0.4)
        lo_p, hi_p = min(pad_out, pad_inn), max(pad_out, pad_inn)
        if axis == 0:
            land = ((QX + lo_n, TOP + SEAT, QZ + t - 0.125), (QX + hi_n, TOP + SEAT + 0.2,
                                                              QZ + t + 0.125))
            pad = ((QX + lo_p, TOP, QZ + t - 0.15), (QX + hi_p, TOP + PAD, QZ + t + 0.15))
        else:
            land = ((QX + t - 0.125, TOP + SEAT, QZ + lo_n), (QX + t + 0.125, TOP + SEAT + 0.2,
                                                              QZ + hi_n))
            pad = ((QX + t - 0.15, TOP, QZ + lo_p), (QX + t + 0.15, TOP + PAD, QZ + hi_p))
        box(f"qfn_land{axis}{side:+d}_{k}", "board_tin", *land)
        box(f"qfn_pad{axis}{side:+d}_{k}", "board_tin", *pad)

# ---- BGA on 0.5 mm balls ---------------------------------------------------
BX, BZ, BHALF, BALL, PITCH, STANDOFF = 22.0, -12.0, 3.5, 0.5, 0.8, 0.4
box("bga_substrate", "board_substrate", (BX - BHALF, TOP + STANDOFF, BZ - BHALF),
    (BX + BHALF, TOP + STANDOFF + 0.36, BZ + BHALF))
box("bga_mold", "board_epoxy", (BX - 3.0, TOP + STANDOFF + 0.36, BZ - 3.0),
    (BX + 3.0, TOP + STANDOFF + 0.86, BZ + 3.0))
for i in range(8):
    for j in range(8):
        cx, cz = BX + (i - 3.5) * PITCH, BZ + (j - 3.5) * PITCH
        box(f"bga_ball_{i}_{j}", "board_solder", (cx - BALL / 2, TOP, cz - BALL / 2),
            (cx + BALL / 2, TOP + BALL, cz + BALL / 2), shape="sphere")

trace("bus", (ROW_X[0] + 0.6, BUS_Z), (QX - QHALF - 0.3, BUS_Z))


def softbox(name, centre, size, nits):
    """An area panel at `centre`, `size` wide and tall (both mm), facing the board's middle."""
    aim = [a - c for a, c in zip((0.0, TOP, 0.0), centre)]
    length = math.sqrt(sum(x * x for x in aim))
    return {"name": name, "type": "area", "position": [c * MM for c in centre],
            "direction": [x / length for x in aim], "size": [s * MM for s in size],
            "color": [1.0, 1.0, 1.0], "intensity": nits, "intensity_unit": "nits"}


scene_desc = {
    "version": 1,
    "_comment": [
        "A circuit board at true scale (spec 13.57), in metres: a 0402 is a millimetre long.",
        "Lit by a tabletop studio of area panels. The camera here is the three-quarter view;",
        "the generator's header gives the whole-board and close-up ones.",
        "",
        "Regenerate with: python3 assets/generators/gen_board_fixture.py",
    ],
    "models": [{"path": asset_ref("board_fixture.gltf")}],
    "typicalSize": 0.001,
    "environment": {"ambient": [1.5, 1.5, 1.5]},
    "lights": [
        dict(softbox("key", (-150.0, 220.0, 180.0), (200.0, 150.0), 400.0), cast_shadows=True),
        softbox("fill", (260.0, 120.0, 40.0), (300.0, 200.0), 80.0),
        softbox("back", (-184.0, 124.0, -202.0), (300.0, 80.0), 250.0),
        {"name": "lamp", "type": "point", "position": [5.0 * MM, (TOP + 30.0) * MM, 6.0 * MM],
         "color": [1.0, 0.97, 0.92], "intensity": 0.05, "intensity_unit": "candela",
         "range": 0.3, "cast_shadows": True},
    ],
    "materials": {
        "board_mask": {"clearcoat": 1.0, "clearcoatRoughness": 0.05,
                       "sss": {"color": [0.3, 0.9, 0.4], "radius": 0.3 * MM}},
        "board_mask_copper": {"clearcoat": 1.0, "clearcoatRoughness": 0.05},
        "board_fr4": {"sss": {"color": [1.0, 0.85, 0.5], "radius": 0.5 * MM}},
    },
    "camera": {"eye": [0.062, 0.042, 0.068], "target": [0.006, 0.0, 0.002], "fov": 35,
               "fitClip": True},
}


write_json("board_fixture.gltf", gltf_of("gen_board_fixture.py", SHAPES, MATERIALS, pieces))
write_json("board_fixture.cscn", scene_desc)
print(f"wrote board_fixture (.gltf + .cscn), {len(pieces)} pieces")
