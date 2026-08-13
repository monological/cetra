// The water surface, evaluated in ONE place.
//
// Both the position the vertex stage rasterizes and the normal the fragment
// stage shades with come from here, and that is the point: a normal derived from
// a different height than the raster used prints as shading that does not match
// the silhouette, and a shadow pass evaluating its own copy casts a shadow from a
// surface that is not where the surface is. wind.glsl makes the same argument for
// the same reason.
//
// Derivatives are ANALYTIC, never finite differences. The models this seam is
// built for both hand them over for free: a Gerstner term differentiates in
// closed form, and an FFT cascade transforms its derivative fields alongside its
// height in the same pass. A finite difference would need a second full
// evaluation per axis and would still be wrong at the crest, which is the one
// place the normal matters most.

uniform float waterLevel;
uniform vec2 waterWindDir;     // unit; the longest wave's travel direction
uniform float waterAmplitude;  // half crest-to-trough of the longest wave
uniform float waterWavelength; // longest wave, world units
uniform float waterSteepness;  // 0 = round sine crests, 1 = the sharpest legal
uniform float waterSpread;     // per-octave direction fan, radians

// Four octaves. Each is a quarter the length and a bit under half the height of
// the one before, which is the classic wave-train spacing: enough separation that
// the octaves do not beat against each other into a visible repeat.
const int OCEAN_WAVES = 4;
const float OCEAN_LENGTH_FALLOFF = 0.42;
const float OCEAN_AMPLITUDE_FALLOFF = 0.44;
const float OCEAN_GRAVITY = 9.81;

// Spectral cascades (waveModel 1). Sampled in the VERTEX stage as well as the
// fragment stage, which GL 3.3 allows and which is the whole point -- the long and
// medium bands displace real geometry.
uniform int waveModel; // 0 = Gerstner octaves, 1 = spectral cascades
uniform sampler2D cascade0_0;
uniform sampler2D cascade0_1;
uniform sampler2D cascade1_0;
uniform sampler2D cascade1_1;
uniform sampler2D cascade2_0;
uniform sampler2D cascade2_1;
uniform float cascadeLength[3];
uniform float cascadeChoppiness[3];
// Last frame's target 0 for the two cascades that displace, and whether they hold a
// frame yet. 0 on the first two frames, and then the previous position falls back to
// the current one -- which reports camera motion only, the behaviour the whole
// spectral path had before these existed.
uniform sampler2D cascadePrev0;
uniform sampler2D cascadePrev1;
uniform int prevAvailable;

// The baked bed, for shoaling: height in R, its world-space gradient in G,B. Absent
// (bedAvailable 0) is the normal case: the per-fragment water column comes from the
// resolved scene depth instead, which is exact and works against arbitrary geometry.
// This answers the one question screen depth cannot -- a vertex needs its own depth to
// know how far to move, and sampling screen depth would need the position that depth
// is meant to produce.
uniform sampler2D bedTex;
uniform int bedAvailable;
uniform float waterExtent;

/*
 * PROJECTED GRID placement (spec 11.34).
 *
 * The lattice is a fixed grid in NDC rather than in the world: each vertex is a ray
 * through its own screen position, and where that ray meets the still-water plane is the
 * point the surface gets evaluated at. So sample density is uniform in PIXELS and reach is
 * whatever the frustum sees -- the two properties a camera-centred mesh has to trade
 * against each other, because for it they are both the same number.
 *
 * The ray is built from the FORWARD projection, not an uploaded inverse: invFocal undoes
 * the two focal scales and mat3(transpose(view)) is the view rotation's inverse while the
 * view is orthonormal, which is the idiom the rest of this tree reconstructs rays with.
 */

// The lattice's horizon row is placed exactly ON the horizon, where the ray is parallel to
// the plane and there is no intersection at all -- so "infinity" has to be a distance.
// Large enough that the row lands within a fraction of a pixel of the true vanishing line:
// the miss angle is atan(eyeHeight / this), 1e-4 rad at a 100-unit eye height.
const float OCEAN_FAR_DIST = 1.0e6;

// Rays are cast slightly outside the frustum. The horizontal half of the displacement
// moves a vertex ACROSS the screen, so an edge vertex can be pulled inward and leave a
// wedge of background along the frame border. Not applied to the horizon edge, which is
// not a screen edge -- there is no water beyond it to be pulled in from.
const float OCEAN_OVERSCAN = 1.12;

// A camera exactly in the plane projects the whole plane onto the horizon line, so every
// ray meets it at distance zero and the lattice collapses to a single point. Exactly right
// for a FLAT plane and useless for a displaced one, so the eye is held a hair off the
// surface rather than in it.
const float OCEAN_MIN_EYE_HEIGHT = 0.05;

vec2 oceanProjectedPosition(vec2 lattice, mat4 view, mat4 projection, vec3 camPos) {
    vec2 invFocal = 1.0 / vec2(projection[0][0], projection[1][1]);
    mat3 viewToWorld = mat3(transpose(view));

    float ndcX = lattice.x * 2.0 * OCEAN_OVERSCAN;
    bool above = camPos.y >= waterLevel;

    /*
     * Which NDC row this column's horizon sits on.
     *
     * A ray's world-Y slope is ndcX*invFocal.x*a + ndcY*invFocal.y*b - c over the world-Y
     * components of the three view basis vectors; setting that to zero and solving for
     * ndcY gives the row running parallel to the plane. Per COLUMN, because a rolled
     * camera's horizon is not a horizontal line on screen.
     *
     * Solving for it is what makes the lattice worth its vertices. Rows past the horizon
     * see no water at all, so without this the usable fraction of the lattice is whatever
     * the pitch happens to leave -- and half is a common answer.
     */
    float a = viewToWorld[0].y;
    float b = viewToWorld[1].y;
    float c = viewToWorld[2].y;
    float denom = invFocal.y * b;
    // b is the world-Y of the view's up axis, so it vanishes looking straight down or up,
    // and then no row is parallel to the plane and the whole lattice is usable. Off the
    // top of the screen when the eye is above the surface, off the bottom when below.
    float horizonY = abs(denom) > 1e-6 ? (c - ndcX * invFocal.x * a) / denom
                                       : (above ? 1.0e30 : -1.0e30);

    // Above the surface the water is the region BELOW the horizon row; below it, the
    // region above. Either way the lattice spans exactly that region and nothing else.
    float y0 = above ? -OCEAN_OVERSCAN : max(horizonY, -OCEAN_OVERSCAN);
    float y1 = above ? min(horizonY, OCEAN_OVERSCAN) : OCEAN_OVERSCAN;
    float ndcY = mix(y0, y1, lattice.y + 0.5);

    vec3 rd = normalize(viewToWorld * vec3(vec2(ndcX, ndcY) * invFocal, -1.0));
    float eye = camPos.y - waterLevel;
    eye = above ? max(eye, OCEAN_MIN_EYE_HEIGHT) : min(eye, -OCEAN_MIN_EYE_HEIGHT);
    float t = -eye / rd.y;
    // A ray running away from the plane, or grazing it, runs to the cap instead. Written as
    // a positive range rather than a negation so that a division by zero -- inf, or NaN
    // when the numerator vanishes too -- takes the same branch.
    t = (t > 0.0 && t < OCEAN_FAR_DIST) ? t : OCEAN_FAR_DIST;
    return camPos.xz + rd.xz * t;
}

// Depth over which a wave goes from fully shoaled to fully open-water. Waves
// shorten and steepen as the bed rises under them; below the floor there is not
// enough water column left to carry any displacement at all.
const float OCEAN_SHOAL_MIN = 0.14;
const float OCEAN_SHOAL_FULL = 2.7;

/*
 * 1 in open water, falling to 0 as the bed comes up, with its own gradient.
 *
 * Multiplies displacement, not height alone: the horizontal term has to shrink with it
 * or the surface slides sideways over a beach it is no longer above. And because it
 * multiplies the displacement, its GRADIENT is a product-rule term in every surface
 * derivative -- the factor alone describes a surface whose normal is the open-water
 * one wherever the bed is steep, which is the surf zone specifically. Zero over a flat
 * bed and zero with no bed at all, so the term costs nothing where it says nothing.
 *
 * .x is the factor; .yz is d(factor)/d(world x, world z).
 */
vec3 oceanShoal(vec2 p) {
    if (bedAvailable == 0)
        return vec3(1.0, 0.0, 0.0);
    vec2 uv = p / (waterExtent * 2.0) + 0.5;
    vec3 bed = texture(bedTex, clamp(uv, vec2(0.0), vec2(1.0))).rgb;
    float column = waterLevel - bed.r;
    float span = OCEAN_SHOAL_FULL - OCEAN_SHOAL_MIN;
    float u = clamp((column - OCEAN_SHOAL_MIN) / span, 0.0, 1.0);
    // d/dp smoothstep(u(p)) = 6u(1-u) * du/dp, and du/dp = -d(bed)/dp / span. Flat
    // outside the window, where smoothstep has clamped and the bed can move without
    // the factor moving.
    float dFactor = 6.0 * u * (1.0 - u) / span;
    return vec3(u * u * (3.0 - 2.0 * u), -dFactor * bed.g, -dFactor * bed.b);
}

struct OceanSurface {
    vec3 world;  // displaced world position
    vec3 normal; // unit normal from the analytic derivatives
    float shoal; // 1 = open water, 0 = the bed has reached the surface
    // Horizontal-map Jacobian; below 1 the map is compressing. 1 means "not
    // computed", which is the Gerstner path.
    float jacobian;
};

/*
 * Spectral surface: the long and medium bands displace the mesh, the short band
 * is left for the fragment stage's slope distribution.
 *
 * Every derivative comes out of the same transform as the height -- field0.a is a
 * cross derivative, field1.rg the two slopes, field1.ba the two horizontal
 * derivatives -- so the normal below is analytic and costs nothing extra. That
 * packing is the reason to spend two RGBA targets per cascade.
 */
// A linear random sea is vertically symmetric, and a real one is not: crests are
// sharper than troughs are deep. These are the low-order bound-harmonic correction
// for that, kept small deliberately -- pushed harder the surface folds, which is what
// the choppy-wave literature bounds. The subtracted constants are each band's mean
// square, so the correction reshapes the surface without raising its mean level.
const float OCEAN_BOUND_LONG = 0.14;
const float OCEAN_BOUND_MED = 0.32;
const float OCEAN_BOUND_LONG_VAR = 0.080;
const float OCEAN_BOUND_MED_VAR = 0.030;

// One band's tiling lookup. Written out eleven times before this existed, twice per band
// for the two targets of a single sample point.
vec2 oceanCascadeUv(vec2 p, int band) {
    return fract(p / cascadeLength[band] + 0.5);
}

/*
 * One band's horizontal-map Jacobian, from its packed derivatives. Below 1 the map is
 * compressing, which is what selects whitecaps and caustic focus.
 *
 * NO shoal term, deliberately: this is asked about the bands that shade the interface and
 * never displace the mesh, so there is no shoal factor scaling them and the two
 * off-diagonals really are equal. That is why this is a square where oceanAssemble's
 * determinant is not -- the distinction is the shoal gradient, and it belongs here in
 * writing because three hand-written copies of this shape invited "unifying" them the
 * wrong way.
 */
float oceanBandJacobian(vec4 field0, vec4 field1, float choppiness) {
    float c = field0.a * choppiness;
    vec2 d = field1.ba * choppiness;
    return (1.0 + d.x) * (1.0 + d.y) - c * c;
}

/*
 * The UNSHOALED displacement of the two bands that reach the mesh: horizontal in .xz,
 * height in .y, crest term included.
 *
 * The one place the height model is written. The previous frame's position comes from
 * retained copies of exactly these two samples and has to be the same arithmetic, and the
 * derivative rows below multiply the same value -- so a second copy is a second place for
 * the bound-harmonic term to drift, which is the failure this file exists to prevent.
 */
vec3 oceanSpectralDisplacement(vec4 long0, vec4 med0) {
    vec2 h = long0.rg * cascadeChoppiness[0] + med0.rg * cascadeChoppiness[1];
    float y = long0.b + med0.b +
              OCEAN_BOUND_LONG * (long0.b * long0.b - OCEAN_BOUND_LONG_VAR) +
              OCEAN_BOUND_MED * (med0.b * med0.b - OCEAN_BOUND_MED_VAR);
    return vec3(h.x, y, h.y);
}

/*
 * Both wave models end here.
 *
 * disp is the unshoaled displacement; dispDx/dispDz are its derivatives with respect to
 * the undisplaced grid coordinates. The shoal factor scales the displacement and its
 * GRADIENT therefore enters as a product-rule term -- over a flat bed sh.yz is zero and
 * these reduce to the plain scaled derivatives.
 *
 * Shared rather than written per model because the two epilogues were the same expression
 * character for character: the identity rows, the product rule, the cross order and the
 * determinant each existed twice, and the Jacobian in particular is easy to "simplify"
 * back into a square, which is only correct while the gradient is zero.
 */
OceanSurface oceanAssemble(vec2 p, vec3 disp, vec3 dispDx, vec3 dispDz, vec3 sh) {
    float shoal = sh.x;
    OceanSurface s;
    s.world = vec3(p.x, waterLevel, p.y) + disp * shoal;
    vec3 dPdx = vec3(1.0, 0.0, 0.0) + dispDx * shoal + disp * sh.y;
    vec3 dPdz = vec3(0.0, 0.0, 1.0) + dispDz * shoal + disp * sh.z;
    // cross(dPdz, dPdx) and not the reverse: on a flat surface that is +Y, and the
    // flipped order would light every wave from underneath.
    s.normal = normalize(cross(dPdz, dPdx));
    // The horizontal map's determinant, from those same rows. The two off-diagonals are
    // NOT equal once the shoal gradient is in, so this cannot be shortened back to a
    // square -- and the difference between them IS the shoaling compression that selects
    // surf-zone foam.
    s.jacobian = dPdx.x * dPdz.z - dPdz.x * dPdx.z;
    s.shoal = shoal;
    return s;
}

vec3 oceanSpectralPosition(vec2 p, vec4 long0, vec4 med0, float shoal) {
    vec3 d = oceanSpectralDisplacement(long0, med0);
    return vec3(p.x, waterLevel, p.y) + d * shoal;
}

/*
 * Spectral surface. No amplitude knob, deliberately: the spectrum is already physical --
 * its height comes out of the wind speed and fetch it was seeded with -- so scaling it
 * would be authoring over a sea state rather than choosing one. waterAmplitude belongs to
 * the Gerstner path, where there is no sea state to ask. A calmer spectral ocean is a
 * lower wind speed, not a smaller number here.
 */
OceanSurface oceanEvaluateSpectral(vec2 p, vec3 sh) {
    vec2 uvLong = oceanCascadeUv(p, 0);
    vec2 uvMed = oceanCascadeUv(p, 1);
    vec4 long0 = texture(cascade0_0, uvLong);
    vec4 long1 = texture(cascade0_1, uvLong);
    vec4 med0 = texture(cascade1_0, uvMed);
    vec4 med1 = texture(cascade1_1, uvMed);

    float q0 = cascadeChoppiness[0];
    float q1 = cascadeChoppiness[1];

    // Not named `cross`: that is a builtin oceanAssemble calls.
    float crossDeriv = long0.a * q0 + med0.a * q1;
    // The crest-sharpening term changes the height, so its derivative has to be in the
    // slope or the normal is the normal of a DIFFERENT surface than the one rasterized.
    // d/dp of b*(h*h - c) is 2*b*h*dh, per band.
    vec2 slope = long1.rg * (1.0 + 2.0 * OCEAN_BOUND_LONG * long0.b) +
                 med1.rg * (1.0 + 2.0 * OCEAN_BOUND_MED * med0.b);
    vec2 dHoriz = long1.ba * q0 + med1.ba * q1;

    return oceanAssemble(p, oceanSpectralDisplacement(long0, med0),
                         vec3(dHoriz.x, slope.x, crossDeriv),
                         vec3(crossDeriv, slope.y, dHoriz.y), sh);
}

/*
 * Sum of Gerstner waves.
 *
 * A Gerstner wave moves its surface points horizontally as well as vertically,
 * bunching them toward the crest, which is what sharpens a crest and broadens a
 * trough instead of leaving a symmetric sine. The horizontal term is what makes
 * it a Gerstner wave rather than a height field, and it is also what can fold the
 * surface over itself: the mapping stops being injective once Q*A*k exceeds 1.
 *
 * So steepness is NOT passed through to Q. It is divided by the octave's own A*k
 * and by the octave count, so the set sums to the authored value: at or below 1
 * the map stays injective at any amplitude or wavelength. Above 1 it folds, which
 * is why the C side clamps the uniform rather than trusting the caller. Authoring
 * Q directly would instead make "sharp" mean something different at every
 * wavelength.
 *
 * `t` is used here and ignored by the spectral path, which reads cascades that
 * already hold one instant -- see the previous-position note in water_vert.
 */
OceanSurface oceanEvaluateAt(vec2 p, float t, vec3 sh) {
    if (waveModel == 1)
        return oceanEvaluateSpectral(p, sh);

    // Accumulated UNSHOALED, then scaled once at the end by oceanAssemble. The shoal
    // factor multiplies the whole displacement, so its gradient multiplies the whole
    // displacement too -- which is one line out there and would be six inside the loop.
    vec3 disp = vec3(0.0);
    // Partial derivatives of the displacement with respect to the undisplaced grid
    // coordinates. The flat plane's own identity rows are added at the end, since they
    // are not part of what shoals.
    vec3 dDispDx = vec3(0.0);
    vec3 dDispDz = vec3(0.0);

    float wavelength = waterWavelength;
    // Note what the shoal factor must NOT be folded into: q is amplitude's reciprocal,
    // so scaling amplitude here leaves qa = steepness/(k*N) unchanged and the lateral
    // shuffle would run at full strength over a beach whose vertical motion had
    // already faded to nothing.
    float amplitude = waterAmplitude;
    vec2 base = normalize(waterWindDir + vec2(1e-6, 0.0));

    for (int i = 0; i < OCEAN_WAVES; i++) {
        // Fan the octaves off the wind, alternating sides so the set stays
        // centred on it. A single direction reads as corduroy however many
        // octaves are stacked on it.
        float fan = waterSpread * float(i) * ((i % 2 == 0) ? 1.0 : -1.0);
        vec2 dir = vec2(base.x * cos(fan) - base.y * sin(fan),
                        base.x * sin(fan) + base.y * cos(fan));

        float k = 6.28318530718 / max(wavelength, 0.01);
        // Deep-water dispersion: long waves travel faster, which is what keeps
        // the octaves sliding past each other instead of marching in lockstep.
        float omega = sqrt(OCEAN_GRAVITY * k);
        float phase = k * dot(dir, p) - omega * t;
        float sinp = sin(phase);
        float cosp = cos(phase);

        // Q normalised by this octave's own A*k and by the octave count, so the
        // steepness uniform sums to itself across the set: at or below 1 the map
        // stays injective. See the header note.
        float q = waterSteepness / max(k * amplitude * float(OCEAN_WAVES), 1e-4);
        float qa = q * amplitude;

        disp.x += qa * dir.x * cosp;
        disp.y += amplitude * sinp;
        disp.z += qa * dir.y * cosp;

        float dqa = qa * k * sinp;
        float dah = amplitude * k * cosp;
        dDispDx.x -= dqa * dir.x * dir.x;
        dDispDx.y += dah * dir.x;
        dDispDx.z -= dqa * dir.y * dir.x;
        dDispDz.x -= dqa * dir.x * dir.y;
        dDispDz.y += dah * dir.y;
        dDispDz.z -= dqa * dir.y * dir.y;

        wavelength *= OCEAN_LENGTH_FALLOFF;
        amplitude *= OCEAN_AMPLITUDE_FALLOFF;
    }

    OceanSurface s = oceanAssemble(p, disp, dDispDx, dDispDz, sh);
    // A Gerstner map DOES compress -- that bunching is what sharpens its crests -- but
    // its Jacobian is not reported here, so the determinant oceanAssemble computed is
    // overwritten. 1 means "no selector on this path", which is what the foam and
    // caustics gates read it as; reporting a real one would turn the clamped-steepness
    // argument for why they are FFT-only into a lie.
    s.jacobian = 1.0;
    return s;
}

// The shoal factor is a function of position alone, so a caller with several questions
// about one point should fetch it once and use oceanEvaluateAt. This is the convenience
// form for callers with only one.
OceanSurface oceanEvaluate(vec2 p, float t) {
    return oceanEvaluateAt(p, t, oceanShoal(p));
}

/*
 * Where this point was one frame ago, which is a motion vector's other half.
 *
 * Gerstner re-evaluates at t - dt and gets it in closed form. The spectral path
 * cannot: its cascades hold ONE instant, the current one, so reading them at t - dt
 * returns the same surface and the waves report no motion of their own. That is what
 * the retained copies answer -- the same position arithmetic against last frame's
 * fields.
 *
 * Only the two displacing bands are retained, which is all this needs: the short band
 * never reaches the mesh, so it has no position to have moved.
 */
vec3 oceanPreviousWorldAt(vec2 p, float tPrev, vec3 sh) {
    if (waveModel == 0)
        return oceanEvaluateAt(p, tPrev, sh).world;
    vec2 uvLong = oceanCascadeUv(p, 0);
    vec2 uvMed = oceanCascadeUv(p, 1);
    // Fetched inside the branches, not before them: reading the current fields and then
    // overwriting from the retained ones spent two texture fetches on every vertex from
    // frame two onward, which is essentially always. A ternary on the samplers themselves
    // is not the alternative -- GLSL 3.30 samplers are opaque and may not appear in an
    // expression -- so this is two branches.
    vec4 long0, med0;
    if (prevAvailable == 1) {
        long0 = texture(cascadePrev0, uvLong);
        med0 = texture(cascadePrev1, uvMed);
    } else {
        long0 = texture(cascade0_0, uvLong);
        med0 = texture(cascade1_0, uvMed);
    }
    return oceanSpectralPosition(p, long0, med0, sh.x);
}

vec3 oceanPreviousWorld(vec2 p, float tPrev) {
    return oceanPreviousWorldAt(p, tPrev, oceanShoal(p));
}
