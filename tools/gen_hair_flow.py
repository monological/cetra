#!/usr/bin/env python3
"""Derive a hair strand map from a hair card atlas.

    python3 tools/gen_hair_flow.py atlas.png                  # -> atlas.hair.png
    python3 tools/gen_hair_flow.py atlas.png -o out.png --debug

Run once per atlas; the result is an ordinary texture the engine loads through
the material mask array. Needs numpy, Pillow and scipy -- it is an offline
authoring tool, not part of the build, so nothing in CI imports it.

WHAT THIS PRODUCES, AND WHY THE ENGINE NEEDS IT

A hair card is a flat quad, so every fragment on it shares one interpolated
tangent. An anisotropic specular lobe keyed on that tangent therefore fires
across the WHOLE card at once and reads as a flat sheet rather than as hair.
The strands the artist painted are real, but they exist only in the texture --
the geometry knows nothing about them.

This tool recovers them and writes them where the shader can read them:

    R, G   strand ORIENTATION, as a coherence-weighted doubled-angle vector
    B      strand ID: constant along a strand, uncorrelated between neighbours
    A      unused (255)

DOUBLED ANGLE IS LOAD-BEARING, NOT NOTATION

A strand has no head or tail: a direction and its negation describe the same
strand. Store a raw direction and every averaging step destroys the field --
mask_array_build resamples each layer through a linear filter and then
glGenerateMipmap builds the chain, and averaging d with -d gives zero. Hair is
exactly the content that ends up minified.

Storing (cos 2t, sin 2t) makes antiparallel directions IDENTICAL, so every
average is well defined. The shader halves the angle back on read.

COHERENCE AS THE VECTOR'S LENGTH

Rather than a separate channel. Averaging coherence-weighted orientation
vectors is the correct way to downsample an orientation field: a neighbourhood
of agreeing strands keeps its length, a neighbourhood of disagreeing ones
shortens toward zero on its own. So the mip chain degrades honestly -- distant
hair loses its per-strand detail and the shader falls back toward the card
tangent, which is what you want anyway. It also keeps the layer to three
channels, matching the ORM convention the mask array already uses.

WHICH SIGNAL THE ORIENTATION COMES FROM

Both alpha and luminance, as SUMMED structure tensors. Tensors add, and each
one's magnitude scales with its own gradient energy, so whichever channel
actually carries the strands dominates automatically and no weighting constant
has to be guessed. This matters because atlases differ: on the raiden groom
alpha is the strand stencil and carries about twice the cross-strand structure
that luminance does (measured: mean |d/dx| 0.066 vs 0.034), but an atlas of
opaque cards with strands painted in colour is the other way round.

WHY LINE INTEGRAL CONVOLUTION FOR THE STRAND ID

The ID has to be constant ALONG a strand and uncorrelated ACROSS neighbours --
otherwise the highlight beads into dashes instead of running the strand's
length. That is precisely LIC's defining property: smear noise along the
field's streamlines. Nothing simpler has it. A hash of the texel coordinate
does not: it is constant along whatever axis it happens to key on, which
correlates with the strands only if they run exactly that way, which real
grooms never do (raiden's lean up to ~30 degrees off vertical and curve).

The walk interpolates the DOUBLED-ANGLE field and decodes per step rather than
interpolating a direction, for the same reason the encoding exists at all: only
the doubled-angle form survives filtering across a strand boundary.

WHAT THE TENSOR WINDOW IS SET AGAINST

A card's own CUT edge is not a strand, but it is a strong straight gradient, so
a narrow window reads the horizontal slice across a clump's roots as horizontal
hair. Measured on the raiden atlas, over the 8 texels below each card's first
opaque row: at sigma 2.0, 57% of confident texels there read horizontal against
5.1% in the body -- i.e. the band is wrong, not merely noisy. At sigma 4.0 that
falls to 39.7% over half as many texels, because the disagreement lowers
coherence and the encoding reports its own uncertainty.

Widening costs nothing elsewhere: the two settings agree to 0.91 degrees median
(3.29 at the 90th percentile) and orientation spread drops 3%, so sigma 4.0 is
not oversmoothing the curved strands -- it differs only where sigma 2.0 was
being fooled. Hence the default.

It does not fix the band entirely, and the obvious erosion of the card outline
is worse: strand TIPS are thin, so eroding deletes them, and the tips matter
more than the roots (they are the silhouette, and the roots are usually buried
under other cards). The residue is ~0.24% of strand texels.
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

# Below this alpha there is no geometry, so there is no strand to have an
# orientation. Written as coherence 0, which the shader reads as "use the card
# tangent" -- an absent answer rather than a wrong one.
ALPHA_FLOOR = 0.03

# Guards the divisions that normalise the tensor. Structure-tensor magnitudes
# are squared gradients of an 8-bit signal, so a flat region lands many orders
# below this rather than near it.
TINY = 1e-12


def load_atlas(path):
    """Return (luminance, alpha) in [0,1], both float64 and the same shape."""
    img = np.asarray(Image.open(path).convert("RGBA")).astype(np.float64) / 255.0
    lum = img[..., :3] @ np.array([0.2126, 0.7152, 0.0722])
    return lum, img[..., 3]


def structure_tensor(signal, sigma):
    """Gaussian-smoothed outer product of the gradient: (Jxx, Jyy, Jxy)."""
    gy, gx = np.gradient(signal)
    return (
        ndimage.gaussian_filter(gx * gx, sigma),
        ndimage.gaussian_filter(gy * gy, sigma),
        ndimage.gaussian_filter(gx * gy, sigma),
    )


def strand_orientation(lum, alpha, sigma):
    """Doubled-angle unit vector of the strand direction, plus coherence."""
    axx, ayy, axy = structure_tensor(alpha, sigma)
    lxx, lyy, lxy = structure_tensor(lum, sigma)
    jxx, jyy, jxy = axx + lxx, ayy + lyy, axy + lxy

    # (Jxx - Jyy, 2 Jxy) is the doubled-angle vector of the dominant GRADIENT.
    # A strand runs perpendicular to its own edges, and rotating by 90 degrees
    # doubles to 180 -- which in doubled-angle space is exactly a negation.
    dx2 = -(jxx - jyy)
    dy2 = -2.0 * jxy

    mag = np.hypot(dx2, dy2)
    trace = jxx + jyy
    coherence = np.divide(mag, trace, out=np.zeros_like(mag), where=trace > TINY)
    np.clip(coherence, 0.0, 1.0, out=coherence)
    coherence[alpha <= ALPHA_FLOOR] = 0.0

    ux = np.divide(dx2, mag, out=np.zeros_like(mag), where=mag > TINY)
    uy = np.divide(dy2, mag, out=np.zeros_like(mag), where=mag > TINY)
    return ux, uy, coherence


def _sample(field, py, px):
    return ndimage.map_coordinates(field, np.stack([py, px]), order=1, mode="nearest")


def strand_id(ux, uy, steps, step_len, strand_px, seed):
    """LIC: white noise smeared along the strands, so it is constant per strand."""
    height, width = ux.shape
    rng = np.random.default_rng(seed)
    noise = rng.random((height, width))
    # Shape the noise to the strand width first, or the ID varies per texel and
    # the jitter it drives becomes sub-pixel noise that TAA eats rather than
    # per-strand variation.
    if strand_px > 1.0:
        noise = ndimage.gaussian_filter(noise, strand_px / 2.0)

    yy, xx = np.mgrid[0:height, 0:width].astype(np.float64)
    acc = noise.copy()
    count = np.ones_like(noise)

    # Walk both ways from every texel at once. Orientation has no sign, so each
    # step re-aligns the new direction with the one being followed; without that
    # the walk reverses on itself wherever the decoded angle wraps.
    for direction in (1.0, -1.0):
        px, py = xx.copy(), yy.copy()
        cx = np.cos(0.5 * np.arctan2(uy, ux)) * direction
        cy = np.sin(0.5 * np.arctan2(uy, ux)) * direction
        for _ in range(steps):
            px = np.clip(px + cx * step_len, 0.0, width - 1.0)
            py = np.clip(py + cy * step_len, 0.0, height - 1.0)
            acc += _sample(noise, py, px)
            count += 1.0
            angle = 0.5 * np.arctan2(_sample(uy, py, px), _sample(ux, py, px))
            nx, ny = np.cos(angle), np.sin(angle)
            sign = np.sign(cx * nx + cy * ny)
            sign[sign == 0.0] = 1.0
            cx, cy = nx * sign, ny * sign

    return acc / count


def stretch(values, mask):
    """Rescale to [0,1] on the 2nd..98th percentile of the masked region.

    Averaging 2 * steps samples collapses the noise toward its mean, so without
    this the ID would occupy a few of the 256 codes an 8-bit channel offers and
    quantise into terraces.
    """
    out = np.full_like(values, 0.5)
    if not mask.any():
        return out
    lo, hi = np.percentile(values[mask], [2.0, 98.0])
    if hi - lo < TINY:
        return out
    out[mask] = np.clip((values[mask] - lo) / (hi - lo), 0.0, 1.0)
    return out


def write_debug(path, ux, uy, coherence, ident):
    """Orientation as hue, coherence, and strand ID -- three panels."""
    angle = 0.5 * np.arctan2(uy, ux)
    hue = ((angle / np.pi) + 0.5) % 1.0
    hsv = np.stack(
        [hue * 255.0, np.full_like(hue, 255.0), coherence * 255.0], axis=-1
    ).astype(np.uint8)
    orient = np.asarray(Image.fromarray(hsv, "HSV").convert("RGB"))
    grey = lambda v: np.repeat((v * 255.0).astype(np.uint8)[..., None], 3, axis=-1)
    Image.fromarray(np.concatenate([orient, grey(coherence), grey(ident)], axis=1)).save(path)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Derive a hair strand map from an atlas.")
    ap.add_argument("atlas", help="hair card albedo PNG (RGBA)")
    ap.add_argument("-o", "--out", help="output PNG (default: <atlas>.hair.png)")
    ap.add_argument("--sigma", type=float, default=4.0,
                    help="structure-tensor smoothing in texels (default 4.0)")
    ap.add_argument("--strand-px", type=float, default=3.0,
                    help="expected strand width in texels (default 3.0)")
    ap.add_argument("--lic-steps", type=int, default=24,
                    help="LIC half-length in texels (default 24)")
    ap.add_argument("--seed", type=int, default=0,
                    help="noise seed; fixed so the map is reproducible (default 0)")
    ap.add_argument("--debug", action="store_true",
                    help="also write <out>.debug.png: orientation, coherence, strand ID")
    args = ap.parse_args(argv)

    out_path = args.out or os.path.splitext(args.atlas)[0] + ".hair.png"

    lum, alpha = load_atlas(args.atlas)
    ux, uy, coherence = strand_orientation(lum, alpha, args.sigma)
    ident = stretch(
        strand_id(ux, uy, args.lic_steps, 1.0, args.strand_px, args.seed),
        alpha > ALPHA_FLOOR,
    )

    rgba = np.zeros(lum.shape + (4,), dtype=np.uint8)
    rgba[..., 0] = np.round((ux * coherence * 0.5 + 0.5) * 255.0)
    rgba[..., 1] = np.round((uy * coherence * 0.5 + 0.5) * 255.0)
    rgba[..., 2] = np.round(ident * 255.0)
    rgba[..., 3] = 255
    Image.fromarray(rgba, "RGBA").save(out_path)

    inside = alpha > ALPHA_FLOOR
    print("%s -> %s (%dx%d)" % (args.atlas, out_path, lum.shape[1], lum.shape[0]))
    print("  strand texels   %.1f%%" % (100.0 * inside.mean()))
    if inside.any():
        print("  coherence       mean %.3f  median %.3f" %
              (coherence[inside].mean(), np.median(coherence[inside])))
        print("  strand id       std %.3f (0 = no per-strand variation)" % ident[inside].std())

    if args.debug:
        debug_path = os.path.splitext(out_path)[0] + ".debug.png"
        write_debug(debug_path, ux, uy, coherence, ident)
        print("  debug           %s" % debug_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
