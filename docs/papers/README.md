# Reference material

Sources this engine's implementations are read off, kept here because the answers to the
questions that actually bit us were **not** in the prose everybody cites — they were in shipped
code, and two of those implementations disagree with each other.

Each entry records the citation, where it came from, when, and **the one claim cetra takes from
it**. That last field is the point of this directory: a paper in a repo with no statement of
what was used from it is decoration.

Papers are converted with `pdf2md` to markdown rather than kept as PDF — greppable, and a
fraction of the size. **The texts are NOT committed**: this repository is public, and a paper
an author hosts for reading is not one we may repost — nearly every copyright notice below
says so in as many words. `.gitignore` keeps `docs/papers/*` on the machine that fetched it,
and this README is what is committed, so each entry's source link is how to rebuild the
directory:

    curl -sL -o paper.pdf <source URL>
    pdf2md paper.pdf > docs/papers/<the file name the entry gives>

The one exception is a paper under an OPEN licence (CC-BY or the like), which is committed in
full with an attribution header and un-ignored by name in `.gitignore`. The entry says which.

---

## Alpha testing / coverage preservation

Read for spec 11.88, after 11.87 shipped a mip coverage-preservation pass written from memory of
the technique rather than from these.

### Castaño, *Computing Alpha Mipmaps* (2010)

- <http://www.ludicon.com/castano/blog/articles/computing-alpha-mipmaps/>
  (mirrored at <http://the-witness.net/news/2010/09/computing-alpha-mipmaps/>)
- Fetched 2026-08-27. A blog post, not a paper — no PDF to convert.

**What cetra takes from it:** the shape of the technique — measure level 0's surviving fraction
under the alpha test, then per level binary-search a scale on alpha that reproduces it.

**What it gets wrong for our purposes, and this is the trap:** the article states coverage as
the naive `Sum(a_i > A_r) / N`, a hard per-texel count. **Castaño's own shipped code does not do
that** (see NVTT below), and the difference is not cosmetic — a texel count is a step function
of the scale with wide plateaus, and the search degenerates against it. 11.87 implemented the
blog and not the code.

### NVIDIA Texture Tools (NVTT) — the reference implementation

- `castano/nvidia-texture-tools`, pinned at `aeddd65f81d36d8cb7b169b469ef25156666077e`
- `src/nvimage/FloatImage.cpp` — `FloatImage::alphaTestCoverage`,
  `FloatImage::scaleAlphaToCoverage`
- `src/nvtt/Surface.cpp` — the `Surface::` wrappers
- Read 2026-08-27.

**What cetra takes from it:**

1. **Coverage is measured over the bilinear reconstruction, not over texels.** It walks
   `(w-1) x (h-1)` 2x2 neighbourhoods, bilinearly interpolates a 4x4 subsample grid inside each,
   and counts subsamples passing. The naive per-texel count sits in the same function under
   `#if 0`. This is what makes coverage effectively continuous in the scale, and every other
   property of the method depends on it.
2. **The applied scale is the best-error one, not the converged one.** It tracks
   `bestAlphaScale` seeded at `1.0f`, updated whenever `fabsf(currentCoverage - desiredCoverage)`
   improves, and applies that — never the tenth bisection midpoint, which was never evaluated.
3. Ten steps of bisection. **Not** its `[0, 4]` range: cetra uses `[1/4, 4]`,
   since `[0, 4]` is asymmetric in log space and the attenuate-to-nothing end has
   no use once an unreachable target resolves to scale 1 anyway.

**What cetra deliberately does NOT take:** NVTT cascades — its documented loop calls
`buildNextMipmap` and `scaleAlphaToCoverage` on the same image in sequence, so each level is
filtered from the rescaled parent. That is safe there because the chain is `FloatImage`, float32
throughout. Cetra's chain is `uint8`, where cascading puts a clamp at 255 and two roundings
inside a feedback loop. See DirectXTex.

### DirectXTex — the second implementation, and it differs

- `microsoft/DirectXTex`, pinned at `0bb96f0f3fb95b7a38a3a6a8293af14249efa796`
- `DirectXTex/DirectXTexMipmaps.cpp` — `CalculateAlphaCoverage`,
  `EstimateAlphaScaleForCoverage`, `ScaleMipMapsAlphaForCoverage`
- Read 2026-08-27.

**What cetra takes from it:** the **pristine chain**. `ScaleMipMapsAlphaForCoverage` takes the
target from `srcImages[0]` and estimates each level's scale from `srcImages[level]` — the
unmodified source mip, not a rescaled predecessor. That is the half NVTT's float32 buffers let
it skip and an 8-bit chain cannot.

It agrees with NVTT on the other two points, independently: same supersampled reconstruction
(at 8x8 rather than 4x4), same best-error tracking. Cetra uses **N = 4**, NVTT's value —
DirectXTex's 8 is 4x the work per bisection step per level for a smoother estimate.

### Yuksel, *Alpha Distribution for Alpha Testing*, I3D 2018

- <http://www.cemyuksel.com/research/alphadistribution/> — fetched 2026-08-27
- Local: `yuksel-2018-alpha-distribution-for-alpha-testing.md`. ACM copyright, whose notice
  requires permission "to post on servers"; committed until spec 13.14.
- The supplementary document is figure plates with no extractable text (`pdf2md` reports it
  needs OCR); not converted, and nothing here depends on it.

**What cetra takes from it — since 11.100, the algorithm itself:**

1. **§3.1's error diffusion**, as `texture_distribute_alpha` (`texture.c`), firing only where
   11.88's rescale reports a residual miss past `TEXTURE_DISTRIBUTE_MISS` — the surgical scope,
   not the paper's every-level one, so levels the rescale can still REACH stay byte-identical and
   the near field keeps A2C's smooth one-pixel edges. (Reachability, not structure, is the
   criterion: the dots fixture's level 3 is structured and still fires, because its reachable
   coverage stops 0.05 short of the target.) Four deviations from §3.1's letter, each deliberate:
   **serpentine scan** (raster FS grows directional worms exactly on the low-density uniform
   fields this fires on); **edge rows keep their residual in play** — the last row flows it ahead,
   a single-column level flows it down — instead of dropping most of it off the boundary, which
   keeps the ON count conservative for the N×1/1×N tails and makes the 1x1 a majority round; the
   **target is 11.88's level-0 bilinear-reconstruction coverage**, not §3.2's ᾱN/(2α_τ) — glTF
   MASK semantics says the thing to preserve across distance is the level-0 TEST RESULT, not
   ground-truth transparency, and on this corpus the two coincide anyway; and the field is
   **normalized to that target and quantized at 1/2**, so the authored cutoff never enters and the
   paper's α_τ = 1/2 design centre dissolves. Two things that are NOT deviations, kept beside
   them because a reader will look for them here: **level 0 is never rewritten**, which is the
   paper's own §6 fix for magnification and was already `texture_derive_levels`' shape; and the
   implementation is **PRNG-free**, which deviates from nothing in §3.1 (Floyd–Steinberg needs no
   randomness) — the randomness requirement belongs to §3.2's pyramid, and is one of the three
   reasons that variant was refused (with its α_τ >= 1/2-only guarantee against authored 0.4s,
   and Yuksel rating it "arguably marginally better").
2. **The honest ceiling on what 11.88 builds.** §2, on the scale-the-alpha family: *"This simple
   fix can help in some cases, but it does not always improve the results."* 11.100 is the answer
   past that ceiling — and found a ceiling of its own the paper does not discuss: under grazing
   anisotropy the sampler averages the dither into a smooth low-alpha field the sharpened test
   deletes, measured on the alphacov plane at 15 degrees.
3. **A third independent vote for the pristine chain.** Both of its algorithms process "each
   mipmap level completely independently".

**Not implemented:** the alpha pyramid (§3.2, refused above); §4's alpha-to-coverage extensions —
§4.1's sample-mask texture needs a second nearest-filtered sampler the ledger does not have,
§4.2's hashed mask needs `gl_SampleMask` work, and the paper's A2C figures that look good are the
§4.1 ones. Note the quotable sentence is COMPARATIVE, not absolute: *"As compared to hashed alpha
testing, alpha distribution produces substantially less noise with alpha testing, but it provides
no apparent qualitative improvement in alpha-to-coverage"* — i.e. §4 matches hashed's A2C quality
rather than beating it; it is not Yuksel saying §4 buys nothing over plain A2C. The refusal here
stands on the sampler ledger and the shader work, not on that quote.

**§6's texcoord jitter for Moiré is TAKEN since 11.101 — and the paper's own scoping of it was
vindicated the hard way.** Yuksel uses the jitter only for his Figure 8, a still. Spec 11.101
added an argument the paper lacks — that TAA's accumulator would average the jittered lookups
into stable fractional coverage — and measured it false at one sample: a binary test under a
clamped history holds no haze, whatever the sequence. What the jitter measurably does is what the
paper uses it for (the Moiré dissolves) plus an 8x churn cut on the one-sample path; under
alpha-to-coverage it is a net loss and is gated off, because the MSAA path already carries the
dither's coverage at truth level. The shipped shape is a static per-pixel white hash (`hash21`)
scaled to the sampled mip's texel — not `ign`, whose dominant frequency beats against a lattice
into crescents when frozen. `assets/alpha_ladder_fixture` is the instrument; the 11.101 spec has
the four-way table.

### Wyman & McGuire, *Improved Alpha Testing Using Hashed Sampling*, TVCG 2017

- <https://casual-effects.com/research/Wyman2017Improved/Wyman2017Improved.pdf> — fetched
  2026-08-27. The extended journal version of *Hashed Alpha Testing* (I3D 2017); it supersedes
  the short paper and additionally covers alpha-to-coverage, which is the path cetra renders on.
- Local: `wyman-mcguire-2017-improved-alpha-testing-hashed-sampling.md`. IEEE copyright;
  committed until spec 13.14.

**What cetra takes from it:** nothing implemented. It is the *rejected alternative* — spec 11.31
deferred to it, 11.87 chose alpha-to-coverage plus sharpening instead, and this is the document
that decision is measured against. Kept because "we picked A2C over hashed alpha testing" is
only a real decision if the alternative is on hand.

Worth knowing before reaching for it: it replaces the alpha path rather than extending it, it
costs shader complexity in `pbr_frag` (which is at 16/16 samplers and ten of twelve UBO blocks),
and Yuksel measures it as introducing substantial noise.

---

## Shader-side anti-aliased alpha test

### Golus, *Anti-aliased Alpha Test: The Esoteric Alpha To Coverage* (2019)

- <https://bgolus.medium.com/anti-aliased-alpha-test-the-esoteric-alpha-to-coverage-8b177335ae4f>
- A blog post, no PDF.

**What cetra takes from it:** the sharpening in `cetra/shaders/include/alpha_coverage.glsl` —
`clamp((a - cutoff) / max(fwidth(a), eps) + 0.5, 0, 1)`, which expresses distance-to-threshold in
pixels so the transition is one pixel wide whatever the texture's own falloff is. Shipped in
11.87 and unchanged by 11.88.

---

## Screen-space ambient occlusion and specular occlusion

The GTAO sweep (`gtao_frag.glsl`) and the specular-occlusion term it feeds
(`include/spec_occ.glsl`, `spec_occ_composite_frag.glsl`). The visibility bitmask is the
live method; the bent cone is the predecessor it retired. Read across specs 4.2.1,
11.3–11.5 and 11.75–11.77.

### Therrien, Levesque & Gilet, *Screen Space Indirect Lighting with Visibility Bitmask*, The Visual Computer (2023)

- arXiv:2301.11376v2, DOI 10.1007/s00371-022-02703-y. Reference implementation and code
  notes: <https://cdrinmatane.github.io/posts/ssaovb-code/>. Converted from the arXiv PDF
  around 2026-08-23 (the file records no fetch date). arXiv's default non-exclusive licence,
  which grants rights to arXiv and nobody else; committed until spec 13.14.
- Local: `therrien-2023-screen-space-visibility-bitmask.md`

**What cetra takes from it — the method itself.** `gtao_frag.glsl` replaces GTAO's two
horizon angles with the paper's visibility bitmask: 32 angular sectors per hemisphere slice
packed into one `uint`, each depth sample marking the finite angular slab it occludes under
a constant thickness. AO is `popcount(mask) / 32` averaged over the slices, and the unset
(visible) bits carry the bent normal and the one-bounce SSGI gather. The finite thickness is
the whole point over an infinite height-field: light passes behind a thin surface instead of
being over-darkened into a halo, and no distance falloff is needed.

**Followed against the authors' own reference shader where the two disagree.** The hit
criterion is the ROUND rule of §3.1 / Figure 5 — a sector is set when at least half covered —
not the floor/ceil of the published code, which over-set about half a sector per slab (spec
11.76 measured heavily-occluded regions lightening ~1.3%, open regions bit-identical). The 32
sectors (§4.1) and the angular-space spacing (§3.1) are the paper's.

**Extended past the paper — the specular-occlusion term (spec 11.76).** The paper carries no
specular method. Its §3.2 template — one sampling direction per hemisphere subregion,
weighted by that subregion's unoccluded sectors — is applied to a *single* subregion, the GGX
reflection lobe: the lobe is projected into each slice and tested against the same mask
(`lobeSectors`), carried as two sums and divided last at the consumer (`specOccSplitAt`) so
the denoise chain averages quantities linear in visibility. This is the default `split` mode
and the engine's live specular occlusion; it replaced the Klehm 2011 cone term below, retired
in spec 11.77.

**Deviations:** a fixed `THICKNESS` constant rather than the paper's optional distance-linear
scaling; two slices per pixel with spatial and temporal jitter, against the paper's single
jittered slice per frame.

### Klehm, Ritschel, Eisemann & Seidel, *Bent Normals and Cones in Screen-space*, VMV 2011

- DOI 10.2312/PE/VMV/VMV11/177-182 (Vision, Modeling, and Visualization 2011, pp. 177–182).
  The committed copy records only the DOI — no source URL and no fetch date, unlike the
  entries above; add the author-hosted PDF link before trusting this bullet.
- Local: `klehm-2011-bent-normals-and-cones-screen-space.md`. Eurographics copyright;
  committed until spec 13.14.

**What cetra took from it, and retired.** Spec 11.3 built a specular-occlusion term from the
paper's bent cone — the AO chain's bent normal plus a length-derived aperture (§3.2)
intersected with a GGX reflection cone (spherical-cap overlap) — as `specOcclusionCone` in
`spec_occ.glsl`, and 11.4 carried it through the split-composite change. It shipped opt-in as
`--spec-occ bent` and the default flip never happened (frequency-mismatch mottling on smooth
metal). Spec 11.76 replaced the default with the bitmask term above, and spec 11.77 deleted
the cone path outright: `specOcclusionCone`, `coneOverlap` and `spec_lobe.glsl` are gone, and
`spec_occ.glsl`'s header is the only surviving mention.

**Why it went — the paper's own §3.2.** Klehm restricts the gather directions rather than
using the cone as a visibility gate; cetra used cone overlap AS the gate, which fails *dark*
where the paper's use fails *bright*, and that is what manufactured the contact ring specs
11.75–11.76 chased. The bitmask answers the directional question before anything collapses to
one cone.

**Kept for its derivations, not its code.** The bent *normal* production still lives in
`gtao_frag` (a debug view, booked for SSGI / SSR / DDGI); the bent *cone* does not.

---

## Fire

Read for spec 13.14: a GPU combustion grid rendered as an emitting, absorbing volume, the light
it casts derived from what it draws, and structural flames for candles. All fetched 2026-10-01.
**The "takes" fields are what the spec INTENDS to take**, written before any of it was built;
each is corrected in the phase that uses it, the way 11.88 found its blog post and its
reference code disagreeing.

### Nguyen, Fedkiw & Jensen, *Physically Based Modeling and Animation of Fire*, SIGGRAPH 2002

- <http://graphics.ucsd.edu/~henrik/papers/fire/fire.pdf> (Jensen's page). ACM copyright.
- Local: `nguyen-fedkiw-jensen-2002-physically-based-modeling-and-animation-of-fire.md`

**What cetra takes from it:** the base model. Fuel that burns into hot products and soot; those
rise by buoyancy; they are drawn by BLACKBODY emission from temperature; the reaction zone has
its own blue core; and products cool by a T⁴ radiative term. Phases 2 and 3.

### Fedkiw, Stam & Jensen, *Visual Simulation of Smoke*, SIGGRAPH 2001

- <http://graphics.ucsd.edu/~henrik/papers/smoke/smoke.pdf> (Jensen's page). ACM copyright.
- Local: `fedkiw-stam-jensen-2001-visual-simulation-of-smoke.md`

**What cetra takes from it:** Boussinesq buoyancy (temperature lifts, soot weighs) and
VORTICITY CONFINEMENT, the term that puts back the curls a coarse grid's numerical dissipation
takes out. Phase 2.

### Stam, *Stable Fluids*, SIGGRAPH 1999

- <http://www.dgp.toronto.edu/people/stam/reality/Research/pdf/ns.pdf> (Stam's page). ACM copyright.
- Local: `stam-1999-stable-fluids.md`

**What cetra takes from it:** semi-Lagrangian advection and the pressure projection, the
unconditionally stable skeleton every other step hangs on. Phase 2.

### Selle, Fedkiw, Kim, Liu & Rossignac, *An Unconditionally Stable MacCormack Method*, J. Sci. Comput. 2008

- <https://faculty.cc.gatech.edu/~jarek/papers/maccormack.pdf> (Rossignac's page). Springer copyright.
- Local: `selle-2008-unconditionally-stable-maccormack-method.md`

**What cetra takes from it:** second-order advection at semi-Lagrangian's cost plus one
backward pass, with the clamp to the source cell's range that keeps it stable. Detail at the
same grid resolution, which on GL 4.1 is the cheaper currency. Phase 2.

### Crane, Llamas & Tariq, *Real-Time Simulation and Rendering of 3D Fluids*, GPU Gems 3 ch. 30, 2007

- <https://www.cs.cmu.edu/~kmcrane/Projects/GPUFluid/paper.pdf> (Crane's page). Book copyright
  NVIDIA / Addison-Wesley; also readable free at
  <https://developer.nvidia.com/gpugems/gpugems3/part-v-physics-simulation/chapter-30-real-time-simulation-and-rendering-3d-fluids>.
- Local: `crane-llamas-tariq-2007-real-time-simulation-and-rendering-of-3d-fluids.md`

**What cetra takes from it:** the template. The whole solver as rasterised passes over 3D
texture slices, written for DX10 before compute shaders existed, which is exactly GL 4.1's
position: obstacles voxelised into the grid, fire as a combustion field, and the volume
ray-marched against the scene's depth. Phases 2 and 3.

### Pegoraro & Parker, *Physically-Based Realistic Fire Rendering*, EG Natural Phenomena 2006

- <https://www.sci.utah.edu/~vpegorar/research/2006_EGWNP.pdf> (Pegoraro's page). Eurographics copyright.
- Local: `pegoraro-parker-2006-physically-based-realistic-fire-rendering.md`

**What cetra takes from it:** emission as the soot's absorption coefficient times Planck
radiance, in absolute units, so a flame's brightness is a physical quantity beside the
engine's nits rather than a tuned colour ramp. Phases 1 and 3.

### Chadwick & James, *Animating Fire with Sound*, SIGGRAPH 2011

- <https://www.cs.cornell.edu/projects/Sound/fire/FireSound2011.pdf> (project page, which says
  its documents "may not be reposted without the explicit permission of the copyright holder").
- Local: `chadwick-james-2011-animating-fire-with-sound.md`

**What cetra takes from it:** the premise only — a flame's sound follows its heat release —
which is why the fire publishes `heat_release` for an app to swell a recorded crackle by. The
paper's synthesis is not built. Phase 4.

### Wrede, Wagner, Mahfuz, Pałubicki, Michels & Pirk, *Fire-X: Extinguishing Fire with Stoichiometric Heat Release*, ACM TOG 44(6), SIGGRAPH Asia 2025

- <https://helgewrede.github.io/firex/data/FireXExtinguishingFireWithStoichiometricHeatRelease.pdf>,
  DOI 10.1145/3763338.
- **CC BY 4.0, so committed:**
  [`wrede-2025-fire-x-stoichiometric-heat-release.md`](wrede-2025-fire-x-stoichiometric-heat-release.md),
  with its attribution in the file's header.

**What cetra takes from it:** nothing built. It is the current frontier — multi-species
thermodynamics, stoichiometric heat release, a hybrid SPH-grid for water on fire — and is on
hand as what the real-time model is a simplification OF, and for its flame colours
(blue-to-orange with mixture) as a reference to judge the blue core against.

### Linked only: no copy that may be fetched

- **Lamorlette & Foster, *Structural Modeling of Flames for a Production Environment*,
  SIGGRAPH 2002.** <https://dl.acm.org/doi/abs/10.1145/566570.566644>. ACM, no author copy
  found. Intended take: flames as SPINES with stochastic flicker and Kolmogorov noise, the
  candle tier of phase 6.
- **Horvath & Geiger, *Directable, High-Resolution Simulation of Fire on the GPU*,
  SIGGRAPH 2009.** <https://dl.acm.org/doi/10.1145/1576246.1531347>. ACM, no author copy
  found. Kept in reserve: a coarse 3D sim refined by view-aligned 2D slices, if the grid's
  detail falls short.
- **Nielsen, Bojsen-Hansen, Stamatelos & Bridson, *Physics-Based Combustion Simulation*,
  ACM TOG 41(5), 2022.** <https://dl.acm.org/doi/full/10.1145/3526213>. The ACM library
  refuses a scripted download and no author copy was found. Intended take: real fuels'
  adiabatic flame temperatures, as a source for the default temperatures.

---

## Local exposure

Read for spec 13.19: local tone mapping layered on the camera exposure, after Unreal 5's
bilateral Local Exposure. Fetched 2026-10-03.

### Durand & Dorsey, *Fast Bilateral Filtering for the Display of High-Dynamic-Range Images*, SIGGRAPH 2002

- <https://people.csail.mit.edu/fredo/PUBLI/Siggraph2002/DurandBilateral.pdf> (Durand's page).
  ACM copyright.
- Local: `durand-dorsey-2002-fast-bilateral-filtering-hdr.md`

**What cetra takes from it:** the method. Log luminance split into a bilateral-filtered BASE and
a DETAIL; only the base's contrast is reduced; colour recomposed by ratios. Also its account of
the halos that survive at antialiased edges and around flare, which is what the blurred-luminance
blend answers.

### Chen, Paris & Durand, *Real-time Edge-Aware Image Processing with the Bilateral Grid*, SIGGRAPH 2007

- <https://people.csail.mit.edu/sparis/publi/2007/siggraph/Chen_07_Bilateral_Grid.pdf> (Paris's
  page). ACM copyright.
- Local: `chen-paris-durand-2007-bilateral-grid.md`

**What cetra takes from it:** the GPU data structure. Homogeneous `(sum, weight)` cells, a
separable 5-tap Gaussian over the grid in a fragment shader, slicing by the pixel's own
luminance, and z levels tiled across one 2D texture so a slice is two bilinear taps.

### Unreal Engine 5, *Local Exposure* (Epic documentation), and its port in kansei PR #49

- <https://dev.epicgames.com/documentation/en-us/unreal-engine/auto-exposure-in-unreal-engine>
- <https://github.com/Siroko/kansei/pull/49>, a port of Unreal's bilateral local exposure that
  states its grid sizes and per-pixel formula.
- Web pages, no PDF.

**What cetra takes from them:** the engine shape. Cells of 64x64 half-res texels, 32 bins over
30 stops, each texel split between its two nearest bins; a base blended with a heavily blurred
luminance (default 0.6) against ringing; highlight and shadow contrast scaled separately about
middle grey, plus a detail strength; and defaults that change nothing.

---

## Film grain: noise that is new every frame

Read for spec 13.28, after the grain was found crawling: the frame number was added to both pixel
coordinates of a sin-fract hash, so each frame was the last one moved one pixel diagonally.

### Jarzynski & Olano, *Hash Functions for GPU Rendering*, JCGT 9(3), 2020

- <https://jcgt.org/published/0009/03/02/paper.pdf>, fetched 2026-10-07. CC BY-ND 3.0; the
  conversion is kept local like the rest, the licence forbidding derivatives.
- Local: `jarzynski-olano-2020-hash-functions-gpu-rendering.md`

**What cetra takes from it:** `pcg3d`, §6.1, as written: a (3 -> 3) hash of the pixel and the
frame, whose every output word changes when any input does, so a frame's noise owes nothing to the
last frame's and nothing slides. The paper's Figure 1 shows the sin-fract form (`trig`) with
"visible banding, linear artifacts", which is the second half of what the grain looked like.

---

## CRT

Read for spec 13.28: a consumer television drawn over the finished frame, game UI included.
Fetched 2026-10-07. Both are shader source, not papers, and both are public domain; kept local
with the rest all the same, the libretro repositories being where to get them again.

### Lottes, *CRTS: public domain CRT-styled scalar*, 2018

- <https://www.shadertoy.com/view/MtSfRK>; read as hunterk's libretro port,
  <https://raw.githubusercontent.com/libretro/slang-shaders/master/crt/shaders/crt-lottes-fast.slang>.
  The Unlicense.
- Local: `lottes-2018-crts-crt-lottes-fast.slang`

**What cetra takes from it:** the filter. Two scanlines each a windowed cosine
(`cos(min(0.5, off * thin) * 2pi) * 0.5 + 0.5`), four taps across each under `exp2(blur * d^2)`,
the warp `pos *= (1 + pos.y^2 * warp.x, 1 + pos.x^2 * warp.y)` with its rounded vignette at the
tube's edge, and the exposure match, `midOut = 0.18 / (scan factor * mask factor)` through
`peak / (peak * tone.y + tone.z)`, which lifts mid-grey back to where it was. Its own warning
holds here: the input must already be low resolution, which is why the finished frame is
resampled to a few hundred lines first.

### Lottes, *PUBLIC DOMAIN CRT STYLED SCAN-LINE SHADER*, 2014

- <https://raw.githubusercontent.com/libretro/glsl-shaders/master/crt/shaders/crt-lottes.glsl>.
  Public domain.
- Local: `lottes-2014-crt-lottes.glsl`

**What cetra takes from it:** only the "very compressed TV style" mask (`shadowMask == 1`), a
slot mask, which is what a living-room set had and CRTS does not carry: RGB stripes three pixels
wide, and a dark row every second line, offset by one row in alternate groups of three columns.
Made darken-only here, CRTS' way, so it cannot clip a bright pixel.
