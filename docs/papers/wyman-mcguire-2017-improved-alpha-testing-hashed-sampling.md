IEEE TRANSACTIONS ON VISUALIZATION AND COMPUTER GRAPHICS, VOL. XX, NO. X, XXXX 2017

## Improved Alpha Testing Using Hashed Sampling

### Chris Wyman, Member, IEEE,

### and Morgan McGuire

**Abstract**— We further describe and analyze the idea of *hashed alpha testing* from Wyman and McGuire [1], which builds on stochastic alpha testing and simplifies stochastic transparency. Typically, *alpha testing* provides a simple mechanism to mask out complex silhouettes using simple proxy geometry with applied alpha textures. While widely used, alpha testing has a long-standing problem: geometry can disappear entirely as alpha mapped polygons recede with distance. As foveated rendering for virtual reality spreads, this problem worsens as peripheral minification and prefiltering introduce this problem on *nearby* objects.

We first introduce the notion of *stochastic alpha testing*, which replaces a fixed alpha threshold of = 0*:* 5 with a randomly chosen *2* [0*::* 1). This entirely avoids the problem of disappearing alpha-tested geometry, but introduces temporal noise.

*Hashed alpha testing* uses a hash function to choose procedurally. With a good hash function and inputs, hashed alpha testing maintains distant geometry without introducing more temporal flicker than traditional alpha testing. We also describe how hashed alpha interacts with temporal antialiasing and applies to alpha-to-coverage and screen-door transparency. Because hashed alpha testing addresses alpha test aliasing by introducing stable sampling, it has implications in other domains where increased sample stability is desirable. We show how our hashed sampling might apply to other stochastic effects.

**Index Terms**—anisotropy, alpha map, alpha test, hash, hashed alpha test, stable shading, stochastic sampling.

F

### 1 INTRODUCTION

OR decades interactive renderers have used *alpha test-*

# Fing, discarding fragments whose alpha falls below a

specified threshold. While not suitable for transparent surfaces, which require alpha compositing [2] and order- independent transparency [3], alpha testing provides a cheap way to render binary visibility stored in an alpha map. Alpha testing is particularly common in engines using deferred rendering [4] or complex post-processing, as it pro- vides a correct and consistent depth buffer for subsequent passes. Today, games widely alpha test for foliage, fences, decals, and other small-scale details. But alpha testing introduces artifacts. Its binary queries alias on alpha boundaries, where. Because this occurs in texture space, geometric antialiasing is ineffective and only postprocess antialiasing addresses these artifacts (e.g., Karis [5], Lottes [6], and Yang et al. [7]). Texture prefiltering fails since a post-filter alpha test still gives binary results. Less well-known, alpha mapped geometry can disap- pear in the distance, as shown in Figure 1. Largely ignored in academic contexts, game developers frequently encounter this problem (e.g., Castano [8]). Some scene-specific tuning and restrictions on content creation reduce the problem, but none completely solve it. To solve both problems, we propose replacing the fixed alpha threshold,, with a stochastic threshold chosen uniformly in [0*::* 1), i.e., we replace: **if** ( color.a *<* ) **discard;**

### with a stochastic test:

*C. Wyman is with NVIDIA Corporation in Redmond, WA.* *E-mail: chris.wyman@acm.org*
*M. McGuire is with NVIDIA Corporation and Williams College.*
*Manuscript received ???; revised ???.*

**if** ( color.a *< drand48*() ) **discard;**

This adds high-frequency spatial and temporal noise, so we propose a hashed alpha test with similar properties but stable behavior, i.e., using: **if** ( color.a *< hash*( *:::*) ) **discard;**

Hashing techniques have many graphics applications, rang- ing from packing sparse data [9] to visualizing big data [10]. However, we present the first application of hash functions that enables controllable stability of stochastic samples. In particular, this paper makes the following contributions: we introduce the idea of *stochastic alpha testing*, which simplifies stochastic transparency [11] but maintains the expected value of geometric coverage at all view- ing distances; we show *hashed alpha testing* provides similar benefits and, with well-selected hash inputs, provides spatial and temporal stability comparable to traditional al- pha testing; we introduce an anisotropic variant of hashed alpha testing that retains these properties even for surfaces viewed at grazing angles; and we demonstrate how to robustly incorporate hashed alpha testing into modern rendering engines, includ- ing those using temporal antialiasing, multisample alpha-to-coverage, or screen-door transparency. We also provide insight into how our new stable, hash-based sampling scheme might apply in other stochastic sampling contexts where stable sampling may be desirable.

### WHY DOES GEOMETRY DISAPPEAR?

Disappearing alpha-tested geometry is poorly covered in academic literature. However, game developers repeatedly

(a) Traditonal alpha test (b) Alpha-to-coverage
(c) Hashed alpha test (d) Hashed alpha-to-coverage
Fig. 1: *Hashed alpha testing* avoids disappearing alpha-mapped geometry and reduces correlations in multisample alpha-to-

coverage. We compare a bearded man rendered with alpha testing, alpha-to-coverage, hashed alpha testing, and hashed alpha-to-coverage. Insets show the same geometry from further away, enlarged to better depict variations.

Fig. 2: Coarse texels average alpha over finer mipmapped

texels. The hair in Figure 1 has an average alphaavg= 0*:* 32, partly due to artist-added padding. Accessing coarse mip levels for alpha thresholding gives many samples close to avg, causing most fragments to fail the alpha test.

Fig. 3: When nearby, each metal wire in this fence is 5 pixels

wide (left). With distance, alpha testing discretely erodes pixel coverage along edges so the wire thins to roughly one pixel (center). Further away wires no longer cover even one pixel, disappearing entirely (right).

encounter this issue. Castano [8] investigates the causes and describes prior ad hoc solutions. Three issues contribute to loss of coverage in alpha-tested geometry: **Reduced alpha variance.** Mipmap construction itera- tively box filters the alpha channel, reducing alpha variance in coarser mipmap levels. At the coarsest level of detail (lod), a single texel averages alpha,avg, over the base texture. Many textures containavg, causing more failed alpha tests in coarser lods, especially if the texture contains padding (see Figure 2). Essentially, a decreasing percentage of texels pass the alpha test in coarser mips.

(a) (b) (c) (d)
Fig. 4: Hashed alpha testing (b) maintains coverage better

than regular alpha testing (a), but it still loses some coverage due to sub-pixel leaf billboards, compared to ground truth

(d). Using conservative raster with hashed alpha testing (c) ensures full coverage, but the alpha threshold must be modified based on sub-pixel coverage to avoid the over- sampled billboards shown here. **Discrete visibility test.** Unlike alpha blending, alpha testing gives binary visibility. Geometry is either visible or not in each pixel. As geometry approaches or recedes from the viewer, pixels suddenly transition between covered and not covered. This discretely erodes geometry with increas- ing distance (see Figure 3). At some point, 1-pixel wide geometry erodes to 0-pixel wide geometry and disappears. **Coarse raster grid.** With enough distance, even proxy billboard geometry becomes sub-pixel. In this case, a rela- tively coarse rasterization grid (i.e., sampling at pixel cen- ters) poorly captures the geometry. Some billboards may not appear, causing an apparent loss of coverage (see Figure 4). Our work does not address this issue, though conservative rasterization and multisampling reduce the problem.
### 3 STATE OF THE ART IN ALPHA TESTING

Game developers have dealt with disappearing alpha- mapped geometry for years, as games often display foliage, fences, and hair from afar. Various techniques can help manage the problem. **Adjusting per texture mipmap level.** Since mipmap- ping filters alpha, adjusting per mip-level can correct for

reduced variance. Consider a screen-aligned, alpha-tested billboard covering *A₀* pixels where *a₀* pixels pass the alpha test at mip level 0. Seen at a distance, this screen-aligned billboard might use mip level *i*. We expect a consistent percentage of pixels passing the alpha test, i.e.:

*a₀=A₀ ai=Ai:* (1)

One can precompute test thresholds (*i*) that maintain this ratio for each mipmap. But content affects this threshold, so it differs between textures and even within a mip level (i.e., we want (*i;u;v*)). Even perfect per-level thresholds do not prevent geometry disappearing with distance; the fence in Figure 3 has nearly constant (*i*) but still quickly disappears with distance. **Adjust stored values per mipmap level.** Castano [8] proposes a similar, mathematically equivalent, approach. Rather than requiring variable (*i*) that complicates shader code, the alpha values in each mipmap level are premulti- plied by 0*:* 5*=* (*i*) at asset creation time. The allows use of a fixed = 0*:* 5 at run time. Otherwise, this has identical properties to defining variable (*i*). **Adjust stored values per texel.** Castano [8] adjusts alpha for each mipmap level. Separate per-texel adjustments (e.g., by 0*:* 5*=* (*i;u;v*)) are possible, but it is unclear how to compute (*i;u;v*) or if independent texels adjustments still just dialate alpha boundaries (e.g., see Figure 11). **Always sampling from finest mip lod.** As filtering reduces alpha variance and changes threshold (*i*), always sampling from mip level 0 trivially avoids the problem. But this either requires two texel fetches per pixel (one for RGB, one for) or eliminates color prefiltering (using both RGB and from mip 0). An alternative limits the maximum mipmap lod to a user-specified *i*max, using level *i*maxwhen *i > i*max. But both approaches can thrash the texture cache and reduce the temporal stability of fine texture details. **Rendering first with-test, and then with-blend.** Alpha testing’s popularity stems from the difficulty of ef- ficient order-independent blending. Naive alpha blending causes halos where the z-buffer gets polluted by transpar- ent fragments. By first rendering with alpha testing and then rerendering with alpha blending, z-buffer pollution is reduced [12]. This guarantees transparent fragments never occlude opaque ones, but requires rendering alpha-mapped geometry twice and does not work with deferred shading. **Supersampling.** When storing binary visibility, a base texture’s alpha channel contains only 0s or 1s. With suf- ficiently dense supersampling, one can always access the full resolution texture, providing accurate visibility. But the required density can be arbitrarily high, due to geometric transformations and parameterizations. **Alpha-to-coverage.** With *n*-sample multisampling, alpha is discretized and outputs *bnc* dithered binary coverage samples [13]. Usually these dither patterns are fixed, causing correlations between layers and preventing correct multi- layer compositing. Enderton et al. [11] propose selecting random pattern permutations to address this problem. **Screen-door transparency.** While fairly uncommon to- day, screen-door transparency behaves similar to alpha-to- coverage except that dithering occurs over multiple pixels. Various mask selection techniques exist for screen-door transparency [14], but even random mask patterns lead to

(a) (b) (c) (d) (e) (f)
Fig. 5: A minified cedar tree using (a) alpha testing, stochas-

tic alpha testing with (b) 1, (c) 4, (d) 16, and (e) 64 samples per pixel, and (f) a supersampled ground truth using sorted alpha blending.

correlation between composited layers and repeated dither patterns visible over the screen.

### 4 STOCHASTIC ALPHA TESTING

To provide a mental framework to help understand hashed alpha testing, we first introduce the idea of *stochastic alpha* *testing*. Essentially, stochastic alpha testing replaces tradi- tional alpha testing’s fixed threshold ( = 0*:* 5) with a stochastic threshold ( = *drand*48()). This replaces one sample on a regular grid (i.e., sampling [0*::* 1) at 0.5) with one uniform random sample. A stochastic alpha test is a simplified form of stochastic transparency [11] with only one sample per pixel. While this seems trivial, we observe that replacing a fixed alpha thresh- old with a random one solves our disappearing coverage problem: alpha mapped surfaces no longer disappear with distance (see Figure 5). Unlike with a fixed alpha threshold, with stochastic sampling the visibility, *V* = (*<*) ? 0 : 1*;* has the correct expected value E[*V*] of. But a single random sample is insufficient. It introduces significant noise, causing continuous twinkle (see video). Reusing random seeds between frames and using stratifi- cation helps stabilize noise for static geometry (see Laine and Karras [15] and Wyman [16]). But whenever motion occurs the high-frequency noise reappears. Supersampling helps, but using just one sample per pixel is a major appeal of alpha testing, given it works in forward and deferred shading, without MSAA, and even on low-end hardware.

### 5 HASHED ALPHA TESTING

In *hashed alpha testing* we target visual quality equivalent to stochastic alpha testing with temporal stability equivalent to traditional alpha testing. Instead of stochastic sampling, we propose using a hash function to generate alpha thresholds. This is not particu- larly surprising, as hash functions have often formed the basis for common pseudo-random number generators [17]. Our contribution is recognizing that by careful manipulation of hash function inputs, rather than hiding them inside a pseudorandom number generator, we can better control the stability of our random sample.

Appropriate hash functions include those that generate outputs uniformly distributed in [0*::* 1), allowing direct sub- stitution for uniform random number generators. To obtain well distributed noise with spatial and temporal stability, we sought to control our hash function inputs to achieve the following properties:

noise anchored to surface, to avoid appearance of swimming; no sample correlations between overlapping alpha- mapped layers; and discretization of at roughly pixel scale, so subpixel translations return the same hashed value.

### 5.1 Hash Function

Our hash function is less important than its properties. We use the following hash function *f* : R²*!* [0*::* 1) from McGuire [18]: floathash( vec2 in) { returnfract( 1.0e4 * sin( 17.0*in.x + 0.1*in.y) * ( 0.1 + abs( sin( 13.0*in.y + in.x))) ); }

||Here,|scales|so that we discretize our hash|
|---|---|---|---|
|We tried other hash functions, which gave largely equivalent results. To hash 3D coordinates, a hash f : R³ ! [0::1) may|input (via the inputs within a pixel-sized region to return the same hashed|) at roughly pixel scale, allowing all||
|provide more control. We found repeatedly applying our 2D hash worked just as well:|value. User parameter scale in pixels (default 1:0). Changing||controls the target noise|

floathash3D( vec3 in) { returnhash( vec2( hash( in.xy), in.z)); }

### 5.2 Anchoring Hashed Noise to Geometry

To avoid noise swimming over surfaces, hash() inputs must stay fixed under camera and object motion. Candidates for stable inputs include those based on texture, world-space, and object-space coordinates. In scenes with a unique texture parameterization, texture coordinates work well. But many scenes lack such parame- terizations. Hashing world-space coordinates provides stable noise for static geometry, and our early tests used world-space co-

|// To discretize|noise|under z-translations:||
|---|---|---|---|
|floatpixDeriv|= floor(|max(length(dFdx(objCoord.xyz)), length(dFdy(objCoord.xyz)))|);|

ordinates. However, this fails on dynamic geometry. Object- space coordinates give stable hashes for skinned and rigid transforms and dynamic cameras. All our stability improvements disappear if coordinate frames become sub-pixel, as each pixel then uses different coordinates to compute threshold. It is vital to use coordinates consistent over an entire aggregate surface (e.g., a tree) rather than a part of the object (e.g., each leaf).

|of|, as below:|||||
|---|---|---|---|---|---|
|// Find|discretized|derivatives|of our|coordinates||
|floatmaxDeriv|= max(|length(dFdx(objCoord.xyz)), length(dFdy(objCoord.xyz))||);||
|vec2 pixDeriv|= vec2(|floor(maxDeriv),|ceil(maxDeriv)||);|
|// Two closest|noise|scales||||
|vec2 pixScales|= vec2(|1.0/(g_HashScale*pixDeriv.x), 1.0/(g_HashScale*pixDeriv.y)|||);|
|// Compute|alpha thresholds|at|our two noise|scales||
|vec2 alpha|= vec2(hash3D(floor(pixScales.x*objCoord.xyz)), hash3D(floor(pixScales.y*objCoord.xyz)));|||||
|// Factor|to interpolate|lerp|with|||
|floatlerpFactor|=|fract(maxDeriv|);|||

### 5.3 Avoiding Correlations Between Layers

For overlapping alpha-mapped surfaces, reusing thresh- olds between layers introduces undesirable correlations similar to those observed in hardware alpha-to-coverage. These correlations are most noticeable when hashing window- or eye-space coordinates, but they can also arise for texture-space inputs. Including the z-coordinate in the hash trivially removes these correlations. We recommend always hashing with 3D coordinates to avoid potential correlations.

### 5.4 Achieving Stable Pixel-Scale Noise

Under slow movement we do not want new thresholds between frames, as this causes severe temporal noise. But we still expect to vary between pixels, allowing dithering of opacity over adjacent pixels. This suggests using pixel- scale noise, allowing reuse of for subpixel movements. Since noise is anchored to the surface, for larger motions will *also* be reused, just in different pixels.

*5.4.1 Stability For Screen-Space Translations in X and Y* For stable pixel-scale noise, we normalize our object-space coordinates by their screen-space derivatives and clamp. This causes all values in a pixel sized region to generate the same hashed value: *//* Find the derivatives of our object-space coordinates floatpixDeriv = max( length(dFdx(objCoord.xyz)),
length(dFdy(objCoord.xyz)));

*//* Scale of noise in pixels (w/ user param g_HashScale) floatpixScale = 1.0/(g_HashScale*pixDeriv);

*//* Compute our alpha threshold float = hash3D( floor(pixScale*objCoord.xyz));

pixScale objCoord floor()

g_HashScale g_HashScale is useful if the chosen hash outputs noise at another frequency. Also, when temporal antialiasing (see Section 7.2.1), using noise with subpixel scale can allow for temporal averaging.

*5.4.2 Stability For Screen-Space Translations in Z* That approach gives stable noise under small vertical and horizontal translations. But moving along the camera’s *z*- axis continuously changes derivatives dFdx() and dFdy(), thus changing hash inputs. This gives noisy results, com- parable to stochastic alpha testing, as thresholds are effectively randomized by the hash each frame. For stability under *z*-translations, we need to discretize changes induced by such motion. In this case, only pixDeriv changes, so discretizing it adds the needed stability: But this still exhibits discontinuities if pixDeriv simulta- neously changes between discrete values in many pixels,
e.g., when drawing large, view-aligned billboards. Ideally, we would change our noise slowly and continuously by interpolating between hashes based on two discrete values pixDeriv *//* Find the discretized derivatives of our coordinates

|// Find|the discretized|derivatives|of our|coordinates|
|---|---|---|---|---|
|floatmaxDeriv floatpixScale|= max( = 1.0/(g_HashScale*maxDeriv);|length(dFdx(objCoord.xyz)), length(dFdy(objCoord.xyz))||);|
|// Find|two nearest|log-discretized|noise|scales|
|vec2 pixScales|= vec2(|exp2(floor(log2(pixScale))), exp2(ceil(log2(pixScale)))||);|
|// Compute vec2 alpha=vec2(hash3D(floor(pixScales.x*objCoord.xyz)),|alpha thresholds hash3D(floor(pixScales.y*objCoord.xyz)));|at|our two noise|scales|
|// Factor|to linearly|interpolate|with||
|floatlerpFactor|= fract(|log2(pixScale)|);||
|// Interpolate|alpha|threshold|from noise|at two scales|
|floatx|= (1-lerpFactor)*alpha.x||+ lerpFactor*alpha.y;||
|// Pass|into CDF to|compute uniformly|distrib|threshold|
|floata vec3 cases|= min(lerpFactor, = vec3(x*x/(2*a*(1-a)), (x-0.5*a)/(1-a), 1.0-((1-x)*(1-x)/(2*a*(1-a)))|1-lerpFactor|);|);|
|// Find|our final, uniformly|distributed|alpha|threshold|
|float|= (x < (1-a)) ((x cases.z;|? < a) ? cases.x|: cases.y)|:|
|// Avoids|== 0.|Could also|do =1-||
|= clamp(|, 1.0e-6,|1.0);|||

Listing 1: Code for our (isotropic) hashed alpha threshold.

threshold

|// Interpolate|alpha|from noise|at two scales|
|---|---|---|---|
|float|= (1-lerpFacor)*alpha.x|+ lerpFactor*alpha.y;||

This *almost* achieves our goal, but has two problems. First, it fails for 0 maxDeriv *<* 1. To solve this we discretize pixScale on a logarithmic scale, akin to the logarithmic steps in a texture’s mipmap chain, instead of discretizing pixDeriv on a linear scale:

|// Scale|of noise in|pixels (w/|user param|g_HashScale)||
|---|---|---|---|---|---|
|floatpixScale|= 1.0/(g_HashScale*maxDeriv);|||||
|// Discretize|pixScales|on a logarithmic||scale||
|vec2 pixScales|= vec2(|exp2(floor(log2(pixScale))), exp2(ceil(log2(pixScale)))|||);|
|// Factor|to interpolate|lerp with||||
|floatlerpFactor|= fract(|log2(pixScale)||);||

A trickier problem arises during interpolation. A well- designed hash function *f* : R²*!* [0*::* 1) produces uniformly distributed output values. Interpolating between two uni- formly distributed values does *not* yield a new uniformly distributed value in [0*::* 1). This introduces strobing because the variance of our hashed noise changes during motion. Fortunately, we can transform our output back into an uniform distribution by computing the cumulative distribu- tion function (of two interpolated uniform random values) and substituting in our interpolated threshold. The cumula- tive distribution function is: 8 2 > >2*a*(1 <u>x</u>

*a*) : 0 *x < a*
< cdf(*x*) = <u>x</u> 1 <u>a=</u> *a* <u>2</u> : *a x <* 1 *a* (2) > ><u>(1 x)</u> :1 : 1 *a x <* *a*(1 *a*)

for *a* = min(lerpFactor, 1-lerpFactor). Combining these improvements gives the final compu- tation for shown in Listing 1.

Fig. 6: Our spatial hash from Listing 1 implicitly voxelizes

space (boxes). Samples in a voxel hash to the same value. Depending on viewing angle, voxels intersect triangles (red regions) either (left) isotropically or (center) anisotropically. Section 6 modifies hash inputs to independently scale voxel dimensions, (right) allowing us to resize intersection regions to remain roughly uniform in *x* and *y*.

### 6 ANISOTROPIC HASHED ALPHA TESTING

Listing 1 provides stable hashing, but using the length of object-space derivatives provides a uniform noise scale ir- respective of surface orientation. Unfortunately, this creates anisotropy if viewing surfaces obliquely, as projected noise scales differ along screen-space *x*- and *y*-axes (see Figure 6).

### 6.1 Difficulties Removing Anisotropy

We tested numerous methods to remove anisotropy, before realizing the impossibility given our desire for stable, uni- form hash values varying on a discrete object-space grid. Traditional anisotropic texture accesses repeatedly sam- ple along a texture space vector. In hashed alpha testing, this breaks stability by introducing temporal variance between multiple subpixel samples. This approach fails to meet hashed alpha’s goal: generation of a single pseudorandom threshold rather than producing a nicely filtered value. Discretizing hash inputs on a coordinate frame aligned with the axes of anisotropy seems a compelling alternative. But this discretization reintroduces temporal instability as the axes of anisotropy change under most types of motion. Continuing to discretize in object-space prevents a com- plete removal of anisotropy, as mismatches between eye- and object-space sampling grids introduce small, but vari- able, amounts of anisotropy over the image.

### 6.2 Mitigating Anisotropy

Given our inability to completely avoid anisotropy, we de- signed a simple algorithm that significantly reduces appar- ent anisotropy yet maintains a approach similar to Listing 1. Anisotropy arises when voxels of constant hash project into screen space with different *x*- and *y*-extents (see Fig- ure 6). With uniform discretization, we can generate pixel- scale noise only along one axis. Noise along the other axis will either be subpixel or too large, reintroducing temporal flicker or generating elongated regions of constant hash (see

Figure 7).

To mitigate this problem, we select a separate dis- cretization scale along each of the three object-space axes, rather than picking a single scale based on the maximum projected derivative. Changing these scales independently significantly reduces anisotropy:

(a) Traditional (b) Isotropic (c) Anisotropic
Fig. 7: An alpha-tested chain fence at extreme grazing an-

gle. (a) Traditional alpha tests make chains disappear. (b) Isotropic hash samples get elongated along one axis. (c) Our anisotropic hash samples have much better shape.

vec3 anisoDeriv = max( abs(dFdx(objCoord.xyz)), abs(dFdy(objCoord.xyz))); vec3 anisoScales = vec3( 1.0/(g_HashScale*ansioDeriv.x),

1.0/(g_HashScale*anisoDeriv.y),
1.0/(g_HashScale*anisoDeriv.z));
But using separate scales along our axes changes the average size of our implicit voxels. The length length(dFdx(objCoord)) is longer than the individual components of dFdx(objCoord), so anisotropic noise appears smaller for a given hash scale. To maintain a consistent scale noise, we needed to scale our *p* lengths by 2: vec3 anisoScales = vec3( 0.707/(g_HashScale*ansioDeriv.x),

0.707/(g_HashScale*anisoDeriv.y),
0.707/(g_HashScale*anisoDeriv.z));
We then compute log-discretized scales independently for each of our axes: vec3 scaleFloor = vec3( exp2(floor(log2( anisoScales.x))), exp2(floor(log2( anisoScales.y))), exp2(floor(log2( anisoScales.z)))); vec3 scaleCeil = vec3( exp2(ceil(log2( anisoScales.x))), exp2(ceil(log2( anisoScales.y))), exp2(ceil(log2( anisoScales.z))));

And then we compute two hash values at opposite corners of this anisotropic box: vec2 alpha = vec2(hash3D(floor(scaleFloor*objCoord.xyz)), hash3D(floor(scaleCeil*objCoord.xyz)));

We tried computing hash values at the 8 corners of each anisotropic voxel, using trilinear interpolation with an ap- propriate CDF to correct the interpolated distribution. But this adds significant computation cost without improving hash sample stability. Instead, we linearly interpolate be- tween two hash values using the following factor represent- ing a pixel’s fractional pre-discretized location in log-space:

*//* Find the discretized derivatives of our coordinates vec3 anisoDeriv = max( abs(dFdx(objCoord.xyz)), abs(dFdy(objCoord.xyz))); vec3 anisoScales = vec3(

0.707/(g_HashScale*ansioDeriv.x),
0.707/(g_HashScale*anisoDeriv.y),
0.707/(g_HashScale*anisoDeriv.z));
*//* Find log-discretized noise scales vec3 scaleFlr = vec3( exp2(floor(log2(anisoScales.x))), exp2(floor(log2(anisoScales.y))), exp2(floor(log2(anisoScales.z)))); vec3 scaleCeil = vec3( exp2(ceil(log2(anisoScales.x))), exp2(ceil(log2(anisoScales.y))), exp2(ceil(log2(anisoScales.z))));

*//* Compute alpha thresholds at our two noise scales vec2 alpha = vec2(hash3D(floor(scaleFlr**objCoord.xyz))); objCoord.xyz)), hash3D(floor(scaleCeil

*//* Factor to linearly interpolate with vec3 fractLoc = vec3( fract(log2( anisoScale.x)), fract(log2( anisoScale.y)), fract(log2( anisoScale.z))); vec2 toCorners = vec2( length(fractLoc), length(vec3(1.0f)-fractLoc)); floatlerpFactor = toCorners.x/(toCorners.x+toCorners.y);

*//* Interpolate alpha threshold from noise at two scales floatx = (1-lerpFactor)*alpha.x + lerpFactor*alpha.y;

*//* Pass into CDF to compute uniformly distrib threshold floata = min( lerpFactor, 1-lerpFactor); vec3 cases = vec3( x*x/(2*a*(1-a)), (x-0.5

1.0-((1-x) *a)/(1-a), *(1-x)/(2*a*(1-a))));
*//* float Find our = (x final, < (1-a)) uniformly ? distributed alpha threshold

((x < a)? cases.x : cases.y) : cases.z;

*//* Avoids == 0. Could also do =1- = clamp(, 1.0e-6, 1.0);

Listing 2: Code for our anistropic hashed alpha threshold.

|vec3 fractLoc|= vec3(fract(log2(|anisoScale.x|)),|
|---|---|---|---|
|vec2 toCorners floatlerpFactor|fract(log2( fract(log2( = vec2(length(fractLoc), length(vec3(1.0f)-fractLoc) = toCorners.x|anisoScale.y anisoScale.z / (toCorners.x+toCorners.y);|)), ))); );|

Given this interpolation factor, the rest of the hashed alpha code remains the same, correcting for the CDF of interpo- lation between two uniform hashed samples to give a final uniform threshold. This code is shown in Listing 2.

### IMPLEMENTATION CONSIDERATIONS

When adding a simple modification like hashed alpha testing into an existing renderer, a developer’s goal is im- proving quality without changing the rendering workflow.

This requires working nicely with other common algorithms without introducing new artifacts. This section looks at some implementation considerations for using hashed alpha tests in existing engines.

### 7.1 Fading in Noise with Distance

Because of hashed alpha testing’s basis in stochastic sam- pling, it introduces (stable) noise everywhere alpha-tested geometry is used. Developers may want to avoid introduc- ing apparent randomness near the viewer, where existing alpha testing works fairly well, and focus on improving visual quality in the distance. Fortunately, we can fade in hashed noise with distance. Consider the following formulation of our alpha threshold:

= 0*:* 5 +*;* (3)

where = 0 for traditional alpha testing and *2* [ 0*:* 5*:::* 0*:* 5) for hashed and stochastic variants. We suggest modifying this as: = 0*:* 5 + *b*(lod)*;* (4)

where *b*(lod) slowly blends in the noise, i.e., *b*(0) = 0 and *b*(*n*) = 1 for some lod = *n* coarse enough for the developer to rely entirely on hashed alpha tests. Values for *n* depend on desired texture size and noise tolerance; we found *n*= 6 worked well in our experiments.

We found that linearly ramping *b* still kept visible noise too close to the camera. A quadratic ramp gave better results, perhaps because apparent noise depends on solid angle, which changes with the square of distance. We used the following function to transition between traditional and hashed alpha testing: 8 > <0 : *x* 0 2 *b*(*x*) = (*x=n*) : 0 *< x < n* (5) > : 1 : *x n:*

*7.1.1 Fading in Noise With Anisotropic Texture Sampling* Equation 5 fails for alpha-tested surfaces viewed at a graz- ing angle. This occurs since anisotropic filtering repeatedly accesses finer mip levels, causing alpha geometry to dis- appear even at relatively low mip levels. This means the transition to a hashed alpha test needs to occur at lower mip levels than in regions sampled isotropically. Scaling *x* based on anisotropy, before computing *b*(*x*), fixes this:

|// Find|degree of|anisotropy|from texture|coords|
|---|---|---|---|---|
|vec2 dTex|= vec2(|length(dFdx(texCoord.xy)), length(dFdy(texCoord.xy))||);|
|floataniso|= max(|dTex.x/dTex.y,|dTex.y/dTex.x|);|
|// Modify|inputs|to b(x)|on degree|of aniso|
|x = aniso|* x;||||

based

Higher anisotropy increases *x*, varying more in Equa- tion 4, avoiding alpha maps disappearing at grazing angles.

### 7.2 Hashed Alpha Testing With Temporal Antialiasing

Given widespread use of temporal antialiasing (TAA) in games [5], hashed alpha testing needs to work seamlessly with temporal accumulation techniques. Since hashed test- ing derives from stochastic sampling, one might assume it works trivially with TAA. But changing hash inputs to induce stability explicitly creates pixel-sized noise. Naive application of TAA gives smooth-edged, pixel-sized blocks rather than accumulating stochastic coverage temporally. We see three simple approaches to integrate TAA with hashed alpha testing: reduce the global noise scale below one pixel, use temporally independent hash samples, or

|1|1|
|---|---|
|2|1 2|
||2|

temporally stratify the hash samples. These techniques all reduce noise, but as in stochastic transparency [11], the eight jittered samples TAA typically uses is insufficient for converged results. This can introduce temporal flicker. We found temporally stratifying hash sam- ples gave the most stable results (see Section 7.2.3).

*7.2.1 Temporal Antialiasing by Reducing Noise Scale* Reducing the global noise scale g_HashScale to a value be- low 1*:* 0 gives subpixel noise. TAA’s temporal camera jitter averages this subpixel noise over multiplepframes. For *n* temporal samples, using a g_HashScale of 1*=n* gives the ideal number of *n* unique subpixel hash values. Unfortunately, reducing noise scale below 1*:* 0 reduces hash sample stability under motion. Until sampling suffi- ciently for a converged solution, this effectively trades off spatial stability for temporal stability. With the 8 TAA sam- ples commonly used today, this approach does not provide a particularly compelling solution.
*7.2.2 Temporal Antialiasing by Independent Hashing* Instead of introducing subpixel noise and trusting to TAA jitter to accumulate the results, another approach keeps the pixel-scale hash grid but chooses *n* different hash values over the *n* frames in a *n*-sample TAA. A naive solution uses multiple different hash functions, using = hash[*i*](hashInput*:* xyz) for frame *i*, repeating every *n* frames. However for 8-sample TAA, this requires designing 8 good hash functions and dynamically selecting which to execute each frame. In theory, separate regions of hash space are also inde- pendent. This means hash(input) and hash(input + oset) give independent random thresholds, allowing us to select = hash(hashInput*:* xyz + oset[*i*]) for frame *i*. In this case, rather than designing separate hash functions, we just need to provide a list of *n* unique vector offsets. While this approach gives a stable set of *n* hash thresh- olds over any set of *n* frames, it still gives temporally unstable results. Since TAA effectively averages via an ex- ponentially weighted moving average, the highest weighted threshold has an *n*-frame cycle. When these *n* samples are independent, this introduces temporal flicker.
*7.2.3 Temporal Antialiasing by Temporal Stratification* Rather than using independent hash samples, we can select *n* dependent alpha thresholds, stratifying them to uniformly sample [0*:::* 1) and distributing them temporally to reduce the flicker introduced by an exponentally weighted moving average. Given a hash sample, computed via the code in Listings 1 or 2, we define alpha threshold over an *n*- frame TAA sequence:
= fract + <u>i</u> *; 8 i 2* [0*:::n* 1] (6) *n*

As with the independent hash samples in Section 7.2.2, this gives spatial stability but temporally flickers as the TAA cycles between heavily weighing low and high thresholds. An exponential moving average weighs one of our *n* stratified samples higher than others, but we can reorder to ensure good distribution of our two highest weighted samples. With a stratified sample set, we know half of our samples will have *<* and half will have. We should alternate so every other sample is below, e.g.:

*j* = <u>i</u> + (*i* mod 2) <u>n</u> 2 2 <u>j</u> = fract + *n*

This provided sufficient stability for us over 8 temporal samples, though for an even better, low discrepancy sam- ple distribution we could define *j* using a radical-inverse sequence like the binary van der Corput sequence.

### 7.3 Using Premultiplied Alpha

As hashed alpha testing accesses diffuse texture samples from a somewhat larger region than standard alpha tests, care is required to avoid sampling in-painted diffuse colors added by artists in transparent regions, especially at higher mip levels when we filter from large texture regions.

Using premultiplied alpha allows correct mipmaping and avoids this problem (e.g., see Glassner [19] for fur- ther discussion). However premultipled alpha textures store (*R;G;B;*), so we need to divide by alpha before returning our alpha tested color (*R;G;B*) to maintain con- vergence to ground truth when increasing sample counts. Hashed alpha testing works with non-premultiplied dif- fuse textures, but we frequently found that when our hash returned *<* 0*:* 5, colors bled from transparent texels and introduced arbitrary colored halos at alpha boundaries.

### 8 APPLICATIONS TO ALPHA-TO-COVERAGE

As noted in Section 3, alpha-to-coverage discretizes frag- ment alpha and outputs *bnc* coverage bits dithered over an *n*-sample buffer. Generating *bnc* coverage bits is equiv- alent to supersampling the alpha threshold, i.e., performing *n* alpha tests with thresholds:

<u>0: 5 1: 5 n 0: 5</u> =*;;:::; :* (7) *n n n* This observation reveals that traditional alpha testing is a special case, where *n* = 1. Applying hashed or stochastic alpha testing to alpha-to- coverage is equivalent to jittered sampling of the thresholds:

|1 +|n||
|---|---|---|
|1|2|n|
|i|i||
|||i j|

<u>1 +</u> =*;;:::;;* (8) *n n n* for hashed samples *2* [0*:::* 1). can be chosen indepen- dently per sample or once per fragment (i.e., =). Traditional alpha-to-coverage uses fixed dither patterns for all fragments with the same alpha. This introduces corre- lations between overlapping transparent fragments, causing aggregate geometry to lose opacity (see Figure 1). To avoid this, we compute a per-fragment offset*o*, increment+, and apply per-sample alpha thresholds:

<u>i+ ((o+ i+) mod n)</u> =*; 8 i 2* [0*:::n* 1] (9) *n*

|; 2 [0::1) and limiting n to powers of two,||and :||||||
|---|---|---|---|---|---|---|---|
|1 2|1 + floathash3D_ float{|2 returnhash( hash3D_ {returnhash(|(vec3 in) (vec3vec2(in) vec2(|hash( in.x,|in.xy hash(|), in.z in.yz)|));} ));}|

Given hashed *o*= *bn₁c* is an integer between 0 and *n* 1 and = 2*b*0*:* 5*n₂c*+ 1 is an odd integer between 1 and *n* 1. This decorrelates the jittered thresholds if1or2varies with distance to the camera.

### 8.1 Applications to Screen Door Transparency

|floathash3D_|(vec3|in) {returnhash3D(|in+offset[0]|);}|
|---|---|---|---|---|
|floathash3D_|(vec3|in) {returnhash3D(|in+offset[1]|);}|

Screen-door transparency simply dithers coverage bits over multiple pixels rather than multiple sub-pixel samples. So similar randomization of the interleaved thresholds and decorrelation between layers can occur between pixels

|vec3 direction|= vec3(|sqrt(|) * cos(|2),||
|---|---|---|---|---|---|
|||sqrt( sqrt(|) * sin( max(0.0, 1.0|2), -)|));|

rather than within a pixel.

### 9 OTHER APPLICATIONS OF HASHED SAMPLING

Until this point, we focused our description on how stable hashed samples apply to disappearing alpha-tested geom- etry. However, interactive applications strive for temporal stability in various domains, and stable hashing may prove useful either for directly generating samples or improving the quality of reprojection caching [20]. For example, light transport algorithms often use Monte Carlo sampling to approximate the rendering equation [21].

(a) Isotropic samples (b) Anisotropic samples
Fig. 8: Using hashed pseudorandom samples for ambient

occlusion, with one visibility ray per pixel. Noise remains largely stable under motion and stays fixed to the geometry. The top image uses ansiotropic sampling (see Section 6), but insets compare (a) isotropic and (b) anisotropic sampling.

Real-time constraints can prevent the increased ray counts needed to avoid temporal noise. A common approach defines per-pixel, fixed pseudo- random seeds, but this induces a screen-door effect visible as geometry moves relative to the screen. We instead generate samples with our stable, pixel-sized grid. This produces largely stable noise that stays fixed to the geometry, even under motion. We demonstrate this for single ray per pixel ambient occlusion (see Figure 8). This is simple enough to visually understand the hash samples, yet remains repre- sentative of more complex light transport. For our simple test, rather than computing via List- ing 2, we generate two hash samples1*;*2*2* [0*:::* 1) with the same code, but using two different hash3D() functions for 1 2 1

As described in Section 7.2.2, independent samples can also be generated using different offsets, e.g.,: 1 2

We convert these hashed values into a cosine-sampled hemi- sphere in the standard way: 1 2 1 2 1

Extending to more general light transport algorithms, which require many stable samples each frame, simply requires us- ing additional oset[*i*] values. Alternatively, a hash return- ing an integer could seed a more traditional pseudorandom number generator.

### STOCHASTIC TRANSPARENCY COMPARISON

Stochastic alpha testing essentially simplifies stochastic transparency to one sample per pixel. Hence, when using

TABLE 1: Cost comparisons for traditional, hashed, and

stochastic alpha tests at 1920 1080 on a GeForce GTX 1080. Performance of isotopic and anisotropic hashing were equal, given our measurement error; the single column in our table represents both.

|||Trad.|Hashed|Stoch.|
|---|---|---|---|---|
|Scene|# tris|test|test|test|
|Single fence|2|0.06 ms|0.08 ms|0.20 ms|
|Bearded Man|7.6 k|0.14 ms|0.16 ms|0.58 ms|
|Potted Palm|68 k|0.10 ms|0.12 ms|0.42 ms|
|Bishop Pine|158 k|0.22 ms|0.30 ms|0.75 ms|
|Japanese Walnut|227 k|0.28 ms|0.36 ms|0.99 ms|
|European Beech|386 k|0.39 ms|0.50 ms|1.69 ms|
|Sponza with Trees|900 k|1.05 ms|1.15 ms|4.04 ms|
|QG Tree|2,400 k|1.08 ms|1.22 ms|3.01 ms|
|UE3 FoliageMap|3,000 k|2.52 ms|2.86 ms|11.42 ms|
|San Miguel|10,500 k|5.19 ms|5.28 ms|7.30 ms|

(a) Traditional (b) Hashed (c) Castano [8]
Fig. 10: A comparison between traditional and hashed alpha

testing with Castano’s precomputed per-mip modifications to texture alpha, which tends to enlarge thin geometry.

more tests per pixel stochastic alpha testing converges to ground truth, just as stochastic transparency does. In this light, based on the thresholds in Equation 7, alpha-to-coverage is stochastic transparency with regular instead of random samples. Using randomized thresholds, as in Equation 8, corresponds to stratified stochastic trans- parency. But a key difference is that alpha testing is designed and frequently *expected* to work with a single sample per pixel. Avoiding temporal and spatial noise is key for adoption, hence our stable hashed alpha testing, which we believe pro- vides an appealing alternative to traditional alpha testing.

### 11 RESULTS

We prototyped our hashed and stochastic alpha test in an OpenGL-based renderer using the Falcor prototyping library [22]. We did not optimize performance, particularly for stochastic alpha testing, as we sought stable noise rather than optimal performance. Timings include logic to explore variations to hashes, fade-in functions, and other normaliza- tion factors.

Table 1 shows performance relative to traditional alpha

testing, rendered at 1920 1080 and using one shader for all surfaces, transparent and opaque. Our added overhead for hashed alpha testing is all computation, without additional texture or global memory accesses. Anistopic hashed alpha testing increases costs, perhaps 10–20% over the isotropic variant, but with our limited timing precision and our test’s small overhead the timing runs were identical for both variants. Our stochastic alpha test prototype uses a random seed texture, requiring synchronization to avoid correlations from seed reuse. This causes a significant slowdown. Cost varies with number, depth complexity, and screen coverage of alpha-mapped surfaces. At 1920 1080 with one test per fragment, our hashed alpha test costs an additional

0.1 to 0.3 ms per frame for scenes with typical numbers of alpha-mapped fragments. For stochastic alpha testing, syn- chronization costs increase greatly in high depth complexity scenes.
Figure 1 shows a game-quality head model with alpha-
 mapped hair billboards. With distance the hair disappears. This is most visible in his beard, as the underlying diffuse
(a) Traditional (b) Hashed (c) Fade in hash
Fig. 12: With alpha testing, tree leaves disappear with dis-

tance. Hashed alpha testing keeps these leaves, but nearby leaves have noisy edges and, due to alpha of 0.99, some in- ternal noise. Fading in the hash contribution, per Section 7.1, keeps distant leaves without nearby noise.

texture has no hair painted on his chin. See the supplemental video for dynamic comparisons with this model.

Figure 9 shows similar comparisons on a number of

artist-created tree models provided as samples by XFrog. These trees’ alpha maps contain 50–75% transparent pixels, causing foliage to disappear quickly when rendered in the distance or at low resolution. Hardware accelerated alpha- to-coverage has high layer-to-layer correlation that causes leaves to appear as a single layer and overly transparent. Hashed alpha testing and hashed alpha-to-coverage fix these problems and appear much closer to the ground truth, despite rendering at lower resolution. In Figures 10 and 11, we compare hashed alpha against the widely used approach of Castano [8], which computes a per-mipmap alpha threshold and bakes this into the texture as a preprocess. This approach dilates distant geometry to keep it visible, giving an overly dense appearance on the thin leaves in Figure 10.

Figure 11 compares hashed alpha testing, 8 MSAA us-

ing hashed alpha-to-coverage, and both hashed techniques with temporal antialiasing. Interestingly using hashed alpha testing with TAA gives results largely similar to alpha-to- coverage, and temporally antialiased alpha-to-coverage is nearly indistinguishable from the ground truth.

Figure 12 shows a more complex example where hashed

noise may be undesirable nearby and the fade-in from Section 7.1 maintains crisp edges near the viewer.

Traditional Traditional Hashed Hashed Alpha Test Alpha-to-Coverage Alpha Test Alpha-to-Coverage Ground Truth

Bishop Pine Tree

Japanese Walnut

European Beech

Potted Palm

Fig. 9: Four plant models whose alpha-mapped polygons disappear with distance. This also happens when rendering at

lower resolution (left four columns), which allows for better comparisons to our supersampled ground truth (right column). Notice how alpha testing loses alpha-mapped details and alpha-to-coverage introduces correlations that under represent final opacity where transparent polygons overlap. Both hashed alpha testing and hashed alpha-to-coverage largely retain appropriate coverage, but both introduce some noise.

(a) Ground truth (b) Traditional (c) Castano [8] (d) Hashed (e) Hashed, TAA (f) Hash A2C (g) A2C w/TAA
Fig. 11: Comparing distant renderings of the bearded man using various techniques, including a ground truth sorted

blend, traditional alpha testing, Castano’s tweaks to texture alpha values, as well as hashed alpha testing and hashed alpha-to-coverage (both with and without temporal antialiasing).

(a) (b) (c)
Fig. 13: Difference images from before and after a sub-pixel

translation along the *x*-axis using a (a) traditional alpha test,

(b) hashed alpha test, and (c) stochastic alpha test. Difference images inverted for better visibility.
(a) (b) (c) (d) (e)
Fig. 14: A synthetic example, with a checkerboard texture

where half the texels have = 0 and half have = 1. We compare (a) traditional alpha testing, (b) isotropic hashed alpha, (c) anisotropic hashed alpha, (d) 8 isotropic alpha- to-coverage, and (e) 8 anisotropic alpha-to-coverage.

Figure 13 compares the temporal stability of traditional,

hashed, and stochastic alpha testing under slight, sub-pixel motion. Note that under the same sub-pixel motion hashed alpha testing exhibits temporal stability roughly equivalent to traditional alpha testing. Stochastic alpha testing and methods that do not anchor noise or discretize it to pixel scale exhibit significantly more instability.

Figure 14 shows a synthetic example of a planar checker-

board texture containing half opaque and half transparent texels. In this case, alpha testing does not disappear with distance but aliases. Hashed alpha testing replaces this alias- ing with noise, and using MSAA-based alpha-to-coverage

(a) Traditional alpha test (b) Hashed alpha test
Fig. 15: Two foveated images, as per Patney et al. [23]. In

both, the viewer looks towards the curtains (yellow circle). Regions outside the circle use progressively lower quality. Low resolution shading uses coarser mipmap levels, causing traditional alpha tests to fail. With hashed alpha testing, the foliage maintains its aggregate appearance.

converges to a desired uniform gray in the distance. Beyond use for distant or low-resolution alpha-mapped geometry, other applications exist for hashed alpha testing. In head-mounted displays for virtual reality, rendering at full resolution in the user’s periphery is wasteful, especially as display resolutions increase. Instead, foveated render- ing [24] uses at lower resolution away from a user’s gaze. Patney et al. [23] suggest prefiltering all rendering terms, but they were unable to support alpha testing due to an inability to prefilter the results. Naive alpha testing in foveated rendering causes even nearby foliage to disappear in the periphery (see Figure 15). With temporal antialiasing, hashed alpha testing enables use of alpha mapped geometry in foveated renderers.

Figure 16 shows hashed alpha testing in a more complex

environment. Results are more subtle as scene scale is small enough to minimize the pixels accessing coarse mipmaps.

### 12 CONCLUSIONS

We introduced the idea of *stochastic* or *hashed alpha testing* to address the problem of alpha-mapped geometry disappear- ing with distance. Stochastic alpha testing uses a random rather than a fixed threshold of 0*:* 5. To address the temporal noise stochasm introduces, we proposed a procedural hash to provide a stable noisy threshold. We obtained stable, pixel scale noise by hashing on dis- cretized object-space coordinates at two scales. We showed

(a) Traditional alpha test (b) Hashed alpha test
Fig. 16: Disappearing geometry in San Miguel.

how to ensure the interpolated hash value maintains a uniform distribution, how to maintain sample quality in the presence of anisotropy, and how hashed alpha testing works with temporal antialiasing. We demonstrate temporal stability both in Figure 13 and the accompanying video. And we suggested how this stable sampling scheme could extend to other stochastic light transport effects. We provided inline code to replicate our hashed test. Thinking about alpha-to-coverage and screen door trans- parency in the context of varying provides insights, showing them all to be different discrete sampling strate- gies for transparency: alpha test and alpha-to-coverage perform regular sampling, screen-door transparency inter- leaves samples, stochastic alpha testing randomly samples, and hashed alpha testing uses quasi-random sampling via a uniform hash function. While our hashed test provides spatially and temporally stable noise without scene-dependent parameters, we did not explore the space of 2D and 3D hash functions to see which minimizes flicker between frames. Additionally, using more sophisticated hash inputs than object-space co- ordinates may generalize over a larger variety of highly instanced scenes. Both areas seem fruitful for future work.

### ACKNOWLEDGMENTS

Our work evolved from a larger research project, so many researchers contributed directly and indirectly. Thanks to Pete Shirley for keeping our hash uniform by deriving the cdf in Equation 2, Cyril Crassin for suggesting alpha maps as a simplified domain worth studying, Anton Kaplanyan for discussing stable noise, Anjul Patney for trying hashed alpha in foveated VR, and Dave Luebke, Aaron Lefohn, and Marco Salvi for additional discussions. Also, thanks to the insightful I3D and TVCG reviewers and numerous helpful comments from the game developer community on Twitter.

### REFERENCES

[1]C. Wyman and M. McGuire, “Hashed alpha testing,” in *Symposium* *on Interactive 3D Graphics and Games*, February 2017, pp. 7:1–7:9. [2]T. Porter and T. Duff, “Compositing digital images,” in *Proceedings* *of SIGGRAPH*, 1984, pp. 253–259. [3]C. Wyman, “Exploring and expanding the continuum of OIT algorithms,” in *High Performance Graphics*, June 2016, pp. 1–11. [4]T. Saito and T. Takahashi, “Comprehensible rendering of 3-d shapes,” *Proceedings of SIGGRAPH*, pp. 197–206, 1990. [5]B. Karis, “High-quality temporal supersampling,” in *SIGGRAPH* *Course Notes: Advances in Real-Time Rendering in Games.*, 2014.

[6]T. Lottes, “FXAA,” NVIDIA, http://developer.download.nvidia. com/assets/gamedev/files/sdk/11/FXAA WhitePaper.pdf, Tech. Rep., 2009. [7]L. Yang, D. Nehab, P. V. Sander, P. Sitthi-amorn, J. Lawrence, and H. Hoppe, “Amortized supersampling,” *ACM Transactions on* *Graphics*, vol. 28, no. 5, p. 135, 2009. [8]I. Castano, “Computing alpha mipmaps,” http://the- witness.net/news/2010/09/computing-alpha-mipmaps/, 2010. [9]S. Lefebvre and H. Hoppe, “Perfect spatial hashing,” *ACM Trans-* *actions on Graphics*, vol. 25, no. 3, pp. 579–588, Jul. 2006. [10]C. A. L. Pahins, S. A. Stephens, C. Scheidegger, and J. L. D. Comba, “Hashedcubes: Simple, low memory, real-time visual exploration of big data,” *IEEE Transactions on Visualization and Computer Graph-* *ics*, vol. 23, no. 1, pp. 671–680, Jan 2017. [11]E. Enderton, E. Sintorn, P. Shirley, and D. Luebke, “Stochastic transparency,” in 2010, pp. 157–164. *Symposium on Interactive 3D Graphics and Games*,

[12]J. Moore and D. Jefferies, “Rendering technology at Black Rock Studios,” in *SIGGRAPH Course Notes: Advances in Real-Time Ren-* *dering in Games.*, 2009. [13]A. Kharlamov, I. Cantlay, and Y. Stepanenko, *GPU Gems 3*. Addison-Wesley, 2008, ch. Next-Generation SpeedTree Rendering, pp. 69–92. [14]J. Mulder, F. Groen, and J. van Wijk, “Pixel masks for screen-door transparency,” in *Proceedings of Visualization*, 1998, pp. 351–358. [15]S. Laine and T. Karras, “Stratified sampling for stochastic trans- parency,” *Computer Graphics Forum*, vol. 30, no. 4, pp. 1197–1204,

2011.
[16]C. Wyman, “Stochastic layered alpha blending,” in *ACM SIG-* *GRAPH Talks*, 2016. [17]W. H. Press, S. A. Teukolsky, W. T. Vetterling, and B. P. Flannery, *Numerical Recipes in C (2nd Ed.): The Art of Scientific Computing*. New York, NY, USA: Cambridge University Press, 1992. [18]M. McGuire, *The Graphics Codex*, 2nd ed. Casual Effects, 2016. [19]A. Glassner, “Interpreting alpha,” *Journal of Computer Graphics* *Techniques*, vol. 4, no. 2, pp. 30–44, 2015. [20]D. Nehab, P. V. Sander, J. Lawrence, N. Tatarchuk, and J. R. Isidoro, “Accelerating real-time shading with reverse reprojection caching,” in *Proceedings of Graphics Hardware*, 2007, pp. 25–35. [21]J. Kajiya, “The rendering equation,” in 1986, pp. 143–150. *Proceedings of SIGGRAPH*,

[22]N. Benty, K.-H. Yao, A. S. Kaplanyan, C. Lavelle, and

C. Wyman, “Falcor real-time rendering framework,” https://github.com/NVIDIA/Falcor, 2016.
[23]A. Patney, M. Salvi, J. Kim, A. Kaplanyan, C. Wyman, N. Benty,

D. Luebke, and A. Lefohn, “Towards foveated rendering for gaze- tracked virtual reality,” *ACM Transactions on Graphics*, vol. 35, no. 6, pp. 179:1–12, 2016.
[24]B. Guenter, M. Finch, S. Drucker, D. Tan, and J. Snyder, “Foveated 3d graphics,” *ACM Transactions on Graphics*, vol. 31, no. 6, pp. 164:1–10, 2012.

**Chris Wyman** is a Principal Research Scientist in NVIDIA’s real-time rendering research group located in Redmond, WA. Before moving to Red- mond, he was a Visiting Professor at NVIDIA and an Associate Professor of Computer Science at the University of Iowa. He earned a PhD in com- puter science from the University of Utah and a BS in math and computer science from the University of Minnesota. Recent research inter- ests include efficient shadows, antialiasing, and participating media, but his interests span the range of real-time rendering problems from lighting to global illumination, materials to color spaces, and optimization to hardware improvements.

**Morgan McGuire** is a professor of Computer Science at Williams College and a researcher at NVIDIA. He is the author or coauthor of *The* *Graphics Codex*, *Computer Graphics: Principles* *and Practice, 3rd Edition*, and *Creating Games*. He received his degrees at Brown University and

M.I.T.
