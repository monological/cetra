"""Draw the chain-link fence apps/silent's yards are closed with (spec 13.35).

A scan would need an opacity channel, and the fetcher's sets have none, so the mesh is drawn:
two families of diagonal wires, a diamond between them, galvanised steel going to rust in
patches. Everything tiles, the noise included (it is white noise blurred through the FFT, which
wraps), so the fence runs on for any length without a seam.

Writes chainlink_{albedo,normal,rough}.png into assets/textures/silent/, the albedo RGBA with
the wire in alpha, and like every map there BOTTOM ROW FIRST (see fetch_textures.py). One repeat
is REPEAT_M of fence; mats.c's repeat for it must agree.

    python3 apps/silent/tools/make_chainlink.py
"""

import os
import sys

import numpy as np
from PIL import Image

from fetch_textures import OUT_DIR

SIZE = 256
REPEAT_M = 0.5
# Wires cross the picture's edge every PITCH texels along x, which is 8 diamonds a repeat: the
# wires 4.4 cm apart, near the 5 cm of an ordinary residential mesh.
PITCH = 32
# The wire's radius in texels: 2.2 mm, a little over a real 11 gauge's 1.5, since anything
# thinner is gone by a few metres away.
RADIUS = 1.1
SEED = 1335

GALVANISED = np.array([0.56, 0.58, 0.57])  # sRGB, weathered zinc
RUST = np.array([0.42, 0.25, 0.15])
DIRT = np.array([0.30, 0.29, 0.26])


def periodic_noise(rng, sigma):
    """Tiling noise, 0..1: white noise blurred by a Gaussian of `sigma` texels in the frequency
    domain, which is a circular convolution and so wraps at the edges."""
    white = rng.standard_normal((SIZE, SIZE))
    f = np.fft.fftfreq(SIZE)
    fx, fy = np.meshgrid(f, f)
    kernel = np.exp(-2.0 * (np.pi * sigma) ** 2 * (fx * fx + fy * fy))
    n = np.real(np.fft.ifft2(np.fft.fft2(white) * kernel))
    return (n - n.min()) / (n.max() - n.min())


def wire_distance(x, y, sign):
    """Texels from the nearest wire of the family running along x + sign * y = k * PITCH,
    measured square to it."""
    s = (x + sign * y) / PITCH
    return np.abs(s - np.round(s)) * PITCH / np.sqrt(2.0)


def main():
    rng = np.random.default_rng(SEED)
    # Texel centres, so the picture is symmetric about its middle and tiles exactly.
    y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64) + 0.5
    da = wire_distance(x, y, 1.0)
    db = wire_distance(x, y, -1.0)
    d = np.minimum(da, db)
    # Coverage, antialiased over a texel; the engine keeps it through the mips.
    alpha = np.clip(RADIUS - d + 0.5, 0.0, 1.0)

    # A round wire's height across it; where two cross, one runs over the other.
    def bulge(dist):
        t = np.clip(dist / (RADIUS + 0.5), 0.0, 1.0)
        return np.sqrt(1.0 - t * t)
    height = np.maximum(bulge(da), 0.8 * bulge(db))
    gy, gx = np.gradient(height)
    strength = 1.6
    n = np.stack([-gx * strength, -gy * strength, np.ones_like(height)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)

    rust = np.clip((periodic_noise(rng, 14.0) - 0.55) * 4.0, 0.0, 1.0)
    rust *= periodic_noise(rng, 3.0) * 0.6 + 0.4
    grime = periodic_noise(rng, 24.0)
    # Dirt collects where the wires hook round each other.
    knot = np.exp(-(np.maximum(da, db) / 2.5) ** 2)
    colour = GALVANISED[None, None, :] * (0.85 + 0.3 * grime[..., None])
    colour = colour * (1.0 - rust[..., None]) + RUST[None, None, :] * rust[..., None]
    colour = colour * (1.0 - 0.5 * knot[..., None]) + DIRT[None, None, :] * 0.5 * knot[..., None]
    rough = 0.5 + 0.4 * rust + 0.1 * knot

    def save(array, name, mode):
        img = Image.fromarray(np.clip(array * 255.0 + 0.5, 0, 255).astype(np.uint8), mode)
        img.transpose(Image.Transpose.FLIP_TOP_BOTTOM).save(
            os.path.join(OUT_DIR, "chainlink_%s.png" % name))

    save(np.concatenate([colour, alpha[..., None]], axis=-1), "albedo", "RGBA")
    save(n * 0.5 + 0.5, "normal", "RGB")
    # Grey in all three channels, as the engine reads a roughness map's green.
    save(np.repeat(rough[..., None], 3, axis=-1), "rough", "RGB")
    print("chainlink: %d px for %.2f m, %d diamonds a repeat" % (SIZE, REPEAT_M, SIZE // PITCH))
    return 0


if __name__ == "__main__":
    sys.exit(main())
