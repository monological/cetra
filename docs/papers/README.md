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

**What cetra takes from it — two things, and neither is code:**

1. **The honest ceiling on what 11.88 builds.** §2, on this whole family: *"Castaño suggests
   scaling the alpha values by first finding a desirable alpha threshold per mipmap level... This
   simple fix can help in some cases, but it does not always improve the results."* Figures 1-3
   show it failing. Cetra is implementing the technique correctly, not solving alpha testing.
2. **A third independent vote for the pristine chain.** Both of its algorithms process "each
   mipmap level completely independently".

**Not implemented:** alpha distribution itself (error diffusion, alpha pyramid). It is the named
successor and has a roadmap row.

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
