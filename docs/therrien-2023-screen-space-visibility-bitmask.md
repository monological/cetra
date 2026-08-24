# Screen Space Indirect Lighting with Visibility Bitmask (Therrien et al., 2023)

Converted from the arXiv PDF (2301.11376v2) with `pdf2md`. Two-column with inline
equations and an algorithm listing, so the maths arrives as prose fragments and
the figures are gone -- a reading copy. Go to the PDF for anything load-bearing.

**This is the paper `shaders/gtao_frag.glsl` implements**, and its header says so.
The engine's AO is not classic GTAO: it marks occluded angular SECTORS in a
32-bit mask per hemisphere slice, which is this method.

## What the engine follows, and what it does not

Checked against the source while diagnosing the contact-ring artifact in spec
11.75. Recorded here because the next person to read this paper beside the
shader will want the diff, not the paper alone.

**Follows.** 32 sectors -- the paper picks that number because it "just crossed
the threshold where the artifacts became almost invisible" and fits one uint,
and the engine's `SECTOR_COUNT` matches. Angular rather than cosine space for
sample distribution (sec. 3). A small constant thickness, the paper's
recommendation over an infinite one. No distance falloff, which is deliberate in
both: the reference implementation states outright that "with visibility
bitmasks the UpdateHorizon() function from GTAO is not needed anymore, because
we don't need to apply any falloff" -- the thickness model replaces it. Anyone
tempted to "fix" the hard `len > radius` reject should read that first.

**Deviates -- the hit criterion.** Sec. 3: "we used the ROUND criterion which
requires the sector to be half covered by the sample." `sectorBits`
(gtao_frag.glsl) instead takes `floor(lo*N)` and `ceil((hi-lo)*N)`, which sets
any sector the sample touches at all. A sample grazing one per cent of a sector
occludes it fully. That is a systematic over-occlusion bias, and it is not a
tuning choice -- it is the criterion the paper names, spelled differently.

**Deviates -- what the bitmask is used for, and this is the big one.** Sec. 3.2
positions the bitmask as SUPERSEDING the bent-normal-and-cone approach:
"Screen space bent normal [KRES11] addresses this problem by sampling towards
the largest non-occluded direction, but is limited to a single ambient direction
per pixel and does not handle thickness properly. Visibility bitmasks can
improve this by weighting the ambient lighting in a given direction by the
directional occlusion." [KRES11] is Klehm et al. 2011 -- the other paper in this
directory, and the ancestor of `include/spec_occ.glsl`. So the engine computes
this paper's 32-sector directional mask, collapses it to one direction plus one
scalar, and then does 2011-era cone-overlap arithmetic on the remains.

The consequence is measurable and was the whole of 11.75: sec. 4.1 says "when
using a wide radius, GTAO has a tendency to produce halos around objects
(Figure 4). Our method doesn't have this problem." The engine haloed anyway,
because the halo is manufactured downstream of the bitmask, after the
information that would have prevented it has been discarded.

**Partially follows -- thickness.** The paper offers scaling t linearly with
distance to counter over-attenuation far from camera; the engine uses a fixed
`THICKNESS`.

O. Therrien, Y. Levesque, G. Gilet, "Screen Space Indirect Lighting with
Visibility Bitmask", The Visual Computer (2023), DOI 10.1007/s00371-022-02703-y,
arXiv:2301.11376v2. Reference implementation and code notes:
https://cdrinmatane.github.io/posts/ssaovb-code/

---

This article has been accepted for publication in The Visual Computer. This is the author’s version which has not been fully edited and content may change prior to final publication. Citation information: DOI 10.1007/s00371-022-02703-y

## Screen Space Indirect Lighting with Visibility Bitmask

Olivier Therrien 1*y* Yannick Levesque 2*z* Guillaume Gilet 3*x* 1 CDRIN, QC, Canada 2 Cégep de Matane, QC, Canada 3University of Sherbrooke

**Figure 1:** *Left: Direct illumination of the scene. Middle: Indirect lighting produced by our method (without texture). Right: Final frame*

*rendered with our method, exhibiting directionally occluded ambient lighting, and a GI bounce that avoids typical thin surface artifacts.* **Abstract** *Horizon-based indirect illumination efficiently estimates a diffuse light bounce in screen space by analytically integrat-* *ing the horizon angle difference between samples along a given direction. Like other horizon-based methods, this technique* *cannot properly simulate light passing behind thin surfaces. We propose the concept of a visibility bitmask that re-* *places the two horizon angles by a bit field representing the binary state (occluded / un-occluded) of N sectors uniformly* *distributed around the hemisphere slice. It allows light to pass behind surfaces of constant thickness while keeping the ef-* *ficiency of horizon-based methods. It can also do more accurate ambient lighting than bent normal by sampling more than* *one visibility cone. This technique improves the visual quality of ambient occlusion, indirect diffuse, and ambient light com-* *pared to previous screen space methods while minimizing noise and keeping a low performance overhead.*

**Keywords:** Real-Time Rendering, Indirect Lighting, Ambient Occlusion, Visibility arXiv:2301.11376v2 [cs.GR] 31 Jan 2023

**1. Introduction**
Screen Space Reflections (SSR) [SKS11]. This approach tends to generate a lot of noise because it implies the numerical integra- Indirect diffuse lighting is challenging to compute in real-time. tion of irradiance over the entire hemisphere around the surface. Screen space approximations can be attractive as they reduce the di- Horizon-Based Indirect Illumination (HBIL) [May18], based on mensionality of the problem and make the execution cost constant Horizon-Based Ambient Occlusion (HBAO) [BSD08, Bav11] and regardless of the scene’s geometric complexity. Modern Screen Ground Truth Ambient Occlusion (GTAO) [JWPJ16], improve the Space Global Illumination (SSGI) implementations often gather efficiency by numerically integrating over a set of directions around indirect light by doing ray marching on screen pixels similar to the view vector *v* (Figure 2) while doing analytic integration of the horizon angle difference between samples. *y* therrien.olivier@cdrin.com *z* levesqueyannick@cgmatane.qc.ca Fundamentaly, the core principle of these methods lies in the es- *x* guillaume.gilet@usherbrooke.ca timation of the scene local geometry around each shading sample

*O. Therrien, Y. Levesque, G. Gilet / Screen Space Indirect Lighting with Visibility Bitmask*
**Figure 4:** *Left: GTAO produces halos around the poles. Right: Our*

*method is able let light pass behind the poles without introducing* *halos artifacts.*

**Figure 2:** *3D view of the scene, centered on the shaded pixel.*

*GTAO/HBIL generates a set of hemisphere slices (in green) in var-* *ious directions around the view vector v. Occluders (in black) in-* *tersect some of the slices, producing occlusion cones (in red).*

by relying on readily-available screen-space information, such as the discrete depth buffer. However, such information is by essence discrete and incomplete, and must be reconstructed. All those tech- niques evaluate Ambient Occlusion, the modulation of indirect ir- radiance due to local geometry, in screen space from a single layer depth buffer, and assume infinite surface thickness by treating it as a height-field (see Figure 3). While this is a valid assumption in some cases, not knowing what the real geometry looks like, it causes ha- los and over-darkening around thin surfaces (see figure 4). Falloff heuristics are used to mitigate those artifacts but fail when using a large sampling radius.

**Figure 3:** *Side view of one slice centered on the view vector v. Left:*

*Ground truth scene with multiple occluders (in black) producing* *multiple visibility cones (in green). Right: GTAO takes a fixed num-* *ber of samples (red dots) in the depth buffer on both sides of the* *hemisphere to find highest elevation angles* q1*and* q2*.*

Our proposed method rejects the assumption that the depth buffer is strictly a height-field and models the behavior of light passing behind surfaces. Additionally, to preserve performance, we want to avoid explicitly tracing new rays from scratch to adequately sample multiple elevation angles. To do so, we introduce the con- cept of visibility bitmask, which is essentially a discretization of the hemisphere slice in *Nb*sectors, that allows us to approximate the tracing of *Nb*rays at the same performance cost as one hori- zon search.

A common limitation of single layer depth buffers is that oc- cluded surfaces are not represented, which can cause missing oc- clusion or lighting. Approaches like Deep G-Buffer [MMNL16] or Multi-layer SSRT [HBSS17] alleviate this issue by storing mul- tiple layers or sample per pixel, to provide more information on background surfaces. As rendering multiple layers is very expen- sive, we chose to limit ourselves to only one layer. This means that some artifacts caused by inaccurate background surface estimation will remain, but using a constant thickness *t* at each depth sam- ple with a visibility bitmask greatly improves quality around thin surfaces compared to horizon-based techniques. In this study, we show that visibility bitmasks can tremendously reduce noise in the image compared to SSR-like tracing, while handling the light pass- ing behind surfaces much more accurately than horizon-based tech- niques. We also show that using a visibility bitmask to sample am- bient light gives a more precise ambient estimation than traditional methods of sampling along the surface normal or even bent nor- mal. The main contributions of this paper revolve around the introduc- tion of a visibility bitmask in traditional SSGI methods, retaining the efficiency and noise reduction qualities of horizon sampling, while handling light passing behind surfaces of constant thick- ness. We demonstrate the capabilities of our method through sev- eral SSGI applications, such as ambient occlusion, directional am- bient occlusion and indirect diffuse lighting.

**2. Previous Work** Estimating ambient occlusion and indirect diffuse lighting from screen space information is a well-known idea. It mainly stems from the seminal Screen Space Ambient Occlusion method [Mit07], approximating ambient occlusion by sampling random points in the depth buffer in a circle around each pixel and has been thoroughly extended over the years [Mit07, MOBH11, MML12]. The keypoint of these methods is to estimate the local geometry around a sample using the incomplete information contained in the various buffers (geometrical normals, depth...) while maintaining high rendering performance. Reconstruction of local geometry can be improved by gather- ing more information from the scene during the rendering passes. Reflective Shadow Maps (RSM) [DS05] approximate indirect dif- fuse lighting coming from a point light source using essentially

a G-Buffer generated from the light’s view point. This tech- nique is costly so in practice it’s usage is limited to less than a handful of light simultaneously, and doesn’t take into ac- count indirect light occlusion. Deep Screen Space [NRS14] adaptively tessellate scene geometry into an unstructured sur- fel cloud used for rendering different effects like AO, GI and more. It bypasses major screen-space limitations like hidden sur- faces and under-sampling of oblique geometry, but is expensive to compute and cannot handle indirect light occlusion between sur- fels. More recently, Stochastic-Depth Ambient Occlusion (SDAO) [VSE21] introduced the notion of stochastic depth map, captur- ing multiple scene layers per pixel at random. This technique is effective at detecting hidden surfaces, but, since it’s used in con- junction with HBAO [BSD08], it doesn’t prevent over-darkening around thin objects. However, while these methods improve re- construction of local geometry, they are more computationally ex- pensive than single-layer approaches, both during sample capture (by forgoing early-z optimization) and reconstruction (by hav- ing to evaluate multiple layers).

Improving the quality of the reconstruction from a single layer is a difficult problem that has been widely studied. Alchemy am- bient obscurance [MOBH11] is based on a similar approach than SSAO and improves robustness and artistic control, with the follow up Scalable Ambient Obscurance (SAO) [MML12] that also im- proves performance. Horizon-Based techniques [BSD08, Bav11] generates high quality results with low amount of noise by sam- pling elevation along a set of directions but causes over-darkening around thin surface. Several methods focus on improving perfor- mance, such as Line Sweep Ambient Obscurance (LSAO) [Tim13], which pre-caches sample information along azimuthal lines in GPU shared memory and reuses the same samples to shade multiple pixels. More recently, Ground Truth Ambient Occlu- sion [JWPJ16] improved the accuracy of HBAO by making it match a path-traced reference, and support a multi-bounce occlu- sion approximation.

These techniques has been derived to propose more advanced in- direct illumination features, taking advantage of the local geometry reconstruction. Silvennoinen *et al.* [ST15] added support for indi- rect lighting in real-time via an SSGI implementation using LSAO as a basis. This method is approximative because only one color sample is taken per horizon highpoint, and it doesn’t take into ac- count partial occlusion that could have occurred along the way. Other SSGI variants like Screen Space Ray Tracing Global Illu- mination (SSRTGI) [SVF17] use an SSR-like technique to sam- ple GI at the ray hit location. However, this approach introduces a lot of noise which is difficult to remove without over-blurring. An- other technique known as HBIL (which is based on HBAO and GTAO) showed how to compute GI accurately from multiple sam- ples by weighting the sample contribution by the angle difference relative to the previous sample. While this method gives accu- rate results within the visibility cone, it’s based on the assumption that the depth buffer is a height field, and it cannot take into ac- count light bounces that would pass behind surfaces.

Finally, Bitmask Soft Shadows (BMSS) [SS07] determine the visibility of an area light source with a bit field where each bit tracks the visibility of a sample point on the light source. Our method solves a slightly different problem but nonetheless shares many similarities regarding surface shape estimation from a depth map and addresses overlapping sample visibility in the same way using a bitmask.

**3. Proposed Algorithm** In this section, we present how our method propose to improve the quality of reconstruction of local geometry, especially in the case of thin surfaces, by treating the depth buffer as a set of unconnected samples each associated with an arbitrary thickness.
**3.1. Ambient Occlusion** Ambient Occlusion (AO) [ZIK98] is a non-physically based light- ing approximation of global illumination that assumes that the scene is lit by uniform ambient lighting and that all objects are oc- cluders. It’s a very common effect in real-time applications because it can be computed efficiently in screen space and adds a lot of per- ceived realism to the scenes. It can be expressed as a an estimation of the visibility function*V* in the hemisphere around each sample :
Z *AO* = 1 <u>1</u> *V* (*p;*w)(*np:* w)*d*w (1) pW By using a parameterization of the hemisphere, it can be decom- posed into : <u>1</u> Z p *AO* = 1 *AO*2(f)*d*f (2) p0 and Z p *AO*2(f) = *V* (*p;*q*;*f) cosqsinq*d*q (3) 0 In practice, the integral of equation 2 is computed using Monte Carlo integration over a few slices. Method such as GTAO and HBAO propose an analytic solution of equation 3 by estimating, through depth sampling, two horizons q1and q2. In our proposed algorithm, the two horizon angles q1and q2of GTAO are replaced by a bit field of size *Nb*, representing the binary state (occluded/un-occluded) of *Nb*visibility sectors uniformly dis- tributed around the hemisphere slice. Samples are still taken on each side of the view vector *v*, but the bit field is centered on pro- jected normal *n* :

*N* <u>1</u> *b* *AO*2(f*i*) *V*(f*i;* q*j*) (4) *Nb* *j*=1 Each sample taken along the hemisphere slice will determine a potential occluder and impact the visibility function *V* of the given sector. To determine the occluded state of a sector, we consider each sample as a local thin geometry having a thickness *t*, acting as an occluding geometry between angles q*f*and q*b*. The activation of a sector depends on the *hit criterion* which ensures sufficient overlap of these angles with the sector to get registered. q*f*is equivalent

to q used in GTAO (directly at the sample), and q*b*depends on sample thickness *t*, (figure 3). For the proposed algorithm, we used the round criterion which requires the sector to be half covered by the sample. The pseudo-code at line 15 to 17 in Algorithm 1 shows how (q*f;* q*b*) are inferred from sample position and thickness. All occluded sectors are set at once, making the algorithm perform in O(1) for any sector count. Note that we must convert the angles from cosine space to angular space for the samples to be properly distributed around the hemisphere. This enables fast directional occlusion and partial integration, at the cost of precision. In the depth buffer, we take a fixed number of azimuthal directions around each pixel and sample along these di- rections to find (q*f*,q*b*) pairs that can be integrated into the hemi- sphere slice (see Figure 5). Like GTAO we distribute the occlusion integral spatially and temporally to increase the number of effec- tive samples.

directly visible (see Figures 6 and 7). Using a fixed world-space thickness can cause an over-attenuation of occlusion for objects far away from the camera, so we give the option to increase *t* linearly over the distance to counter this effect. This causes a slight change in occlusion when the camera moves, but is barely noticeable and fixes the problem effectively. Finding an efficient heuristic to esti- mate an accurate thickness for each sample would further improve the accuracy of the method but remains a difficult problem we leave for future work. **Algorithm 1** Generate AO and GI using visibility bitmasks 1: *t* constant thickness 2: *Nb*bitmask size 3: *p* view space fragment position 4: *np* view space fragment normal 5: *r* projected radius onto image plane 6: Determine stepsize as *r=*(*Ns* + 1) 7: Determine directions with random offset 8: *AO; GI* 0 9: **for** direction *di*where *i* = 0 to *Nd***do**

|9: for direction d|iwhere i = 0 to N||ddo||
|---|---|---|---|---|
|10: t p|slice plane tangent vector in direction d|||i|
|11: tq|angle of t|p with XY-plane|||
|12: Bitmask b|i|0|||
|13: for step s|jwhere j = 0 to N||s do||
|14:|Front sample s|fview-space position at step j|p||
|15:|Back sample s|bsf|kpkt||
|16:|qf, qbangles of s|fand s|bon XY-plane||
|17:|qmin, q max|min(qf; q|b); max(qf|; qb)|
|18:|a, b bq|p +N cb, dq|p q +N e|b|
|19: 20: 21:|bj2b cjdirect lighting at step j (from GBuffer) njnormal at step j (from GBuffer)|1 a|||
|22:|lj kss|ppk|||
|23:|GI GI +|COUNTBITS(N|b & b)cj|(n p lj)(nj|
|24: 25: end for|bibij b|j|||
|26: AO 27: end for|AO + 1|COUNTBITS|(bi)=Nb||
|28: return AO=N|d, GI=N|d|||

**Figure 5:** *The hemisphere is divided into Nbuniform sectors that*

*can be either occluded (in red) or un-occluded (in green).* q*bis de-* *rived from* q*fand thickness t. Sectors that are at least half covered* *by the* (q*f;* q*b*) *pair get occluded by the sample. We assume v and n* *are aligned in this diagram for simplicity.*

The choice of *t* has a big impact on visual fidelity. GTAO with- out falloff is equivalent to using an infinite value for *t*. Ideally we would want to use the per-pixel surface thickness of objects as value for *t*, unfortunately this is more expensive to compute in real- time and impossible to know precisely using a single layer depth buffer. We propose using a small constant value since artifacts of over-occlusion around thin objects are much more noticeable than light leaks behind thick objects. This is because bigger objects usu- ally occlude completely what is behind them so a light leak can still look plausible, whereas over-occlusion around thin objects is

**Figure 6:** *Left: GTAO using horizon angles without falloff term*

*exhibits no light leaks. Middle: our method using visibility bit-* *masks with fixed thickness causes light leaks at depth disconti-* *nuities. Right: GTAO using horizon angles with falloff term also* *causes some light leaks at depth discontinuities.*

<u>min</u> <u>p</u> <u>2 max min</u> <u>p</u> <u>2</u>

<u>f</u> *f* <u>j i</u> *l* *j* ) *b*

The following sections explain the usage of the above-described core algorithm in implementing ambient occlusion, directionally occluded ambient lighting, and indirect diffuse bounce.

**3.2. Directionally Occluded Ambient Lighting** Ambient lighting in real-time applications is usually sampled using the surface normal, but tends to give poor results because it doesn’t take into account the directional occlusion of lighting. Screen space bent normal [KRES11] addresses this problem by sampling to- wards the largest non-occluded direction, but is limited to a sin- gle ambient direction per pixel and does not handle thickness prop- erly. Visibility bitmasks can improve this by weighting the ambient lighting in a given direction by the directional occlusion while al- lowing light to pass behind surfaces. To this end, we divide the

**Figure 7:** *From left to right: Ray tracing reference, GTAO without falloff, GTAO with falloff, visibiliy bitmask using a thickness of 1, 0.5,*

*0.25, 0.1. All methods use a radius of 2.* hemisphere into as many subregions as the number of ambient sam- ples, generating a sampling direction vector at the center of each subregion. The ambient source is then sampled with this vector and multiplied the lighting intensity by the amount of un-occluded sec- tors over the total sector count (see Figure 8). It made the ambient light color vary smoothly according to changes in occlusion direc- tionality.
**Figure 9:** *Left: The yellow sample intersects one un-occluded sec-*

*tor and can contribute lighting equivalent to one over the total* *number of visibility sectors. The sector is set to an occluded state* *for subsequent samples. Right: Sampling continues and a new ob-* *ject on the right is found, but it intersects an already occluded sec-* *tor, so it cannot contribute lighting. The yellow sample on the left* *crosses an un-occluded sector and can contribute.*

**Figure 8:** *In this example, 4 ambient samples per slice per pixel*

*are taken so the hemisphere is divided into 4 subregions. Vectors a*1*,* *a* *, a*3*, a*4*are generated at the center of their respective subregion.* 2

**3.3. Indirect Diffuse** Indirect diffuse lighting is the bouncing of light on nearby surfaces. It’s traditionally expensive to compute accurately, even in screen space. HBIL can do it efficiently but fails to account for light pass- ing behind surfaces. Figure 9 shows how we computed this effect with better thickness handling using visibility bitmasks. Samples were taken along the slice direction and detected (in O(1)) how many un-occluded sectors are covered to estimate lighting contri- bution. These sectors were then set to an occluded state (also in O(1)) to handle partial or total occlusion of light coming from sub- sequent samples. The more the visibility sectors, the more precise the estimation of lighting and occlusion. It was observed that 32 sectors gave good quality and makes the bit field fit nicely within a single unsigned integer.
The pseudo-code at line 23 in Algorithm 1 shows how the light- ing contribution of a sample is implemented, using the number of occluded zones by the current sample. If one or more sectors are covered, the sample contributed lighting and the light buffer is sampled at the sample location. Then *n l* and *nll* are com- puted and used for weighting light intensity. The light is further- more weighted by the occluded sector count over the total sector count.

**4. Results and evaluation**
**4.1. Ambient Occlusion** In this subsection, the core algorithm will be demonstrated con- jointly with AO, as it’s much easier to discern the properties of vis- ibility bitmasks in this mode than using indirect diffuse. The ren- ders in Figure 10 are produced by our extension of Unity’s GTAO implementation, where the two horizon angles q and q have been replaced by a single visibility bitmask.

**Figure 10:** *Comparison of GTAO, our method based on visibility bitmasks, and a ray-traced AO reference at different radius and sample*

*count values.*

It highlights how easy it is to implement our method on top of an existing horizon-based technique, and how this single modification can dramatically enhance visual quality compared to a ray-traced reference. The Lumberyard Bistro scene has been chosen because it contains a lot of thin and shallow surfaces that are typically a prob- lem with horizon-based techniques, but are improved by visibility bitmasks. All benchmarks use one hemisphere slices per pixel jit- tered over multiple frames. The radius parameter (Figure 10) is the radius of the hemisphere

**Figure 11:** *Left: GTAO exhibit too much occlusion behind the bars.*

aligned to the screen in world units. Wider hemispheres will cover *Right: Our method is able let light pass behind the bars, minimizing* wider regions of the screen, casting farther-reaching occlusion. A *the over-occlusion artifact.* wide radius is problematic for GTAO because the single cone ap- proximation tends to cast too much occlusion in regions enclosed by thin objects. Even around not-so-thin objects, a wide radius tends to produce a blurry occlusion blob that does not capture fine trapped behind the bars instead of passing by, causing a lot of over- detail. In contrast, our method (with the exact same samples) is able occlusion. Our method is able to handle this situation much better. to let light pass behind surfaces, avoiding over-occlusion and cap-When using a wide radius, GTAO has a tendency to produce ha- turing a lot of small details. los around objects (Figure 4). Our method doesn’t have this prob- lem, and capture more geometric and normal details.

Figure 11 demonstrates an even more difficult case for GTAO

where most of the objects are behind a fence. The light gets The number of samples (Figure 10) indicates the number of

count the fact that some light could be occluded in some direction and tend to make ambient lighting change sharply with the scene geometry. A better approach is to use a screen space bent normal per pixel that is modulated according to nearby occlusion. It points towards the direction of incoming light and gives a smoother re- sult. However, it cannot handle multiple light directions and will misrepresent the ambient lighting of surfaces enclosed by thin ob- jects.

fetches taken in the depth buffer along one horizon side. There- fore, for one slice, the actual number of samples taken is twice that number. A wider radius causes the samples to be sparser on-screen, so it’s typical to increase sample count for a wider radius to main- tain the same sampling density. A low sampling density increases the likelihood of missing potential occluders, especially if they are thin on-screen. Increasing the number of samples has a big impact on performance, so it’s a tradeoff. Performance depends primarily on the number of samples taken and the radius of the effect. A wider radius increases the proba- bility of occurrence of a cache miss (sample not being present in the cache) and consequently can lower performance. The execu- tion time of the technique tends to scale linearly with the number of samples. Table 1 compares the performance of the horizon-based GTAO implementation versus our method using visibility bitmasks. Both techniques are composed of a sampling pass and a denoising pass. Only the results of the sampling pass are included in the ta- ble since it’s the only one impacted by our method. The denoising pass has a constant cost of 0.3 ms in 1080p. It can be observed that our method has a modest impact on performance around 0.01-0.02 milliseconds, with a fixed ALU overhead of about 15 GPU instruc- tions per sample. Increasing the radius masks this overhead as the execution becomes bandwidth-limited. Radius Sample Count GTAO Our Method

0.8 8 0.49 ms 0.51 ms 1 12 0.75 ms 0.77 ms 1 16 0.95 ms 0.97 ms 2 16 1.12 ms 1.13 ms 3 16 1.12 ms 1.13 ms
**Table 1:** *Render time of the sampling pass for GTAO and our*

*method with various radius and sample parameters, at 1920x1080,* *with 32 visibility sectors per hemisphere slice. Benchmarks are* *done on an Nvidia RTX 2080 GPU.*

Another parameter that impacts image quality is the number of visibility sectors. When the sector count is too low, banding arti- facts can appear, particularly around thin objects. The proposed im- plementation used 32 visibility sectors because it just crossed the threshold where the artifacts became almost invisible. Additionally, it nicely fits into a single unsigned integer which gives good per- formance on the GPU. By contrast, a 128 bits version require four unsigned integers and the use of vector instructions, which lim- its the amount of instruction packing that the compiler could do, negatively impacting the performance. A performance overhead of around 5-10% was observed with 128 visibility sectors compared to 32.

**4.2. Directionally Occluded Ambient Lighting** In this subsection, we compare different ambient sampling strate- gies and show how they can dramatically influence the resulting lighting and occlusion. Figure 12 shows the average normal on the left, and the resulting ambient lighting on the right for each strat- egy. In most 3D applications, ambient lighting is sampled in the di- rection of the surface normal. This approach does not take into ac-
**Figure 12:** *Comparison of ambient lighting using the surface nor-*

*mal, bent normals, and visibility bitmasks. The average sampling* *normal direction is shown on the left to make the difference more* *visible. Top row: G-Buffer normals. Middle row: Bent normals.* *Bottom row: visibility bitmask.*

Our approach used visibility bitmasks to take multiple samples along each hemisphere slice in the directions that were not oc- cluded. Doing so allow ambient light to pass behind surfaces, giv- ing smooth lighting from multiple directions. It’s also worth noting that each ambient sample is weighted according to the occlusion in that specific direction. If the ambient color had been simply av- eraged out and then multiplied by ambient occlusion, a lot of the color variation in the lighting would have been lost.

**4.3. Indirect Diffuse** In this subsection, we look at the visual quality and performance of the indirect diffuse portion of the algorithm. In addition to sam- pling the depth buffer, we also sample the HDR light buffer and the screen space normal buffer for every sample taken. The light buffer contains only the direct lighting (with shadows), as the am- bient light is computed by our method. Figure 13 compares our result with a path tracing reference for single and multi-bounce in- direct diffuse lighting. The direct lighting coming from the sun on the left wall bounces on the brick wall, illuminating it and cast- ing indirect shadows. With multiple bounces, the light is even able to bounce back on the left wall, illuminating a shadowed region of

**Figure 13:** *Comparison of indirect diffuse algorithm and path tracing reference with single and multi-bounce indirect diffuse.*

the wall. Multiple bounces are implemented by injecting the indi- rect illumination into the light buffer to be used as input for the next frame. Light intensity needs to be properly balanced when using multi-bounce, or it can cause a feedback loop resulting in lighting accumulation over time. The result cannot match perfectly the path-traced reference since the algorithm operates only on the screen pixels as opposed to the entire scene geometry. One ma- jor drawback of our technique, along with screen-space methods, is that if direct light leaves the screen, the indirect lighting disap- pears.

Low sample density can also cause a loss of detail around small objects. To improve this, we distribute the samples exponentially around the shaded pixel, as nearby surfaces usually have more in- fluence on the result than farther ones.

Figure 16 compares the amount of noise incurred when sampling

the scene with SSR-like tracing and our method based on visibil- ity bitmasks. Both techniques are taking the same maximum num- ber of samples but the visibility bitmask approach has a lot less noise. Rays in SSR are doing at most one accumulation operation (when a hit is found). In contrast, the visibility bitmask approach is accumulating each sample that is visible from the current pixel. This algorithm is bandwidth-intensive because we need to sam- ple the HDR light buffer and the screen space normal buffer for every sample taken. Those sample locations are not correlated and impair caching. Table 2 compares the performances of the indirect diffuse part of the algorithm with different sampling parameters. The corresponding renders are shown in Figure 15. Full resolution means that the shader is executed for every pixel of the final ren- der resolution, whereas half-resolution executes it on a screen that is half the size (a quarter the number of pixels). The image is then upscaled with a classic bilateral upsampler to avoid aliasing. Ren- dering in half resolution is much more efficient (around 4x), but can introduce more flickering in the image and a blurrier result.

**5. Conclusion and future work** Previous screen space GI methods that rely on HBAO cannot han- dle thickness because of the assumption that the depth buffer is strictly a height field. Other techniques based on SSR tracing squander a lot of rays that end up passing behind and over sur- faces, exiting the screen without hitting anything, thereby introduc-
**Figure 14:** *When samples are evenly spaced along the sampling di-*

*rection, a banding pattern can appear. Jittering the samples along* *the sampling direction helps mask the banding artifact.*

The number of samples taken has a big impact on quality. Hav- ing a too low sampling density can introduce banding artifacts. To mitigate the issue, samples are jittered along the sampling direc- tion, as shown in Figure 14.

**Figure 15:** *Resulting renders using different sample, radius, stepping, and resolution parameters for the indirect diffuse lighting. The param-*

*eters of each figure are indicated in Table 2.*

|Configuration|Sampling|Denoising|Total|
|---|---|---|---|
|a) 8 samples, radius 1, const. steps, full res.|0.9 ms|0.33 ms|1.23 ms|
|b) 8 samples, radius 4, const. steps, full res.|1.7 ms|0.33 ms|2.03 ms|
|c) 16 samples, radius 4, const. steps, full res.|2.3 ms|0.33 ms|2.63 ms|
|d) 16 samples, radius 4, exp. steps, full res.|2.6 ms|0.33 ms|2.93 ms|
|e) 32 samples, radius 4, exp. steps, full res.|4.0 ms|0.33 ms|4.33 ms|
|f) 16 samples, radius 4, exp. steps, half res.|0.97 ms|0.1 ms|1.07 ms|

**Figure 16:** *Left: 2 rays per pixel SSR tracing, with 16 steps per*

*ray. There is a lot of noise, as a lot of rays exit the screen or pass* *behind surfaces without hitting anything. Right: Horizon search on* *both sides of the pixel using a visibility bitmask with 16 steps per* *horizon is a lot less noisy.*

**Table 2:** *Render time at 1920x1080, with a constant thickness value*

*of 0.2.* ing a lot of noise. In contrast, our method combined the sampling efficiency of horizon-based methods, while retaining the capabil- ity of handling thickness properly along the way. The proposed al- gorithm is easy to understand and implement on modern GPUs and constant thickness value that optionally increases linearly over the can be integrated into any horizon-based technique with only a mild distance. A thickness heuristic that would give a plausible thick- performance overhead. Moreover, this method can also be used to ness per pixel would help improve the occlusion leaks around some improve ambient light sampling by taking into account the direc- very thin objects or light leaks behind very thick ones and would tionality of occlusion when integrating ambient light. be interesting to explore in future work. At the performance level, Even though the ray-tracing part of the algorithm handles thick-SSGI can be an expensive algorithm because it is very demanding ness properly, since we operate on a single layer of the depth buffer, on GPU bandwidth and utilizes the cache poorly. Using a caching the actual object thickness is unknown. We are forced to rely on a method similar to LSAO could potentially improve this. Finally,

the common ambient light sources are either static (constant color, [SVF17]SHERGIN D., VIDIGER D., FOFANOVA A.: Superposition light probes) or expensive to update at runtime. We might investi- benchmark: innovative ssrtgi lighting in real time. In *ACM SIGGRAPH* gate in the future ways to approximate low-frequency ambient irra- *2017 Real Time Live!* 2017, pp. 25–25. diance dynamically. [Tim13]TIMONEN V.: Line-sweep ambient obscurance. In *Computer* *graphics forum* (2013), vol. 32, Wiley Online Library, pp. 97–105. [VSE21]VERMEER J., SCANDOLO L., EISEMANN E.: Stochastic-depth

**6. Acknowledgments and data statements** ambient occlusion. *Proceedings of the ACM on Computer Graphics and*
*Interactive Techniques 4*, 1 (2021), 1–15. Special thanks to Deepti Joshi (CDRIN) and Peter Shirley [ZIK98]ZHUKOV S., IONES A., KRONIN G.: An ambient light illumina- (NVIDIA) for their guidance in the redaction of this paper. We tion model. In *Eurographics Workshop on Rendering Techniques* (1998), also want to thank our other colleagues namely Antoine Fortin Springer, pp. 45–55. (CDRIN), Olivier Leclerc (CDRIN), Steven Pigeon (UQAR), and Vahe Vardanyan (CDRIN) who have actively supported the current body of work. Most of the models are from the Amazon Lumber- yard Bistro scene. This research is financed in part by the province of Quebec (Canada) via the grant "Programme d’aide à la recherche et au transfert (PART)". Data sharing not applicable to this article as no datasets were generated or analysed during the current study.

**References** [Bav11]BAVOIL L.: Horizon-based ambient occlusion using compute shaders. *Nvidia DirectX 11* (2011). [BSD08]BAVOIL L., SAINZ M., DIMITROV R.: Image-space horizon- based ambient occlusion. In *ACM SIGGRAPH 2008 talks*. 2008, pp. 1–1. [DS05]DACHSBACHER C., STAMMINGER M.: Reflective shadow maps. In *Proceedings of the 2005 symposium on Interactive 3D graphics and* *games* (2005), pp. 203–231. [HBSS17]HOFMANN N., BOGENDÖRFER P., STAMMINGER M., SEL- GRAD K.: Hierarchical multi-layer screen-space ray tracing. In *Proceed-* *ings of High Performance Graphics*. 2017, pp. 1–10. [JWPJ16]JIMÉNEZ J., WU X., PESCE A., JARABO A.: Practical real-time strategies for accurate indirect occlusion. *SIGGRAPH 2016* *Courses: Physically Based Shading in Theory and Practice* (2016). [KRES11]KLEHM O., RITSCHEL T., EISEMANN E., SEIDEL H.-P.: Bent normals and cones in screen-space. In *VMV* (2011), Citeseer, pp. 177–182. [May18]MAYAUX B.: Horizon-based indirect lighting. 2018. [Mit07]MITTRING M.: Finding next gen: Cryengine 2. In *ACM SIG-* *GRAPH 2007 courses*. 2007, pp. 97–121. [MML12]MCGUIRE M., MARA M., LUEBKE D. P.: Scalable ambient obscurance. In *High Performance Graphics* (2012), Citeseer, pp. 97–

103.
[MMNL16]MARA M., MCGUIRE M., NOWROUZEZAHRAI D., LUE- BKE D. P.: Deep g-buffers for stable global illumination approximation. In *High Performance Graphics* (2016), pp. 87–98. [MOBH11]MCGUIRE M., OSMAN B., BUKOWSKI M., HENNESSY P.: The alchemy screen-space ambient obscurance algorithm. In *Proceed-* *ings of the ACM SIGGRAPH Symposium on High Performance Graphics* (2011), pp. 25–32. [NRS14]NALBACH O., RITSCHEL T., SEIDEL H.-P.: Deep screen space. In *Proceedings of the 18th meeting of the ACM SIGGRAPH Sym-* *posium on Interactive 3D Graphics and Games* (2014), pp. 79–86. [SKS11]SOUSA T., KASYAN N., SCHULZ N.: Secrets of cryengine 3 graphics technology. In *ACM SIGGRAPH* (2011), vol. 1. [SS07]SCHWARZ M., STAMMINGER M.: Bitmask soft shadows. In *Computer Graphics Forum* (2007), vol. 26, Wiley Online Library, pp. 515–524. [ST15]SILVENNOINEN A., TIMONEN V.: Multi-scale global illumina- tion in quantum break. In *ACM SIGGRAPH* (2015).

