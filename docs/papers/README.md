# Reference material

Sources this engine's implementations are read off, kept here because the answers to the
questions that actually bit us were **not** in the prose everybody cites — they were in shipped
code, and two of those implementations disagree with each other.

Each entry records the citation, where it came from, when, and **the one claim cetra takes from
it**. That last field is the point of this directory: a paper in a repo with no statement of
what was used from it is decoration.

Papers are converted with `pdf2md` and committed as markdown rather than PDF — diffable,
greppable, and a fraction of the size. The originals are author-hosted and linked below.

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
- [`yuksel-2018-alpha-distribution-for-alpha-testing.md`](yuksel-2018-alpha-distribution-for-alpha-testing.md)
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
- [`wyman-mcguire-2017-improved-alpha-testing-hashed-sampling.md`](wyman-mcguire-2017-improved-alpha-testing-hashed-sampling.md)

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
