"""The light-specular group: a light's specular share (a light that lights diffusely; spec
13.30).
"""

import os

import numpy as np

from gatelib.harness import NO_HALOS, asset, render
from gatelib.images import _SRGB_TO_LINEAR, _read_ppm
from gatelib.scenes import cscn_copy


# cornell_point's one light over a metal box and a rough red wall (spec 13.30). The box's
# colour exists only to find it in the albedo view; the wall keeps its own.
SPECULAR_FIXTURE = "cornell_point.cscn"
SPECULAR_BOX = {"albedo": [0.9, 0.2, 0.9], "metallic": 1.0, "roughness": 0.4}
SPECULAR_LIT_MIN = 0.01      # the metal box's mean linear light with the light's lobes on
SPECULAR_METAL_RATIO = 0.02  # ... and with them off, as a fraction of that: none of it left
# The wall off over on: kept, give or take the rough lobe it loses, and by no more than F's share.
SPECULAR_DIFFUSE_GAIN = (0.97, 1.15)


def run_light_specular_gate(workdir):
    """A light's specular share (Light.specular, spec 13.30) takes its reflection and only that.

      spec-metal    a metal has no diffuse, so lit by a light whose share is 0 it is black: the
                    box goes from lit to nothing, with SSR off so no other surface reaches it.
      spec-diffuse  a rough red wall under the same light is not dimmed: what the light's lobes
                    give up its diffuse keeps, so the wall is as bright or a little brighter.

    The box's colour exists only to find it in the albedo view; at a share of 1 nothing moves, which
    the goldens hold.
    """
    src = asset(SPECULAR_FIXTURE)
    if not os.path.exists(src):
        print(f"  spec-metal   SKIP  (missing {SPECULAR_FIXTURE})")
        print(f"  spec-diffuse SKIP  (missing {SPECULAR_FIXTURE})")
        return []

    def variant(tag, share):
        def mutate(d):
            d["materials"] = {"cornell_tall_box": dict(SPECULAR_BOX)}
            if share is not None:
                for light in d["lights"]:
                    light["specular"] = share
        return cscn_copy(src, os.path.join(workdir, f"specular_{tag}.cscn"), mutate)

    def shot(scene, tag, extra):
        out = os.path.join(workdir, f"specular_{tag}.ppm")
        err = render(scene, out, NO_HALOS + ["--no-ssr"] + extra)
        if err:
            print(f"  spec-metal   ERROR rendering {tag}: {err.strip()[-300:]}")
            return None
        w, h, data = _read_ppm(out)
        return np.frombuffer(data, dtype=np.uint8).reshape(h, w, 3)

    on_scene, off_scene = variant("on", None), variant("off", 0.0)
    albedo = shot(on_scene, "albedo", ["--render-mode", "6"])
    on, off = shot(on_scene, "lit_on", []), shot(off_scene, "lit_off", [])
    if albedo is None or on is None or off is None:
        print("  spec-diffuse ERROR  a frame did not render")
        return ["spec-metal", "spec-diffuse"]

    # The albedo view writes display codes: the box near (242, 124, 242), the wall (205, 90, 80).
    r, g, b = (albedo[..., k].astype(int) for k in range(3))
    box = (r > 200) & (b > 200) & (r - g > 60)
    wall = (r > 150) & (r - g > 80) & (r - b > 80)
    decode = np.array(_SRGB_TO_LINEAR)

    def mean(img, mask):
        return float(decode[img[mask]].mean()) if mask.any() else float("nan")

    failures = []
    lit, dark = mean(on, box), mean(off, box)
    ok = box.sum() > 200 and lit >= SPECULAR_LIT_MIN and dark <= SPECULAR_METAL_RATIO * lit
    if not ok:
        failures.append("spec-metal")
    print(f"  spec-metal   {'PASS' if ok else 'FAIL'}  the metal box over {int(box.sum())} px: "
          f"{lit:.4f} lit, {dark:.5f} with the light's share at 0 (want <= "
          f"{SPECULAR_METAL_RATIO} of it, and lit >= {SPECULAR_LIT_MIN})")
    kept, had = mean(off, wall), mean(on, wall)
    gain = kept / had if had > 0 else float("nan")
    ok = wall.sum() > 200 and SPECULAR_DIFFUSE_GAIN[0] <= gain <= SPECULAR_DIFFUSE_GAIN[1]
    if not ok:
        failures.append("spec-diffuse")
    print(f"  spec-diffuse {'PASS' if ok else 'FAIL'}  the red wall over {int(wall.sum())} px: "
          f"{had:.4f} with its share, {kept:.4f} without, x{gain:.3f} (want "
          f"{SPECULAR_DIFFUSE_GAIN[0]}-{SPECULAR_DIFFUSE_GAIN[1]}: not dimmed)")
    return failures
