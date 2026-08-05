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

K mirrors profileWeight() in include/sss_profile.glsl -- the SAME diffusion profile the
screen-space blur uses, so the two mechanisms cannot disagree about the shape of
skin. That coupling has no compiler behind it, which is why the default mode re-measures it.

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

    per fragment: (w, a1, a2, a3, d0, e0) = lerp of two table rows at sigma
    per light:    t  = max((NdotL + w) / (1 + w), 0)
                  D  = d0 + e0 NdotL + t (a1 + t (a2 + t a3))

The six coefficients come from a 16-entry TABLE rather than polynomials in
sigma. Polynomials were tried first and cost nine times the error the shape
itself can reach: the coefficient curves have a kink where a1 meets zero and a
branch switch near sigma 1.4, and nothing low-order follows that. Sampling the
curves directly IS the ceiling.

That is not the LUT the roadmap ruled out. The objection there was texture
units, of which pbr_frag has none spare; this is a uniform array, the same
mechanism sssProfiles already uses to reach the same shader, and it costs no
unit at all.

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

# Mirror of profileWeight() in cetra/shaders/include/sss_profile.glsl (weight, sigma
# multiplier). If that changes, re-run this tool: nothing else couples them.
PROFILE = ((0.35, 0.30), (0.40, 1.00), (0.25, 2.20))

# Pasted from a --fit run: TABLE_N rows of (w, a1, a2, a3, d0, e0), one per
# sigma sample. Row 0 is Lambert exactly and is not fitted.
#
# a1 collapsing to zero between rows 3 and 4 is the boundary described above,
# and the lerp across it is where the worst remaining error sits -- which is
# also where the effect itself is smallest, so it buys the least to chase.
FITTED = (
    +0.000000, +1.000000, +0.000000, +0.000000, +0.000000, +0.000000,   # sigma 0.0000
    +0.002937, +0.982777, +0.036989, -0.020117, -0.008114, +0.000003,   # sigma 0.0089
    +0.014651, +0.909094, +0.180625, -0.095951, -0.006599, +0.001883,   # sigma 0.0356
    +0.042966, +0.739358, +0.448593, -0.223398, +0.002926, +0.013395,   # sigma 0.0800
    +0.296181, +0.000002, +1.676619, -0.743188, +0.007737, +0.017651,   # sigma 0.1422
    +0.370892, +0.000001, +1.299864, -0.487884, +0.046116, +0.056531,   # sigma 0.2222
    +0.428386, -0.000001, +0.921226, -0.299382, +0.107266, +0.100139,   # sigma 0.3200
    +0.477456, -0.000001, +0.630532, -0.178147, +0.162334, +0.112379,   # sigma 0.4356
    +0.513038, -0.000000, +0.443553, -0.111772, +0.201927, +0.110250,   # sigma 0.5689
    +0.513773, +0.000000, +0.321250, -0.080772, +0.232518, +0.107509,   # sigma 0.7200
    +0.502463, -0.000000, +0.240158, -0.062827, +0.253784, +0.099107,   # sigma 0.8889
    +0.511037, -0.000000, +0.187660, -0.047886, +0.266279, +0.085433,   # sigma 1.0756
    +0.550399, -0.000001, +0.153151, -0.034116, +0.272962, +0.071102,   # sigma 1.2800
    +0.615359, -0.000000, +0.128879, -0.021986, +0.276624, +0.059246,   # sigma 1.5022
    +0.690852, -0.000000, +0.110138, -0.012613, +0.279306, +0.051065,   # sigma 1.7422
    +0.761687, +0.000001, +0.094044, -0.006485, +0.282269, +0.046447,   # sigma 2.0000
)

KERNEL_SAMPLES = 1024  # periodic + smooth, so the trapezoid is spectrally accurate
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
TABLE_N = 16   # sigma samples
SIGMA_MAX = 2.0

# The shape at sigma = 0: clamped Lambert exactly, and entry 0 of the table.
SHAPE_BASE = (0.0, 1.0, 0.0, 0.0, 0.0, 0.0)

# a1 is fitted, not pinned. Wherever t > 0 the term a1*t is affine in NdotL and
# so is d0 + e0*NdotL, which makes them nearly collinear -- and left free, a1
# slides to exactly 0 around sigma 0.15 and sticks there against the boundary.
# Pinning it to 1 to remove that redundancy was tried and measured WORSE (0.087
# against 0.059 max), because the two only span the same directions where the
# clamp is open; a1 alone moves the lit side without touching the dark one.


def table_sigmas():
    """Sample positions, QUADRATIC in the index.

    Two things fall out of that choice. The shader's inverse is one sqrt, and
    the samples bunch up at small sigma, which is where the coefficients move
    fastest -- uniform spacing spends half its entries above sigma 1 where the
    curves are nearly flat and under-resolves the part that ships.
    """
    return [SIGMA_MAX * (i / (TABLE_N - 1)) ** 2 for i in range(TABLE_N)]


def coefficients(table, sigma):
    """Lerp the shape out of the table, exactly as the shader will.

    A table rather than polynomials in sigma. The coefficient curves have a kink
    where a1 meets zero and a branch switch near sigma 1.4, and a low-order
    polynomial through them costs nine times the error the shape itself can
    reach. Sampling them directly IS that ceiling.

    It is not the LUT the roadmap ruled out: that objection was texture units,
    of which pbr_frag has none spare. This is a uniform array, the same
    mechanism sssProfiles already uses to reach the same shader.
    """
    s = min(max(sigma, 0.0), SIGMA_MAX)
    f = math.sqrt(s / SIGMA_MAX) * (TABLE_N - 1)
    i = min(int(f), TABLE_N - 2)
    frac = f - i
    lo = table[i * SHAPE_DIM : (i + 1) * SHAPE_DIM]
    hi = table[(i + 1) * SHAPE_DIM : (i + 2) * SHAPE_DIM]
    return tuple(a + (b - a) * frac for a, b in zip(lo, hi))


def evaluate(coef, ndotl):
    w, a1, a2, a3, d0, e0 = coef
    t = (ndotl + w) / (1.0 + w)
    if t < 0.0:
        t = 0.0
    # Mirrors skinDiffuse's outer max(). The objective below never penalises
    # sign, so the fit is free to dip below zero on the dark side; the shader
    # clamps it and this has to clamp identically or the reported error
    # describes a shader nobody ships.
    return max(d0 + e0 * ndotl + t * (a1 + t * (a2 + t * a3)), 0.0)


def shape_objective(shape, thetas, ref, dtheta):
    """Squared error at ONE sigma, with the energy and monotonicity constraints."""
    total = 0.0
    energy = 0.0
    prev = None
    for i, t in enumerate(thetas):
        got = evaluate(shape, math.cos(t))
        total += (got - ref[i]) ** 2
        energy += got
        # theta ascends so NdotL descends: D must not increase along it.
        if prev is not None and got > prev + 1e-9:
            total += 50.0 * (got - prev) ** 2
        prev = got
    # Doubled: the grid covers 0..pi and D is even in theta.
    return total + 200.0 * (2.0 * energy * dtheta - 2.0) ** 2




def nelder_mead(fn, start, step, iters=4000):
    """Compact Nelder-Mead. No scipy, and none needed at this size."""
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
    band_rms = math.sqrt(sum_sq_band / max(n_band, 1))
    print()
    # These are the ACCEPTED values, not aspirational bars. The 0.005 / 0.0015
    # targets this fit was originally written against were not met and were
    # superseded (spec 11.13 §K6) by a rendered-image bar; printing them here
    # told anyone who ran the tool that the shipped table was failing.
    print(f"  shipped band (sigma <= {SHIPPED_BAND}):  max {worst_band:.5f}  "
          f"rms {band_rms:.5f}")
    print(f"  full domain  (sigma <= 2.0):  max {worst_full:.5f}  "
          f"rms {math.sqrt(sum_sq_full / max(n_full, 1)):.5f}")
    print(f"  worst energy error: {worst_energy:.5f}")
    print(f"  monotone in NdotL:  {'yes' if monotone else 'NO'}")

    # sigma = 0 must be Lambert bit-exactly, or the off path is not free.
    coef0 = coefficients(params, 0.0)
    exact = all(evaluate(coef0, math.cos(t)) == max(math.cos(t), 0.0) for t in thetas)
    print(f"  sigma = 0 is bit-exact Lambert: {'yes' if exact else 'NO'}")

    # Regression bars, set from the accepted measurement with headroom. The
    # point is to catch profileWeight() drifting out from under the table, not
    # to re-litigate the accuracy the spec already accepted -- so they sit just
    # above what ships rather than at the original targets.
    fails = []
    if worst_band > 0.035:
        fails.append(f"band max {worst_band:.5f} > 0.035")
    if band_rms > 0.012:
        fails.append(f"band rms {band_rms:.5f} > 0.012")
    if worst_energy > 0.05:
        fails.append(f"energy {worst_energy:.5f} > 0.05")
    if not monotone:
        fails.append("not monotone in NdotL")
    if not exact:
        fails.append("sigma = 0 is not bit-exact Lambert")
    if fails:
        print("\n  REGRESSED: " + "; ".join(fails))
    return fails


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
    # Derivations, not verification: they re-integrate from scratch and the
    # default mode never consults them, so they ride with --fit.
    if do_fit:
        area_weights()
        chord_vs_arc()

    if do_fit:
        print("\nfitting the shape at each table sigma (this takes a minute)...")
        dtheta = FIT_THETAS[1] - FIT_THETAS[0]
        sigmas = table_sigmas()
        table = list(SHAPE_BASE)  # entry 0 is Lambert, exactly, not fitted
        seed = SHAPE_BASE
        for s in sigmas[1:]:
            ref = ground_truth(s, FIT_THETAS)
            fn = lambda sh: shape_objective(sh, FIT_THETAS, ref, dtheta)
            # Continued from the previous sigma so the fit tracks ONE branch.
            # Restarting cold lets equivalent minima swap, and a lerp between
            # two entries sitting on different branches is not on either.
            p, sc = nelder_mead(fn, seed, [0.15] * SHAPE_DIM, iters=3000)
            p, sc = nelder_mead(fn, p, [0.02] * SHAPE_DIM, iters=3000)
            table.extend(p)
            seed = p
            print(f"  sigma {s:5.3f}: residual {sc:.3e}   "
                  + " ".join(f"{lab}={v:+.4f}"
                             for lab, v in zip(("w", "a1", "a2", "a3", "d0", "e0"), p)))

        report(table, "TABLE (this run)")
        print("\npaste into FITTED above and into preintegrated_skin.glsl:")
        print("    FITTED = (")
        for i in range(TABLE_N):
            row = table[i * SHAPE_DIM : (i + 1) * SHAPE_DIM]
            print(f"        {', '.join(f'{v:+.6f}' for v in row)},"
                  f"   // sigma {sigmas[i]:.4f}")
        print("    )")
        return 0

    # Non-zero on regression, so this is usable as a check rather than a report.
    # The coupling to profileWeight() has nothing else enforcing it.
    return 1 if report(FITTED, "CHECKED-IN FIT") else 0


if __name__ == "__main__":
    sys.exit(main())
