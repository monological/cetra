# Armor Speckle Investigation

Status: **RESOLVED** 2026-07-15 (commit `410afa7`). Root cause: skinned
normals were transformed by `mat3(boneTransform)` instead of its inverse-
transpose, so cross-rig retargeting shear skewed the normal direction. The
decisive test was 2x SSAA (it changed nothing — proving the specks are
surface-locked, not screen-space aliasing). See §6.

A dense field of fine white specks appears on the Raiden model's armor. This
document records every symptom, every discriminating test, and every fix
attempt, so the next debugging session does not repeat work.

---

## 1. Symptom

- Fine, bright, near-white specks scattered densely across the metal armor
  plates (and sometimes bright streaks in the hair). Concentrated along plate
  edges/seams in some poses, spread across plate faces in others.
- Appears with the standard viewer command:
  ```
  ./out/bin/render -m my_models/raiden/source/raiden_textured_rigged.glb \
      -t my_models/raiden/textures -e my_models/studio_small_03_8k.hdr \
      -a my_models/animations/strut_walk.fbx -s my_models/animations/T-Pose.fbx
  ```
- **Per-run random**: launch the exact same binary/command several times — some
  launches show it, some don't. Within a single run it is stable (it does not
  flicker frame-to-frame at a held pose).

---

## 2. Confirmed facts (high confidence)

These are established by controlled tests, not inference.

| # | Fact | How it was established |
|---|------|------------------------|
| F1 | **Requires the skinned/animated (deformed) mesh.** With **no `-a`** (static bind pose), 7 consecutive runs were clean. With `-a`, it appears. It shows even when the animation is *paused* in free mode — i.e. it is the deformed pose, not motion per se. | User ran no-anim 7× clean; posed/paused runs speckle. |
| F2 | **Requires the analytic key lights.** `--no-key-light` (pure IBL) removes it. Running with no HDR (`-e` omitted) also removes it — because the key lights are extracted from the HDR lobes, so no HDR ⇒ no key lights. | User tested both. |
| F3 | **Deterministic at a fixed pose.** Two headless renders with a *frozen* pose (`-a T-Pose.fbx -s T-Pose.fbx`) are **byte-identical** (0 differing pixels). | `cmp` of two renders. |
| F4 | **The per-run "randomness" is the animation phase.** The app advances the walk cycle off the wall clock, so every launch catches Raiden at a different pose at screenshot time. Different builds (ASan/TSan/zero-init) run at different speeds ⇒ catch different poses ⇒ different speckle state. Combined with F3, there is **no run-to-run nondeterminism in the render itself** — only in which pose is shown. | Inference from F3 + F4 observations. |
| F5 | **Not a memory-safety bug and not a data race.** AddressSanitizer + UBSan build: clean (no OOB/UAF/UB). ThreadSanitizer build: clean (no data races). | Instrumented builds run headless. |
| F6 | **Not an uninitialized *stack* variable.** `-ftrivial-auto-var-init=zero` build still shows it. | Instrumented build. |
| F7 | **Immune to every post/shadow/AA toggle.** Still present with bloom OFF, SSAO OFF, SSR OFF, normals-G-buffer OFF, shadows OFF, shadow catcher OFF, ground projection OFF, and **Spec AA = 0**. | User toggled all off in the GUI. |

The net of F1–F7: at a deformed pose, the **direct key-light lighting math
produces genuinely bright/wrong values at specific pixels**. It is not post
processing, not shadows, not screen-space aliasing (spec-AA off changes
nothing), not z-fighting (see attempt A7), not a memory fault, and not motion —
it is a property of the shaded result at particular deformed poses.

---

## 3. Related earlier history (the "specular artifact" lineage)

The white speckle is the current tail of a longer chain of bright/dark
specular artifacts, all rooted in intense key-light specular on glossy armor.
These were **real fixes for real sub-problems** and are committed, but none
fully resolved the animated speckle:

- **Black flecks (fp16 overflow → NaN).** The RGBA16F scene buffer overflowed
  tight GGX spikes to +INF; both tonemap curves turned INF into NaN → black.
  Fixed by clamping PBR output < 65504 and sanitizing tonemap input
  (`e55ac43`), plus NaN guards on the thin-film sqrt chain, Fresnel `pow`s with
  bases that can round negative, and the half-vector `normalize`.
- **White "blocky" bloom spots.** Same spikes, seen through the bloom bright
  pass. Reduced by the bloom firefly clamp.
- **Per-light firefly clamp** (`0959078`): `min(contribution, 10)` per light.
- **Area-light specular approximation** (`d22d4bb`): Karis sphere-light lobe
  widening with energy renormalization `(a/a')²`, to stop needle GGX lobes on
  polished texels. (An earlier naive roughness-floor attempt caused a "disco
  ball" — widening *without* renormalization — and was replaced by this.)

The user's bisect showed the speckle is present at `0959078`, `d22d4bb`,
`898f26b` — i.e. it **predates** the area-light fix. So those fixes reduced how
*often*/how *severely* the speckle shows, but did not touch its root.

---

## 4. Diagnostic tests run (and results)

| Test | Result | Interpretation |
|------|--------|----------------|
| Toggle SSAO / normals / SpecAA off individually | Still speckles | Not SSAO, not the normals G-buffer, not spec-AA |
| `--no-key-light` | **Clean** | Needs the analytic key lights |
| No HDR (`-e` omitted) | **Clean** | Same (no HDR ⇒ no extracted key lights) |
| No animation (`-a` omitted), 7 runs | **Clean** | Needs the skinned/deformed pose |
| Frozen pose `-a T-Pose` ×2, `cmp` | **0 diff** | Deterministic at a fixed pose |
| Two bind-pose headless renders, `cmp` | 702 px, Δ1–2, near top only | Benign hair-edge (A2C/MSAA); **not** the armor speckle |
| Two close-side headless renders, `cmp` | 69 px, Δ1 | Essentially identical; armor is bit-stable without `-a` |
| ASan + UBSan build, headless | **No errors** | No OOB / UAF / UB |
| TSan build, headless | **No data races** | No thread race |
| `-ftrivial-auto-var-init=zero` build | Still speckles | Not an uninitialized stack variable |
| All post/shadows off + SpecAA 0 | Still speckles | Isolated to raw PBR direct lighting |
| **2x SSAA** (render at 2x, box-downsample) | **Still speckles, unchanged** | **Decisive: NOT screen-space aliasing.** Supersampling provably removes sub-pixel sampling error; the specks survived untouched, so they are a genuinely wrong shaded value at a fixed surface point — not a sampling artifact. Redirected the hunt to the shading normal at its source. |

Instrumented builds were produced with throwaway CMake directories
(`out_asan`, `out_tsan`, `out_zeroinit`) using
`-fsanitize=address,undefined`, `-fsanitize=thread`, and
`-ftrivial-auto-var-init=zero` respectively. `build.sh` was intentionally left
minimal (no sanitizer flags baked in).

---

## 5. Fix attempts (all FAILED for the animated speckle)

| # | Attempt | Rationale | Outcome |
|---|---------|-----------|---------|
| A1 | Strengthen geometric spec-AA cap (0.18 → 0.4) | If it were sub-pixel specular aliasing, wider roughness on high-variance pixels would suppress it | Still speckles. **Reverted.** |
| A2 | Widen area-light lobe floor (0.05 → 0.08) + apply widening to the anisotropic NDF path | Cover flat glossy faces and close the anisotropic gap | Still speckles. **Reverted.** |
| A3 | Per-sample shading: `glEnable(GL_SAMPLE_SHADING); glMinSampleShading(1.0)` | MSAA only AAs geometry coverage; the fragment shader runs once per pixel, so specular isn't multisampled. Forcing per-sample shading makes the 4× MSAA average the specular. | Still speckles. **Reverted** (also a 4× fragment cost not worth leaving in). |
| A4 | Depth-range fix: near `scene_radius*0.01`→`*0.05`, and **remove the `far<100 ⇒ far=10000` fallback** (far now `scene_radius*40`) | The clip planes were near≈0.0138 / far=10000 (≈700000:1 ratio), destroying 24-bit depth precision; retargeting pushes layered plates near-coplanar, so they could z-fight into edge speckle | Still speckles. **KEPT** — it is a genuine depth-precision bug fix regardless, and improves overall quality. |
| A5 | (Phase-5-era) PCSS blocker/filter radius reduction | A different manifestation during PCSS work — an oversized, undersampled shadow filter injected per-pixel noise onto the specular. Reduced that, but is unrelated to the base speckle. | N/A to the base issue. |

### Also fixed en route but not the cause
- **`getShadowSlot` reverse-lookup fragility** (`pbr_frag.glsl`): the shader
  mapped light→shadow-slot by reverse-searching `shadowLightIndex`, which is
  only correct if lights arrive in shadow-slot order. Replaced with a direct
  per-light slot lookup. Verified **inert for this scene** (the key lights sort
  in identity order because directional lights share position (0,0,0)), so it
  is a robustness fix, **not** the speckle cause. Kept, uncommitted.

---

## 6. Root cause (RESOLVED) and the decisive test

**Root cause: skinned normals were transformed by `mat3(boneTransform)`
instead of its inverse-transpose** (`pbr_skinned_vert.glsl`). For a pure
rotation the two are equal — which is why rigid / non-retargeted content was
always fine — but cross-rig retargeting (strut_walk -> Raiden via `-s
T-Pose`) injects non-uniform scale/shear into the blended bone matrix.
`mat3(boneTransform) * normal` then skews the normal's *direction*;
`normalize()` fixes its length but not its direction. That wrong base normal,
under the sharp GGX lobe of the intense near-point key lights, mirrors them
into a field of clipped-white specks — surface-locked, so no screen-space
method (MSAA, spec-AA, SSAO, SSR, or SSAA) can touch it.

### The reasoning that nailed it
The normal map is applied in **both** the bind pose and the animated pose.
Bind pose is clean (F1); animated speckles. The *only* thing that differs is
the skinning transform, so the cause is in skinning — not the normal map, not
the lights, not anything screen-space (which is why every toggle in §4 did
nothing). **2x SSAA closed it out**: supersampling provably removes sub-pixel
sampling error, and the specks survived it unchanged — proving a genuinely
wrong shaded value at a fixed surface point, not a sampling artifact.

### The fix (committed, `410afa7`)
The skinned normal is now transformed by
`transpose(inverse(mat3(boneTransform)))`, with a determinant guard so a
degenerate blend cannot produce a NaN normal. Tangents/bitangents keep the
forward matrix — they are surface vectors, correctly transformed by it.
Verified clean across every deformed walk-cycle pose at full resolution.

### Superseded hypothesis (kept for the record)
The theory below blamed a non-orthonormal TBN and applied a fragment-shader
Gram-Schmidt re-orthonormalization (`a2427b2`). It could never work: it
re-orthonormalized T/B **against the same wrong base normal**, building a tidy
frame around a normal that pointed the wrong way. The Gram-Schmidt change is
harmless and correct in isolation (it handles interpolation across triangles),
so it was kept — but it was not the cause.

---

**A non-orthonormal TBN skews the normal-mapped shading normal on deformed,
normal-mapped surfaces.**

The decisive test — Normals render view at a speckling pose — came back
**clean**. But `renderMode==1` visualizes `normalize(Normal)`, the **geometric
interpolated vertex normal**, *not* the normal-mapped shading normal that
lighting uses (`normalize(TBN * normalMapSample)`). So:

- The clean Normals view only proves the **geometry** is fine.
- The very fact that specks appear *despite* a clean geometric normal proves the
  armor **has a normal map** and the **shading normal** is what's wrong.

The shading normal is built from the interpolated TBN. That TBN is **not
re-orthonormalized** in the fragment shader (`pbr_frag.glsl`, old line 375:
`N = normalize(TBN * N)`). Two things make it non-orthonormal:

1. **Interpolation** across a triangle does not preserve orthonormality.
2. **Skinning** — `pbr_skinned_vert.glsl` transforms T/B/N with
   `mat3(boneTransform)`, and cross-rig retargeting (`strut_walk` → Raiden)
   puts **non-uniform scale/shear** in the bone matrices, so T and B come out
   non-perpendicular and unequally scaled.

`TBN * nTex` then mixes the normal map's x/y/z through a skewed basis,
**amplifying the error where the normal map is steep** — producing bright
specks under the intense key lights, only on deformed poses. This matches
**every** confirmed fact (F1 skinning-only, F2 key-lights, F3/F4
deterministic-per-pose, F5/F6 not-memory, F7+A1–A4 immune to sampling/post/AA/
depth because the *input normal* is wrong).

### Fix applied
`pbr_frag.glsl` now re-orthonormalizes the TBN per fragment (Gram-Schmidt)
against the clean geometric normal before applying the normal map:

```glsl
vec3 Ng = normalize(Normal);
vec3 T  = normalize(TBN[0] - dot(TBN[0], Ng) * Ng);
vec3 B  = cross(Ng, T);
if (dot(B, TBN[1]) < 0.0) B = -B;      // preserve authored handedness
N = normalize(mat3(T, B, Ng) * (normalMapSample*2-1 with xy*scale));
```

This is standard practice for normal mapping and is correct independent of
skinning. **Status: built, not yet confirmed by the user.** If it does not
resolve the speckle, the remaining candidates are the GGX specular denominator
`4·NdotV·NdotL + 0.0001` blowing up at grazing angles (widen ε / clamp
`specular` itself, not just the per-light sum) and occasional bad bone matrices
from retargeting.

---

## 7. Committed state

- `410afa7` — the root-cause fix (skinned normal inverse-transpose).
- `014c850` — optional `--ssaa` supersampling (off by default; also the
  discriminator that proved this was surface-locked).
- Earlier committed correctness work that reduced severity but was not the
  cause: depth-range fix (`9d9ac0f`), TBN Gram-Schmidt + shadow-slot
  (`a2427b2`), firefly/area-light clamps (`0959078`, `d22d4bb`).
- Throwaway sanitizer build dirs `out_asan/`, `out_tsan/`, `out_zeroinit/`
  may still exist on disk (untracked); safe to delete.

Branch: `rendering-roadmap`. Phase 5 (PCSS) is stashed in `stash@{0}`.

---

## 8. Key lesson

The single most useful test was the **frozen-pose determinism check**
(`-a T-Pose` ×2, `cmp` → 0 diff). It proved the render is deterministic and that
the "per-run randomness" is entirely the wall-clock animation phase choosing a
different pose. That reframed the whole hunt away from a memory/race bug (which
sanitizers had already ruled out) and toward **pose-dependent shading**. Do this
test *first* next time any "random per run" rendering artifact appears: freeze
all time-varying inputs and diff two runs before reaching for sanitizers.

Two more lessons from the tail of this hunt:

- **"Present in both the clean and the broken case" eliminates a suspect.**
  The normal map is applied in the bind pose *and* the animated pose; the bind
  pose was clean. That single observation ruled out the normal map (and
  everything else shared by both) and pointed straight at what differs —
  skinning. State the invariant explicitly before theorizing.
- **SSAA is a discriminator, not just a fix.** If 2x supersampling doesn't move
  an artifact, it is not screen-space sampling aliasing — it is surface-locked
  (a genuinely wrong shaded value). That one test collapsed months of
  "specular aliasing" framing and redirected to the shading normal's source.
