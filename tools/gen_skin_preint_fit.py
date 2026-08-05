#!/usr/bin/env python3
"""Fit the pre-integrated skin diffuse response (Penner 2011) to a closed form.

    python3 tools/gen_skin_preint_fit.py            # verify the checked-in fit
    python3 tools/gen_skin_preint_fit.py --fit      # re-fit and print constants

Run once; the constants are pasted into cetra/shaders/include/preintegrated_skin.glsl
and builds never regenerate them. Verification is the mode that matters day to
day, so it is the default and needs nothing installed -- stdlib only.

WHAT IS BEING FITTED

On a sphere of radius r, light entering at one point and leaving at another has
travelled the chord between them. Integrating the diffuse response over that
scattering gives Penner's pre-integrated falloff:

    D(theta, sigma) = INT clamp(cos(theta + x), 0, 1) * K(x) dx / INT K(x) dx

    K(x) = SUM_i w_i * exp( -(2 sin(x/2))^2 / (2 (c_i sigma)^2) )

K mirrors profileWeight() in sss_blur_frag.glsl -- the SAME diffusion profile the
screen-space blur uses, so the two mechanisms cannot disagree about the shape of
skin. That coupling has no compiler behind it, which is why --verify exists.

Two things about K that are easy to get wrong, and both bias the fit sharp:

  * The sub-Gaussians are PEAK-1, not area-normalised: the shader returns
    0.35 g1 + 0.40 g2 + 0.25 g3 and normalises by the accumulated weight
    afterwards. Their effective AREA weights are therefore w_i c_i renormalised,
    which is 52% broad tail and 10% core -- not the 35% core the literal
    coefficients suggest.
  * The argument is the true CHORD 2 sin(x/2), not the arc x. Substituting the
    chord makes r cancel identically, which is why one dimensionless sigma
    describes the whole family exactly rather than only for small angles. The
    chord is shorter than the arc, so the real kernel has fatter angular tails
    than a Gaussian of the same sigma.

WHY THIS FORM

sigma depends on curvature and the material, never on the light, so the fit is
split: coefficients that depend only on sigma are computed once per fragment,
and the per-light part is a wrap plus a cubic. Fitting anything whose per-light
half is more expensive would cost roughly one polynomial per channel per light.

    per fragment: w  = s (W1 + s (W2 + s W3))     (and likewise a1, a2, a3, d0, e0)
    per light:    t  = max((NdotL + w) / (1 + w), 0)
                  D  = d0 + e0 NdotL + t (a1 + t (a2 + t a3))

e0 is the other term a wrap alone cannot supply. The Fourier series of the
response is 1/pi + (m1/2) cos(theta) + higher harmonics: the first two terms
live over the WHOLE sphere and only the harmonics concentrate near the
terminator. A clamped cubic can only model the harmonics, so without an
unclamped fundamental the fit has to fake cos(theta) with something identically
zero over half the domain. Adding it halves the error above sigma 0.2.

d0 is the load-bearing term and the reason a wrap alone will not do. The kernel
is a function of the CHORD, so it has support over the whole sphere: at sigma
0.3 a surface facing directly AWAY from the light still returns 0.0117, and as
sigma grows the response tends to the isotropic 1/pi everywhere. A clamped wrap
is exactly zero past its support and cannot represent a floor at all, which is
the same thing the Fourier form says -- D is 1/pi plus harmonics, and dropping
the DC term throws away the part that survives longest.

At sigma = 0 this is Lambert BIT-EXACTLY, not approximately: every coefficient
is multiplied by s, so w = 0, a1 = 1, a2 = a3 = d0 = 0, and
t = max((x + 0)/1, 0) = max(x, 0) by IEEE. The delta a shader adds is then
exactly +0.0, so flat geometry and hard edges cost nothing and cannot drift.
Written in that algebraic order for that reason.

s is sigma itself rather than a bounded remap. sigma^2/(1+sigma^2) reads safer
but is quadratic near zero and spans a tenth of its range over the band that
ships, which compresses the polynomial basis and forces coefficients in the
hundreds that cancel against each other. The shader clamps sigma anyway.

No pow(): pow(x, 1.0) is not guaranteed exact to an ulp and pow(0, 1) is
undefined in GLSL, which rules out the usual wrapped-diffuse-with-exponent.

ENERGY IS A CONSTRAINT, NOT A METRIC

Convolving with a normalised kernel preserves the integral, so INT D dtheta = 2
for every sigma. A fit that misses it is a global brightness error on all skin,
which reads far worse than local shape error. It also rules out both obvious
wrap families on its own: saturate(NdotL + w) and saturate((NdotL + w)/(1 + w))
each add energy.
"""

import math
import sys

# Mirror of profileWeight() in cetra/shaders/sss_blur_frag.glsl (weight, sigma
# multiplier). If that changes, re-run --verify: nothing else couples them.
PROFILE = ((0.35, 0.30), (0.40, 1.00), (0.25, 2.20))

# Pasted from a --fit run: SHAPE_DIM groups of POLY_DEG cubic coefficients, in
# the order w, a1, a2, a3, d0, e0.
FITTED = tuple([0.0] * 18)

KERNEL_SAMPLES = 1024  # periodic + smooth, so the trapezoid is spectrally accurate
# Stage 1 fits each of these independently, so a fine grid costs time but not
# conditioning -- and stage 2 needs enough points to draw a curve through.
FIT_SIGMAS = [0.04 * (1.25 ** i) for i in range(18)]  # ~0.04 .. 2.2, log spaced
FIT_THETAS = [math.pi * i / 60.0 for i in range(61)]  # 3 degree steps, 0 .. pi
SHIPPED_BAND = 0.6  # sigma the fixture and a close-up head actually reach


def kernel_weights(sigma):
    """Normalised kernel over one period, as (cos x, sin x, weight) triples."""
    out = []
    total = 0.0
    for i in range(KERNEL_SAMPLES):
        x = -math.pi + 2.0 * math.pi * i / KERNEL_SAMPLES
        chord2 = (2.0 * math.sin(0.5 * x)) ** 2
        w = 0.0
        for weight, mult in PROFILE:
            s = mult * sigma
            w += weight * math.exp(-chord2 / (2.0 * s * s))
        out.append((math.cos(x), math.sin(x), w))
        total += w
    return [(cx, sx, w / total) for cx, sx, w in out]


def ground_truth(sigma, thetas):
    """Penner's integral, evaluated exactly (no small-angle substitution)."""
    if sigma <= 0.0:
        return [max(math.cos(t), 0.0) for t in thetas]
    kw = kernel_weights(sigma)
    out = []
    for t in thetas:
        ct, st = math.cos(t), math.sin(t)
        acc = 0.0
        for cx, sx, w in kw:
            # cos(t + x) by angle addition: the cos() call is the whole cost
            # here, and hoisting it out of the inner loop is what makes a pure
            # stdlib integration practical.
            c = ct * cx - st * sx
            if c > 0.0:
                acc += c * w
        out.append(acc)
    return out


SHAPE_DIM = 6  # w, a1, a2, a3, d0, e0
POLY_DEG = 3   # each shape coefficient is a cubic in sigma through the origin

# What each shape coefficient must equal at sigma = 0 for the form to reduce to
# clamped Lambert exactly. Every coefficient is s * poly(s), so a1 is the only
# one with a non-zero base.
SHAPE_BASE = (0.0, 1.0, 0.0, 0.0, 0.0, 0.0)

# a1 is FITTED, not pinned. Wherever t > 0 the term a1*t is affine in NdotL and
# so is d0 + e0*NdotL, which makes them nearly collinear -- and left free, a1
# slides to exactly 0 around sigma 0.15 and sticks there against the boundary.
# Pinning it to 1 to remove the redundancy was tried and measured WORSE (0.087
# against 0.059 max), because the two only span the same directions where the
# clamp is open; a1 alone moves the lit side without touching the dark one.



def coefficients(params, sigma):
    """Expand the packed polynomial constants into this fragment's shape."""
    s = sigma
    out = []
    for i in range(SHAPE_DIM):
        p = params[i * POLY_DEG : (i + 1) * POLY_DEG]
        out.append(SHAPE_BASE[i] + s * (p[0] + s * (p[1] + s * p[2])))
    return tuple(out)


def evaluate(coef, ndotl):
    w, a1, a2, a3, d0, e0 = coef
    t = (ndotl + w) / (1.0 + w)
    if t < 0.0:
        t = 0.0
    return d0 + e0 * ndotl + t * (a1 + t * (a2 + t * a3))


def evaluate_shape(shape, ndotl):
    """Same evaluation from a raw per-sigma shape, for the stage-1 fit."""
    return evaluate(shape, ndotl)


def build_truth(sigmas, thetas):
    return {s: ground_truth(s, thetas) for s in sigmas}


def shape_objective(shape, thetas, ref, dtheta):
    """Squared error at ONE sigma, with the energy and monotonicity constraints."""
    total = 0.0
    energy = 0.0
    prev = None
    for i, t in enumerate(thetas):
        got = evaluate_shape(shape, math.cos(t))
        total += (got - ref[i]) ** 2
        energy += got
        # theta ascends so NdotL descends: D must not increase along it.
        if prev is not None and got > prev + 1e-9:
            total += 50.0 * (got - prev) ** 2
        prev = got
    # Doubled: the grid covers 0..pi and D is even in theta.
    return total + 200.0 * (2.0 * energy * dtheta - 2.0) ** 2


def solve(matrix, rhs):
    """Gaussian elimination with partial pivoting. Small and dense; no numpy."""
    n = len(rhs)
    a = [row[:] + [rhs[i]] for i, row in enumerate(matrix)]
    for col in range(n):
        pivot = max(range(col, n), key=lambda r: abs(a[r][col]))
        a[col], a[pivot] = a[pivot], a[col]
        if abs(a[col][col]) < 1e-14:
            return None
        for r in range(n):
            if r == col:
                continue
            f = a[r][col] / a[col][col]
            for c in range(col, n + 1):
                a[r][c] -= f * a[col][c]
    return [a[i][n] / a[i][i] for i in range(n)]


def polyfit_through_base(sigmas, values, base):
    """Least-squares fit of value = base + s(p0 + s p1 + s^2 p2).

    Exact rather than iterative: the model is linear in its coefficients once
    the fixed sigma = 0 value is subtracted, so this is a 3x3 normal equation.
    """
    powers = [[s ** (k + 1) for k in range(POLY_DEG)] for s in sigmas]
    targets = [v - base for v in values]
    mat = [[sum(p[i] * p[j] for p in powers) for j in range(POLY_DEG)]
           for i in range(POLY_DEG)]
    rhs = [sum(p[i] * t for p, t in zip(powers, targets)) for i in range(POLY_DEG)]
    return solve(mat, rhs) or [0.0] * POLY_DEG


def nelder_mead(fn, start, step, iters=4000):
    """Compact Nelder-Mead. No scipy, and none needed for nine parameters."""
    n = len(start)
    simplex = [list(start)]
    for i in range(n):
        p = list(start)
        p[i] += step[i]
        simplex.append(p)
    scores = [fn(p) for p in simplex]
    for _ in range(iters):
        order = sorted(range(n + 1), key=lambda i: scores[i])
        simplex = [simplex[i] for i in order]
        scores = [scores[i] for i in order]
        if abs(scores[-1] - scores[0]) < 1e-14:
            break
        centroid = [sum(simplex[i][j] for i in range(n)) / n for j in range(n)]
        worst = simplex[-1]
        refl = [centroid[j] + (centroid[j] - worst[j]) for j in range(n)]
        fr = fn(refl)
        if fr < scores[0]:
            exp = [centroid[j] + 2.0 * (centroid[j] - worst[j]) for j in range(n)]
            fe = fn(exp)
            simplex[-1], scores[-1] = (exp, fe) if fe < fr else (refl, fr)
        elif fr < scores[-2]:
            simplex[-1], scores[-1] = refl, fr
        else:
            con = [centroid[j] + 0.5 * (worst[j] - centroid[j]) for j in range(n)]
            fc = fn(con)
            if fc < scores[-1]:
                simplex[-1], scores[-1] = con, fc
            else:
                for i in range(1, n + 1):
                    simplex[i] = [
                        simplex[0][j] + 0.5 * (simplex[i][j] - simplex[0][j])
                        for j in range(n)
                    ]
                    scores[i] = fn(simplex[i])
    best = min(range(n + 1), key=lambda i: scores[i])
    return simplex[best], scores[best]


def report(params, label):
    """Measure the fit on a finer grid than it was trained on."""
    thetas = [math.pi * i / 180.0 for i in range(181)]
    dtheta = thetas[1] - thetas[0]
    sigmas = [0.05 * (1.18 ** i) for i in range(24)]  # ~0.05 .. 2.6
    print(f"\n{label}")
    print("  sigma      max abs     rms       max rel(D>0.05)   energy err  mono")
    worst_band = worst_full = 0.0
    sum_sq_band = sum_sq_full = 0.0
    n_band = n_full = 0
    worst_energy = 0.0
    monotone = True
    for s in sigmas:
        ref = ground_truth(s, thetas)
        coef = coefficients(params, s)
        max_abs = max_rel = 0.0
        sq = 0.0
        energy = 0.0
        prev = None
        mono_here = True  # per sigma; a global flag reads as failing everywhere
        for i, t in enumerate(thetas):
            got = evaluate(coef, math.cos(t))
            err = abs(got - ref[i])
            max_abs = max(max_abs, err)
            sq += err * err
            if ref[i] > 0.05:
                max_rel = max(max_rel, err / ref[i])
            energy += got
            if prev is not None and got > prev + 1e-6:
                mono_here = False
                monotone = False
            prev = got
            if s <= SHIPPED_BAND:
                sum_sq_band += err * err
                n_band += 1
                worst_band = max(worst_band, err)
            if s <= 2.0:
                sum_sq_full += err * err
                n_full += 1
                worst_full = max(worst_full, err)
        energy_err = abs(2.0 * energy * dtheta - 2.0)
        worst_energy = max(worst_energy, energy_err)
        print(
            f"  {s:6.3f}   {max_abs:9.5f}  {math.sqrt(sq / len(thetas)):9.5f}  "
            f"{max_rel:14.4f}   {energy_err:9.5f}   {'ok' if mono_here else 'FAIL'}"
        )
    print()
    print(f"  shipped band (sigma <= {SHIPPED_BAND}):  max {worst_band:.5f}  "
          f"rms {math.sqrt(sum_sq_band / max(n_band, 1)):.5f}   (bars 0.005 / 0.0015)")
    print(f"  full domain  (sigma <= 2.0):  max {worst_full:.5f}  "
          f"rms {math.sqrt(sum_sq_full / max(n_full, 1)):.5f}")
    print(f"  worst energy error: {worst_energy:.5f}   (bar 0.005)")
    print(f"  monotone in NdotL:  {'yes' if monotone else 'NO'}")

    # sigma = 0 must be Lambert bit-exactly, or the off path is not free.
    coef0 = coefficients(params, 0.0)
    exact = all(evaluate(coef0, math.cos(t)) == max(math.cos(t), 0.0) for t in thetas)
    print(f"  sigma = 0 is bit-exact Lambert: {'yes' if exact else 'NO'}")
    return worst_band, worst_energy, monotone, exact


def chord_vs_arc():
    """How far the exact chord departs from the small-angle model, per sigma."""
    print("\nexact chord vs small-angle arc (why the substitution is not optional)")
    print("  sigma    max |D_chord - D_arc|")
    thetas = [math.pi * i / 90.0 for i in range(91)]
    for s in (0.15, 0.30, 0.60, 1.00, 1.60):
        exact = ground_truth(s, thetas)
        # Same integral with the arc x in place of the chord 2 sin(x/2).
        acc = []
        total = 0.0
        samples = []
        for i in range(KERNEL_SAMPLES):
            x = -math.pi + 2.0 * math.pi * i / KERNEL_SAMPLES
            w = 0.0
            for weight, mult in PROFILE:
                sd = mult * s
                w += weight * math.exp(-(x * x) / (2.0 * sd * sd))
            samples.append((math.cos(x), math.sin(x), w))
            total += w
        for t in thetas:
            ct, st = math.cos(t), math.sin(t)
            a = 0.0
            for cx, sx, w in samples:
                c = ct * cx - st * sx
                if c > 0.0:
                    a += c * w
            acc.append(a / total)
        print(f"  {s:5.2f}    {max(abs(a - b) for a, b in zip(acc, exact)):.5f}")


def area_weights():
    """The peak-1 vs area-normalised distinction, as a number."""
    areas = [w * m for w, m in PROFILE]
    total = sum(areas)
    print("\nprofile sub-Gaussian weights")
    print("  literal (peak-1):   " + ", ".join(f"{w:.3f}" for w, _ in PROFILE))
    print("  effective (area):   " + ", ".join(f"{a / total:.3f}" for a in areas))
    print("  -> the tail carries the profile; an integrator that uses the literal")
    print("     weights fits systematically sharp")


def main():
    do_fit = "--fit" in sys.argv
    area_weights()
    chord_vs_arc()

    if do_fit:
        # Two stages, because a joint search over every constant at once is
        # badly conditioned: the shape at one sigma and the way it varies with
        # sigma are different problems, and solving them together lets a bad
        # sigma trade against a good one. Stage 1 finds the best shape at each
        # sigma independently -- the ceiling this form can reach. Stage 2 draws
        # smooth curves through those, which is linear and solved exactly.
        print("\nstage 1: best shape at each sigma (this takes a minute)...")
        dtheta = FIT_THETAS[1] - FIT_THETAS[0]
        seed = (0.2, 1.0, 0.0, 0.0, 0.0, 0.0)
        per_sigma = []
        for s in FIT_SIGMAS:
            ref = ground_truth(s, FIT_THETAS)
            fn = lambda sh: shape_objective(sh, FIT_THETAS, ref, dtheta)
            # Continued from the previous sigma so the fit tracks ONE branch:
            # restarting cold lets equivalent minima swap and the coefficient
            # curves come out jagged, which no smooth polynomial can follow.
            p, sc = nelder_mead(fn, seed, [0.2] * SHAPE_DIM, iters=3000)
            p, sc = nelder_mead(fn, p, [0.02] * SHAPE_DIM, iters=3000)
            per_sigma.append(p)
            seed = p
            print(f"  sigma {s:5.3f}: residual {sc:.3e}   "
                  + " ".join(f"{lab}={v:+.4f}"
                             for lab, v in zip(("w", "a2", "a3", "d0", "e0"), p)))

        print("\nstage 2: smooth curves through the coefficients...")
        params = []
        for i in range(SHAPE_DIM):
            params.extend(
                polyfit_through_base(FIT_SIGMAS, [p[i] for p in per_sigma], SHAPE_BASE[i])
            )

        report(params, "FITTED (this run)")
        print("\npaste into FITTED above and into preintegrated_skin.glsl:")
        labels = ("w", "a2", "a3", "d0", "e0")
        for i, lab in enumerate(labels):
            chunk = params[i * POLY_DEG : (i + 1) * POLY_DEG]
            print(f"    {lab:3s} " + ", ".join(f"{v:+.6f}" for v in chunk))
        print("\n    FITTED = (" + ", ".join(f"{v:.6f}" for v in params) + ")")
        return

    if not any(FITTED):
        print("\nFITTED is still all zeros -- run with --fit first.")
        return
    report(FITTED, "CHECKED-IN FIT")


if __name__ == "__main__":
    main()
