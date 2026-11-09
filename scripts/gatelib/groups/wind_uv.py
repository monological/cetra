"""The wind-uv group: UV1 carries wind data through import (spec 11.51)."""

import base64
import json
import struct
import os

from gatelib.harness import asset, render
from gatelib.images import _read_ppm


WIND_UV_FIXTURE = "wind_uv_fixture.cscn"
WIND_UV_STILL = "wind_uv_fixture_still.cscn"
# A quad owns a pixel when its channel beats both others by this factor. The
# backdrop is authored neutral, so nothing there can clear it; the margin only
# has to survive the vignette and the tonemap toe.
WIND_UV_DOMINANCE = 1.6
WIND_UV_FLOOR = 40  # 8-bit; a lit quad reads far above this, the backdrop far below
# Travel separation below this means the flex channel is not reaching the shader
# at all -- which is the silent failure, since the body-lean term still leans.
WIND_UV_MIN_SPREAD = 2.0
# The two gaps between three evenly-spaced flex values must match. Generous
# because the measurement is a centroid of an antialiased silhouette at the
# gate's small frame, not because the underlying relation is loose.
WIND_UV_LINEARITY = 0.30


def _wind_uv_flex():
    """The quads' authored flex weights, read out of the fixture's own glTF.

    Read rather than transcribed, for the reason _water_ramp_edges is: a copy
    here would be a second statement of what the fixture says, and the two are
    only equal until someone edits one. Worse than a stale box, it would be a
    stale LABEL -- the arm's own report would name flex values the fixture does
    not carry, which is how a red gate ends up describing the wrong defect.

    Returned in node order, left to right, which is the order the centroids come
    back in.
    """
    with open(asset("wind_uv_fixture.gltf")) as f:
        doc = json.load(f)
    raw = base64.b64decode(doc["buffers"][0]["uri"].split(",", 1)[1])
    out = []
    # Node order, not mesh order: the arm reads the frame left to right, and it
    # is the node's translation that puts a quad there.
    quads = sorted((n for n in doc["nodes"] if n["name"].startswith("flex_")),
                   key=lambda n: n["translation"][0])
    for node in quads:
        prim = doc["meshes"][node["mesh"]]["primitives"][0]
        acc = doc["accessors"][prim["attributes"]["TEXCOORD_1"]]
        view = doc["bufferViews"][acc["bufferView"]]
        base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        out.append(struct.unpack_from("<2f", raw, base)[1])
    return out


def _wind_uv_centroids(path):
    """Mean x of each quad, in pixels, keyed by channel dominance."""
    w, h, pix = _read_ppm(path)
    tot = [0.0, 0.0, 0.0]
    n = [0, 0, 0]
    for py in range(h):
        row = (py * w) * 3
        for px in range(w):
            o = row + px * 3
            r, g, b = pix[o], pix[o + 1], pix[o + 2]
            for c, (hi, a, bb) in enumerate(((r, g, b), (g, r, b), (b, r, g))):
                if hi >= WIND_UV_FLOOR and hi >= WIND_UV_DOMINANCE * max(a, bb, 1):
                    tot[c] += px
                    n[c] += 1
    return [(tot[c] / n[c] if n[c] else None) for c in range(3)], n


def run_wind_uv_gate(workdir):
    """UV1.y arrives at the shader as the flex weight it was authored as (spec 11.51).

    Three identical quads differing only in TEXCOORD_1.y (1.0, 0.5, 0.0), rendered
    still and then under a wind blowing along +x. Travel must fall in that order
    and by equal steps, because flex is a raw linear multiplier on the sway term.

    The failure this guards is SILENT, which is the whole reason it is worth a
    fixture. Geometry that reaches the engine without UV1 does not error --
    tex_coords2 is NULL, attribute 8 reads (0,0), and every surface gets phase 0
    and flex 0. It still LEANS, because the height-mask body term carries no
    flex, so the frame looks like a calm day rather than like a bug. Nothing in
    the corpus could see that before 11.51: every vegetation surface here is
    generated in C, where the wind channels are written by the same code that
    builds the mesh and cannot disagree with it.

    What can regress is narrower than the whole path and worth naming. Three
    V-flips sit between an authoring tool and this shader -- the exporter's, then
    assimp's glTF2 importer, then cetra's aiProcess_FlipUVs -- and the last two
    CANCEL. That cancellation is what makes the accessor value the shader value,
    and it is the half cetra owns: an assimp bump or a change to uv_flip_flag()
    inverts the convention silently, and every imported plant animates wrong with
    the stiffest leaves at the tips. The exporter's own flip lives on the far side
    of the file boundary and is recorded in the spec instead.

    Two renders rather than one: wind has no CLI flag, so the still reference has
    to come from a second scene file. A single frame cannot substitute, because a
    linear ramp of displacement preserves the quads' even spacing -- three quads
    that all moved by the same wrong amount look exactly like three quads that
    moved correctly.
    """
    scene = asset(WIND_UV_FIXTURE)
    still = asset(WIND_UV_STILL)
    if not (os.path.exists(scene) and os.path.exists(still)):
        print(f"  wind-uv-flex SKIP  ({WIND_UV_FIXTURE} not present)")
        return []

    a = os.path.join(workdir, "wind_uv_still.ppm")
    b = os.path.join(workdir, "wind_uv_wind.ppm")
    for path, out in ((still, a), (scene, b)):
        err = render(path, out, [])
        if err:
            print(f"  wind-uv-flex ERROR render failed: {err.strip()[-200:]}")
            return ["wind-uv-flex"]

    rest, n_rest = _wind_uv_centroids(a)
    blown, n_blown = _wind_uv_centroids(b)
    if any(v is None for v in rest + blown):
        print(f"  wind-uv-flex FAIL  a quad was not found: still pixels {n_rest}, "
              f"wind pixels {n_blown} (red, green, blue)")
        return ["wind-uv-flex"]

    travel = [blown[c] - rest[c] for c in range(3)]
    # MAGNITUDE, in the order the quads were authored. Normalising by the sign of
    # (first - last) instead would be the obvious way to survive the fixture's
    # wind being re-aimed, and it silently defeats the arm: an inverted
    # convention hands red 0.0 and blue 1.0, the travels come back ascending, and
    # flipping the sign turns that into a descending sequence that passes every
    # check. The regression this exists to catch would have been the one thing it
    # could not see. Magnitude survives a re-aim and still fails an inversion.
    t = [abs(v) for v in travel]

    flex = _wind_uv_flex()
    ordered = t[0] > t[1] > t[2]
    spread = t[0] - t[2]
    step_hi, step_lo = t[0] - t[1], t[1] - t[2]
    linear = abs(step_hi - step_lo) <= WIND_UV_LINEARITY * max(spread, 1e-6)
    # Descending travel is only the right expectation because the fixture authors
    # descending flex. Asserted rather than assumed, so re-ordering the fixture
    # turns this arm red instead of quietly inverting what it tests.
    ok = (ordered and spread >= WIND_UV_MIN_SPREAD and linear
          and flex[0] > flex[1] > flex[2])

    print(f"  wind-uv-flex {'PASS' if ok else 'FAIL'}  travel {t[0]:.2f} / {t[1]:.2f} / "
          f"{t[2]:.2f} px for TEXCOORD_1.y {flex[0]:g} / {flex[1]:g} / "
          f"{flex[2]:g} (want descending: {ordered}); spread {spread:.2f} px "
          f"(want >= {WIND_UV_MIN_SPREAD}, below it UV1 is not reaching the shader and the "
          f"body lean is all that moves); steps {step_hi:.2f} vs {step_lo:.2f}, "
          f"{abs(step_hi - step_lo) / max(spread, 1e-6) * 100:.1f}% apart "
          f"(want <= {WIND_UV_LINEARITY * 100:.0f}%: flex is a linear multiplier)")
    return [] if ok else ["wind-uv-flex"]
