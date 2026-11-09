"""The pbr-variant group: lit-surface variants (a material compiles what it can use; spec 11.93)."""

import os

from gatelib.harness import RENDER, _run, asset
from gatelib.probes import _PBR_VARIANT, _pbr_feature_bits
from gatelib.scenes import SKIN_FIXTURE


def _pbr_variants(model):
    """Every distinct variant a run built, and its PBR_FEAT_ALL.

    Returns (variants, full, ok), where variants is {name: (features, samplers)}.
    `ok` is False when the render itself failed -- a process that logged its
    variants at init and then died would otherwise report a pass, which is the
    shape the house `render()` helper guards against.

    `samplers` is what the LINKED program kept, not what the source declares.
    That distinction is the arm below: a driver is free to drop a sampler whose
    reads were compiled away and this one does not, so the declaration has to be
    removed by the preprocessor for the unit to come back.
    """
    r = _run([RENDER, "-x", "-f", "3", "-m", model], capture_output=True, text=True)
    out = r.stdout + r.stderr
    variants = {m.group(1): (int(m.group(2)), int(m.group(4)))
                for m in _PBR_VARIANT.finditer(out)}
    # The C-side PBR_FEAT_ALL, which the log line already carries. Reading it
    # here rather than taking max() of what happened to be built keeps the arm
    # true if the engine ever stops building the full variant at init.
    full = next((int(m.group(3)) for m in _PBR_VARIANT.finditer(out)), None)
    return variants, full, r.returncode == 0


def run_pbr_variant_gate(workdir):
    """Lit-surface variants: a material compiles only the features it can use.

    Every arm reads the builder's own log line, because a correct variant and the
    uber-shader are the SAME PICTURE -- that is what the feature is for, and it
    is why no pixel arm anywhere in this suite can see any of this.

      pbr-variant-full    the mask the C side calls PBR_FEAT_ALL is the union of
                          the bits in pbr_features.glsl, the file C and GLSL both
                          include. A bit added to one and not the other is the
                          failure this whole scheme exists to make impossible.
      pbr-variant-lean    a material using none of the features resolves to a
                          variant carrying none. The arm that says the resolver
                          ran at all -- without it every material could sit on
                          the uber-shader and nothing else here would notice.
      pbr-variant-keeps   a material that USES a feature resolves to a variant
                          that HAS it. The direction that matters: dropping a
                          feature a material needs renders a plausible frame, so
                          the golden alone cannot be trusted to find it.
      pbr-variant-samplers a lean variant spends fewer texture image units than
                          the full one, and no variant spends more (spec 11.95).
                          Bounded both ways because the two failures are
                          opposite: equal counts mean the declarations are not
                          being gated at all, and a variant ABOVE the full mask
                          means a guard is inverted and a lean program is
                          carrying something the uber-shader does not.
      pbr-variant-skinned a SKINNED material resolves into the skinned family
                          rather than sitting on its uber-shader. Its own arm
                          because no golden in the corpus is skinned -- the
                          fixture set is entirely static, so this is the only
                          instrument that sees the family at all.

    What these do NOT check is the ASSIGNMENT -- which material ended up on which
    program. They read the variants BUILT. Inverting the resolver's sheen test so
    the wrong materials get the bit produces the identical set of builds, and only
    the sheen golden catches that. Closing it needs the resolver to log per
    material, which is worth doing and is not done here.

    Nor does the sampler arm catch ONE guard being reverted. Source count and
    linked count move together, so the honest bar is relative: all three guards
    gone fails, one gone does not. Said here rather than left for a reader to
    discover, per the rule that a coverage claim states the mutation it misses.
    """
    if not os.path.exists(RENDER):
        print("  pbr-variant-full SKIP  (render not built)")
        return []
    failures = []
    arms = ["pbr-variant-full", "pbr-variant-lean", "pbr-variant-keeps",
            "pbr-variant-samplers", "pbr-variant-skinned"]

    sheen_model = asset("sheen_fixture.gltf")
    if not os.path.exists(sheen_model):
        print("  pbr-variant-full SKIP  (sheen fixture not present)")
        return []

    bits, want_full = _pbr_feature_bits()
    if not bits or want_full is None:
        print("  pbr-variant-full ERROR could not read pbr_features.glsl")
        return arms

    built, full, ran = _pbr_variants(sheen_model)
    if not ran or not built:
        print("  pbr-variant-full ERROR the render failed or logged no variant")
        return arms

    # --- full ---------------------------------------------------------------
    union = 0
    for bit in bits.values():
        union |= bit
    ok = full == want_full == union
    print(f"  pbr-variant-full {'PASS' if ok else 'FAIL'}  engine reports {full}, "
          f"pbr_features.glsl declares PBR_FEAT_ALL {want_full} and a union of "
          f"{union} over {len(bits)} bits (want all three equal: they are the contract "
          f"between the C that emits a mask and the shader that reads it back)")
    if not ok:
        failures.append("pbr-variant-full")

    # --- lean ---------------------------------------------------------------
    # The sheen fixture's other materials use none of the features, so a lean
    # variant must exist in the same run that produced the sheen one.
    ok = any(f == 0 for f, _ in built.values())
    print(f"  pbr-variant-lean {'PASS' if ok else 'FAIL'}  built {sorted(built)} (want one "
          f"carrying 0 features: without it every material sits on the uber-shader and no "
          f"pixel arm in this suite would notice)")
    if not ok:
        failures.append("pbr-variant-lean")

    # --- keeps --------------------------------------------------------------
    sheen_bit = bits["sheen"]
    kept = [n for n, (f, _) in built.items() if f & sheen_bit and f != want_full]
    ok = bool(kept)
    print(f"  pbr-variant-keeps {'PASS' if ok else 'FAIL'}  {sorted(kept)} carry the sheen bit "
          f"({sheen_bit}) without being the full mask (want at least one: a variant that "
          f"dropped a feature its material needed renders a plausible frame, so the golden "
          f"cannot find this on its own)")
    if not ok:
        failures.append("pbr-variant-keeps")

    # --- samplers -----------------------------------------------------------
    # The FULL variant is the reference rather than the literal 16, so this
    # cannot rot when a declaration is added or the ledger changes shape.
    full_units = next((s for f, s in built.values() if f == want_full), None)
    lean_units = min((s for f, s in built.values() if f != want_full), default=None)
    most = max(s for _, s in built.values())
    ok = (full_units is not None and lean_units is not None
          and lean_units < full_units and most == full_units)
    print(f"  pbr-variant-samplers {'PASS' if ok else 'FAIL'}  leanest spends {lean_units} of "
          f"the full mask's {full_units} texture image units, most spent by any variant is "
          f"{most} (want fewer than full and no variant above it: equal means the "
          f"declarations are not gated at all, above means a guard is inverted)")
    if not ok:
        failures.append("pbr-variant-samplers")

    # --- skinned ------------------------------------------------------------
    # Its own render, because the sheen fixture has no skinned mesh and nothing
    # in the golden corpus is skinned either.
    skinned_model = asset(SKIN_FIXTURE)
    if not os.path.exists(skinned_model):
        print(f"  pbr-variant-skinned SKIP  ({SKIN_FIXTURE} not present)")
        return failures
    skinned_built, _, skinned_ran = _pbr_variants(skinned_model)
    lean_skinned = [n for n, (f, _) in skinned_built.items()
                    if n.startswith("pbr_skinned") and f != want_full]
    ok = skinned_ran and bool(lean_skinned)
    print(f"  pbr-variant-skinned {'PASS' if ok else 'FAIL'}  {sorted(lean_skinned)} from "
          f"{sorted(skinned_built)} (want a skinned variant below the full mask: before 11.95 "
          f"the skinned program set no features and every rig sat on the uber-shader, which "
          f"no golden in this corpus can see because none of them is skinned)")
    if not ok:
        failures.append("pbr-variant-skinned")

    return failures
