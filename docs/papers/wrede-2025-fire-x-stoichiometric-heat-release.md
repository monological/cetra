<!--
Wrede, Wagner, Mahfuz, Pałubicki, Michels & Pirk, "Fire-X: Extinguishing Fire with Stoichiometric
Heat Release", ACM Transactions on Graphics 44(6), Article 268 (SIGGRAPH Asia 2025).
https://doi.org/10.1145/3763338

Licensed under Creative Commons Attribution 4.0 International
(https://creativecommons.org/licenses/by/4.0/), as the paper's own first page states.
Copyright held by the authors.

Converted from the authors' PDF
(https://helgewrede.github.io/firex/data/FireXExtinguishingFireWithStoichiometricHeatRelease.pdf)
with pdf2md, fetched 2026-10-01. Changed from the original: converted to markdown, so the figures
are gone and the equations arrive as fragments. Go back to the PDF for anything load-bearing.
-->

# Fire-X: Extinguishing Fire with Stoichiometric Heat Release

## HELGE WREDE, Kiel University, Germany ANTON R. WAGNER, Kiel University, Germany

## SARKER MIRAZ MAHFUZ, Kiel University, Germany WOJTEK PAŁUBICKI, Adam Mickiewicz University, Poland

## DOMINIK L. MICHELS, KAUST, KSA SÖREN PIRK, Kiel University, Germany

Fig. 1. A complex scene of fire and water interactions generated with our framework. A large fire affecting three cars is extinguished with a water jet, causing

large amounts of smoke and vapor. Our combustion model enables simulating multi-species thermodynamics and the extinction of flames.

We present a novel combustion simulation framework to model fire phe- nomena across solids, liquids, and gases. Our approach extends traditional fluid solvers by incorporating multi-species thermodynamics and reactive transport for fuel, oxygen, nitrogen, carbon dioxide, water vapor, and resid- uals. Combustion reactions are governed by stoichiometry-dependent heat release, allowing an accurate simulation of premixed and diffusive flames

Authors’ Contact Information: Helge Wrede, helge.wrede@email.uni-kiel.de, Kiel Uni- versity, Christian-Albrechts-Platz 4, 24118 Kiel, Germany; Anton R. Wagner, awa@ informatik.uni-kiel.de, Kiel University, Christian-Albrechts-Platz 4, 24118 Kiel, Ger- many; Sarker Miraz Mahfuz, smm@informatik.uni-kiel.de, Kiel University, Christian- Albrechts-Platz 4, 24118 Kiel, Germany; Wojtek Pałubicki, wojciech.palubicki@amu. edu.pl, Adam Mickiewicz University, 61-614, Poznań, Poland; Dominik L. Michels, dominik.michels@kaust.edu.sa, KAUST, Thuwal 23955, KSA; Sören Pirk, soeren.pirk@ gmail.com, Kiel University, 24118 Kiel, Germany.

with varying intensity and composition. We support a wide range of sce- narios including jet fires, water suppression (sprays and sprinklers), fuel evaporation, and starvation conditions. Our framework enables interactive heat sources, fire detectors, and realistic rendering of flames (e.g., laminar-to- turbulent transitions and blue-to-orange color shifts). Our key contributions include the tight coupling of species dynamics with thermodynamic feed- back, evaporation modeling, and a hybrid SPH-grid representation for the efficient simulation of extinguishing fires. We validate our method through numerous experiments that demonstrate its versatility in both indoor and outdoor fire scenarios.

CCS Concepts: • Computing methodologies → Physical simulation; Interactive simulation.

Additional Key Words and Phrases: Combustion, Evaporation, Extinguishing, Fire Simulation, Fluid Simulation, Heat Release, Stoichiometry.

ACM Reference Format: Helge Wrede, Anton R. Wagner, Sarker Miraz Mahfuz, Wojtek Pałubicki, Dominik L. Michels, and Sören Pirk. 2025. Fire-X: Extinguishing Fire with Stoichiometric Heat Release. *ACM Trans. Graph.*44, 6, Article 268 (Decem- ber 2025), 17 pages. [https://doi.org/10.1145/3763338](https://doi.org/10.1145/3763338)

ACM Trans. Graph., Vol. 44, No. 6, Article 268. Publication date: December 2025.

This work is licensed under a Creative Commons Attribution 4.0 International License. © 2025 Copyright held by the owner/author(s). ACM 1557-7368/2025/12-ART268 [https://doi.org/10.1145/3763338](https://doi.org/10.1145/3763338)

268:2 • Wrede, et al.

1 Introduction Simulating combustion in computer graphics poses substantial chal- lenges due to the interplay of fluid dynamics, thermodynamics, and chemical kinetics across multiple phases – solids, liquids, and gases. Modeling combustion and fire effects is critical in many do- mains: from visual storytelling in games and films [Nielsen et al. 2019], to safety training [Kinateder et al. 2014], fire suppression planning [Cao et al. 2020], and wildfire research [Hädrich et al. 2021; Kokosza et al. 2024]. Furthermore, modeling different extin- guishing behaviors is important for capturing the wide range of fire-suppression phenomena encountered in real-world scenarios. Whether it is a sprinkler diffusing fine water droplets, a fire extin- guisher dispersing foam or gas, or a direct hose stream dousing flames, each method interacts with combustion in distinct thermo- and fluid-dynamic ways. Visually differentiating between extin- guishing mechanisms – such as steam plumes from evaporation or darkening flames due to oxygen deprivation – adds to the visual fidelity and immersion to virtual scenes. While computer graphics has made significant progress in flame rendering and fluid animation [Kim et al. 2008; Nguyen et al. 2002; Stam 1999], most existing techniques either simplify combustion chemistry or focus solely on gaseous flames, ignoring phase tran- sitions, multi-fuel reactions, or water-based suppression. On the other hand, physics-based approaches for fire provide high-fidelity models but are designed for predictive simulation and not for in- teractive applications [Merci and Beji 2022; Nielsen et al. 2022]. Other approaches focus on simulating water [Clavet et al. 2005] also undergoing temperature changes [Mihalef et al. 2006], the interac- tion of multiple fluids [Losasso et al. 2006a], and the melting and burning of solids into liquids and gases [Losasso et al. 2006b]. The coupling of reactive multi-phase flow, with varying fuel composi- tions and evaporation, remains largely unaddressed in an interactive or controllable setting. In this work, we present a physically-based combustion model which addresses the simulation of multiple species at interactive rates. Compared to other combustion models that focus on specific aspects of combustion phenomena, our method trades descriptive complexity for a more integrated combustion simulation. We show that combustion across solids, liquids, and gases can be unified within an efficient hybrid framework that couples Eulerian grids for thermochemical fields with Lagrangian smoothed particle hydrody- namics (SPH) for fluid motion and droplet dynamics. This enables the simulation of a broad range of scenarios, such as premixed flames, water sprays, candle and jet fires, and suppression via sprin- klers and direct water jets. Unlike previous models, our framework explicitly tracks major species – fuel, oxygen, carbon dioxide, nitro- gen, water vapor, and residuals – allowing stoichiometry-dependent heat release, oxygen starvation, and phase transition effects such as evaporation. Our approach extends the state-of-the-art of simulat- ing combustion by modeling complete and incomplete combustion, along with visual cues such as flame color shifts and smoke as well as vapor transitions. Moreover, our solver efficiently couples chemical kinetics and thermodynamics with a particle-grid representation to

### maintain high accuracy with interactive runtime performance. Addi-

tionally, we render realistic flame and smoke behaviors, modulated by environmental conditions and user-defined parameters. In Fig. 1, we show a complex simulation showcasing the dynamic interaction between fire and water. Three vehicles are engulfed in intense flames which are being extinguished by a high-pressure water jet. The interaction of fire and water results in large smoke and vapor clouds. Our novel combustion model captures multi-species thermodynamics, allowing realistic visualization of flame extinction and complex thermal behaviors in reactive flows. In summary, our contributions are (1) a hybrid combustion simu- lation framework combining SPH with an Eulerian thermochemical grid, (2) a stoichiometry-aware reaction and heat release model spanning multiple combustion species, (3) a coupled liquid-gas-solid model for extinguishing dynamics, and (4) an interactive param- eter space for rendering and controlling fire behaviors in various scenarios.

2 Related Work Simulating fire and combustion has been a long-standing research topic in both computer graphics and physics-based modeling. Semi- nal books in combustion modeling and fluid dynamics include Merci and Beji [2016], Kross and Potter [2014], Peters [2000], as well as Bridson’s introduction to fluid solvers [Bridson 2015]. Fluid motion in most fire models is described by the incompressible Navier-Stokes equations, often discretized using semi-Lagrangian advection and pressure projection [Versteeg and Malalasekera 2007]. In computer graphics, fire is typically modeled using computa- tional fluid dynamics (CFD) and grid-based fluid solvers [Bridson and Müller-Fischer 2007], which effectively capture both laminar and turbulent flame behaviors [Hong et al. 2007; Nguyen et al. 2002; Stam 1999], as well as smoke dynamics [Fedkiw et al. 2001; Pan and Manocha 2017]. Various approaches have been developed for the vi- sual modeling of fire, including physically-based methods [Nguyen et al. 2002; Nielsen et al. 2022, 2019; Pegoraro and Parker 2006], tech- niques that emphasize the physical properties of flames [Nguyen et al. 2001], methods that prioritize artistic control [Aguilera and Johansson 2019; Kim et al. 2017; Lamorlette and Foster 2002], and models that rely on particle-based representations [Horvath and Geiger 2009]. Futhermore, vorticity confinement [Bridson 2015] is a widely used method to enhance the visual fidelity of flames. In physics-based fire simulations, the modeling process aims to accurately replicate the underlying thermodynamic and chemical phenomena governing combustion. These simulations include de- tailed representations of fluid motion, heat transfer, and combustion reactions, incorporating processes such as convection, conduction, radiation, and finite-rate chemical kinetics [Merci and Beji 2016]. Radiative heat transfer is commonly handled through models based on the Stefan-Boltzmann law, with some approaches using ray cast- ing techniques to approximate radiative heating of the surrounding fuel [Stam and Fiume 1995]. By integrating these physical princi- ples, such models can produce realistic flame behavior and energy distribution, making them suitable for high-fidelity applications in both visual effects and scientific visualization [Nielsen et al. 2022, 2019; Pegoraro and Parker 2006]. Chemical reactions in fire are often

Fire-X: Extinguishing Fire with Stoichiometric Heat Release • 268:3

**Staggered Grid Particle from Grid** **Mesh to Particles Particle Kernel Grid from Particle**

x x x **Particles** x x x x x x x x x **Mesh** x x x x x x

|(a)|(b)|(c)|(d)|(e)|
|---|---|---|---|---|
|Solids|Liquids|Update Grid|Gases|Update Particles|
|(Particles)|(Particles)|from Particles|(Grid)|from Grid|

**Rendering**

**Simulation Step**

Fig. 2. Overview of our multiphase framework: our liquid-gas-solid model enables simulating the thermodynamics of solids, liquids and gases. We use a

Lagrangian representation for solids (a) and liquids (b) and an Eulerian representation for gases (d). To maintain all states of matter, we synchronize solid and fluid particles with the gas grid in two update steps (c, e).

modeled using modified Arrhenius equations as formalized in the IUPAC Gold Book [1997], while the heat of combustion is computed following thermochemical approaches [Schmidt-Rohr 2011]. More extreme combustion phenomena, such as explosions and detonations, require compressible flow models [Ihm et al. 2004]. Kwatra et al. [2009] proposed a method that dynamically tran- sitions between compressible and incompressible regimes, mak- ing it suitable for simulating shock waves and pressure-driven deflagrations in graphics contexts. Particle-based explosion mod- els, such as that of Feldman et al. [2003], combine Eulerian air simulation with Lagrangian fuel particles and approximate heat transfer. Advection and diffusion are typically handled using semi- Lagrangian schemes [Stam 1999], while pressure projection and multigrid solvers are used for velocity field correction. Tools like AMGCL [2019] and PyBullet [2021] as well as methods for fast fluid simulations [Rabbani et al. 2022] support scalable fluid-rigid interaction and large-scale solver efficiency. Sparse volumetric data structures such as OpenVDB [2013] facilitate the efficient handling of complex simulation domains at high resolution. While we do not focus on simulating the combustion of solids, several approaches leverage volumetric grids to enable the tracking of disconnected, propagating fire fronts [Liu et al. 2012, 2009; Melek and Keyser 2002; Zhao et al. 2003] and support fire spreading across surfaces [Chiba et al. 1994]. Hong et al. [2010] present a method ca- pable of modeling combustion under complex geometric constraints, while Pirk et al. [2017] focus on the combustion of geometrically complex plant models. Stomakhin et al. [2014] introduce a point- based method for simulating heat transfer to capture the melting and solidification of materials. Their approach models the thermo- dynamic and mechanical behavior of various substances but lacks support for real-time interaction and heat diffusion. More recently, Liu et al. [2025] use signed distance fields to support the combustion of general wooden structures. Their approach enables to efficiently query the surface information required to compute the insulating effect caused by the char layer. Physically-based fire simulators have been developed to model the complex interactions between combustion, fluid flow, and heat trans- fer, particularly in engineering and safety-critical environments. The Fire Dynamics Simulator (FDS), is a widely used computational fluid dynamics tool for simulating fire-driven flows in buildings and en- closures [McGrattan et al. 2013]. It incorporates low-Mach number

Navier-Stokes equations, radiation transport, combustion models, and detailed treatment of smoke and heat propagation. Several meth- ods aim at simulating combustion for wildfire scenarios, including WFDS [Mell et al. 2007], WRF-Fire [Coen et al. 2013], CAWFE [Coen 2013], ABWiSE [Katan and Perez 2021], FIRETEC [Linn et al. 2002], OpenFOAM[Lapointe et al. 2020], and QUIC-Fire [Linn et al. 2020]. Bakhshaii and Johnson [2019] offer a comprehensive review of modeling strategies across empirical, physical, and hybrid domains, highlighting the trade-offs between real-time capability and physical accuracy. Unlike the existing methods, which primarily focus on either high- fidelity offline simulation or artist-driven visual effects, we introduce a method that allows the interactive simulation of extinguishing behaviors of flames. Furthermore, we devise a principled physics model that can simulate stoichiometric reactions that accurately capture heat release across multiple combustion regimes, including diffusion and premixed flames.

3 Overview Our hybrid multiphase fluid model is expressed in two different spatial domains. We use a continuous Cartesian coordinate system and a discrete uniform grid composed of voxels to capture gas, liquid, and solid interactions relevant for combustion scenarios (see Fig. 2). The discrete simulation domain represents either gaseous or solid regions, using a binary occupancy flag. SPH particles representing liquids are embedded in the continuous spatial domain and are synchronized with the grid simulation. At initialization, the solid geometry usually defined as meshes is encoded as a dense point cloud (solids, Fig. 2a). Each point populates a corresponding grid cell, which is flagged as solid and used to enforce boundary conditions for velocity, temperature, and species transport during simulation. A user can specify emitters to generate SPH particles for fluids. Particles contribute mass, temperature, and velocity to the underlying grid using conservative sampling schemes (liquids, Fig. 2b, c). The particles explicitly represent the liquid phase (e.g., water or ethanol) and model its behavior using SPH forces to capture realistic fluid dynamics. Our SPH simulation describes surface tension, pressure projection, and collision using a double density relaxation scheme. Each gaseous grid cell stores thermodynamic quantities including velocity, density, temperature, and a vector of chemical species mass fractions *𝑌𝑖* (Fig. 3).

268:4 • Wrede, et al.

Our grid solver describes diffusion, pressure projection, liquid evaporation, advection, buoyancy, combustion, vorticity confine- ment, thermal properties, density, and species transport. Gas density is dynamically updated from local temperature and species com- position using a temperature-dependent ideal gas law formulation describing chemical species evolution. This allows our model to express stochiometric combustion over a range of different gas mix- tures (gases, Fig. 2d). Solid cells contribute to heat conduction and boundary enforce- ment; liquid particles exchange mass and energy with the gas phase through heat-driven evaporation (Fig. 2e). Particles whose lifetime has expired (e.g., due to complete evaporation) are removed. This two-way coupling handles multiphase interfaces and phenomena such as heat conduction from solid walls into fluid, or energy-driven phase transitions between liquid and vapor. Our evaporation model includes both continuous heat-based mass loss and discrete droplet- based evaporation tied to local energy budgets (Fig. 3). Our approach supports modeling of thermodynamic processes including conduction, and chemical kinetics using modified Arrhe- nius equations, and supports grid-based combustion dynamics such as flame propagation, soot production, and volumetric expansion due to temperature increase. The model is designed to accommo- date both physical approximations of deflagration scenarios and artist-tuned stylizations, depending on the application domain.

4 Methodology In this section, we present our multiphase combustion model. We begin by outlining the equations for fluid motion, thermodynamics,

||= 𝐷 ∇2𝑌 + 𝑣𝑐𝑞𝑖 𝑀𝑖 + 𝑆|,|
|---|---|---|
||𝑖 𝑖|𝑖|
|𝑖 𝑖||𝑖|

### and species transport, including extensions for combustion-driven

buoyancy and heat generation. Finally, we detail the evaporation model, which describes liquid–gas phase transitions based on ther- mal energy balance and droplet-level resolution. A list of parameters is included in Appendix A.1.

4.1 General The simulation domain defines velocity, temperature, species frac- tions, and material states, described by coupled PDEs for fluid dy- namics, thermodynamics, and species transport as illustrated in
Fig. 3. To model natural convection (i.e., buoyancy-driven flow), the
 incompressible Navier-Stokes equation is adapted to ideal gas flows using the Boussinesq approximation and hydrostatic pressure is neglected. We obtain the following momentum equation:
### 𝐷𝒖21 𝑇amb

= *𝜈*∇ *𝒖* − ∇*𝑝* − *𝒈* 1 −*.* (1) *𝐷𝑡 𝜌 𝑇* Here, *𝒖* denotes the velocity field, *𝜈* the kinematic viscosity, *𝜌* the fluid’s local density, *𝑝* pressure, *𝑇* temperature, and *𝑇*ambthe ref- erence ambient temperature. The right term with effective gravity *𝒈* introduces buoyancy based on temperature difference, enabling plume rise and convection effects critical for combustion-driven flows. Thermal transport is governed by a modified heat equation which has been extended with radiative cooling and a combustion energy source term. We obtain *𝐷𝑇 𝑘*2*𝜖𝜎*4 4*𝐽𝑐* = ∇ *𝑇* + (*𝑇* amb − *𝑇*)+*,* (2) *𝐷𝑡 𝜌𝐶𝑝 𝐶𝑝 𝜌𝐶𝑝*

Fig. 3. Schematic representation of the multiphysics model, illustrating

the interdependent processes governing fire plume dynamics. The model couples natural convection, thermal transport, chemical species evolution, evaporation, and combustion.

in which*𝑘* denotes thermal conductivity,*𝐶𝑝* specific heat at constant pressure, *𝜖* emissivity, and *𝜎* the Stefan-Boltzmann constant. The radiative term accounts for the net exchange of energy with the environment. The source term *𝐽𝑐* (see Eq. 6) incorporates the local heat release rate from combustion. The evolution of chemical species is handled per-species, i.e.,

<u>𝐷𝑌𝑖</u>

(3)
### 𝐷𝑡 𝜌

in which *𝑌* denotes mass fraction of species *𝑖*, *𝐷* diffusion coeffi- cient, and *𝑆* represents the source term from the phase change.

4.2 Combustion We model combustion using a simplified global one-step reaction mechanism. The heat of combustion is approximated from the em- pirical formula of the fuel:
Δ*𝑐𝐻𝑜* ≈−417(*𝑐* + 0*.*25*ℎ* − 0*.*5*𝑜*)*.* (4) −1 This estimates the lower heating value (measured in kJ mol) for hydrocarbon fuels of composition *𝐶𝑐 𝐻ℎ𝑂𝑜 𝑁𝑛* [Schmidt-Rohr 2015]. The combustion rate follows an Arrhenius-type expression

### 𝑣𝑐 = 𝐴𝑇 𝑛 exp

<u>−𝐸𝑎</u> *𝑐𝑎 𝑐𝑏,* (5) *𝑅𝑇𝑓* *𝑜*

in which *𝑣𝑐* denotes the volumetric reaction rate,*𝑇* temperature, *𝑅* universal gas constant, *𝐴* pre-exponential factor, and *𝐸𝑎* activation energy. The molar concentrations of fuel and oxidizer *𝑐𝑓* and *𝑐𝑜* are raised to empirical exponents *𝑎* and *𝑏*. The resulting combustion energy rate per unit volume is given by 0 *𝐽* *𝑐*= −*𝜑𝑣𝑐*Δ*𝑐𝐻* (6)

which occurs in Eq. 2 feeding back the heat release into the thermal equation. We use *𝜑* as a weighting factor for adjusting the heat efficiency of the combustion (e.g., in the case of an incomplete combustion).

Fig. 4. Visualization of various hydrocarbons: we simulate the the establishment of a flame 800 ms after ignition for Acetylene (a), Butane (b), Cyclopropane (c),

Propane (d), Methane (e), and Ethylene (f). Our framework enables simulating stoichiometric mixtures of various fuels. The combustion patterns reflect differences in turbulence, stability, and height, related to molecule properties.

Moreover, species evolution from the combustion reaction is mod- eled by <u>𝜕𝑌𝑖 𝑣𝑐𝑞𝑖 𝑀𝑖</u> =*,* (7) *𝜕𝑡 𝜌* in which *𝑞𝑖* denotes the stoichiometric coefficient for species *𝑖* with molar mass *𝑀𝑖*. To complete the system, we relate the gas density to pressure, temperature, and mixture composition using the ideal gas law: ! −1 ∑︁ <u>𝑌</u> <u>𝑖</u> *𝜌* = *𝑝*amb*𝑇𝑅 𝑀𝑖,* (8) *𝑖* *,*

where *𝑅* is the universal gas constant. This formulation accounts for varying mixture composition and temperature effects in a physically grounded manner.

4.3 Evaporation Model To model phase change from liquid to gas, we implement an energy- driven evaporation scheme. The liquid density *𝜌𝑙* decreases accord- ing to the local energy surplus:
### <u>𝜕𝜌𝑙 𝜌𝑙𝐶𝑝(𝑇𝑙 − 𝑇amb)</u>

### = −

0 *,* (9) *𝜕𝑡 𝐶𝑝*(*𝑇𝐵* − *𝑇*amb)+ Δ*𝑣𝐻*

in which *𝑇𝑙* denotes the current liquid temperature, *𝑇𝐵* boiling tem- perature, *𝐶𝑝* liquid specific heat capacity, and Δ*𝑣𝐻* 0 latent heat of vaporization. Droplet conduction accelerates the heat transfer be- tween the air and the liquid as we assume a spray of droplets instead of a laminar flow: *𝜌* *𝑙𝐴𝑑𝜕𝑇 𝑘𝑇𝑑𝑐𝜕𝑇𝑙𝑘𝑇𝑑𝑐* *𝑇𝑑𝑐* = (*𝑇𝑙* − *𝑇*)*,* =*,* = −*.* (10) *𝜌𝑑 𝑑𝑑 𝜕𝑡 𝜌𝐶𝑝 𝜕𝑡 𝜌𝐶𝑝* The droplet’s diameter is denoted with *𝑑𝑑*, its surface area with *𝐴𝑑*, and its density with *𝜌𝑑*. Please note that the latter one does not correspond to the liquid density, but instead to the amount of liquid in the form of droplets. The mass fraction of a specific species changes based on the evaporated density and an evaporation coefficient. This decouples the evaporation process from the actual liquid and gas compositions, and enables the evaporation of different mixtures: <u>𝜕𝑌𝑖</u> = Γ*𝑖𝜌𝑙*(= *𝑆𝑖*)*,* (11) *𝜕𝑡* in which Γ*𝑖*denotes the evaporation coefficient for a specific species.

5 Algorithmics and Implementation Our simulation framework is implemented in Rust and uses WGPU as the primary GPU abstraction layer for compute shaders written in WGSL. The system architecture is based on the shipyard entity- component system, providing structured, parallel simulation state management. User interaction and visualization are handled through winit for windowing and egui for the GUI layer. All results pre- sented in this paper were generated on a workstation equipped with an Intel i9-13900K CPU, 128 GB of RAM, and an NVIDIA RTX 4090 GPU. The implementation closely follows the numerical model de- scribed in Algo. 1 which summarizes the complete simulation loop, incorporating Eulerian fluid dynamics for gas and heat transport, Lagrangian SPH for liquid behavior, and thermochemical models for evaporation, phase change, and combustion. To differentiate between all the different parts of the system, we settled on a few different subscripts. The subscript *𝑠* denotes properties that are only used for the correct behavior of the SPH system and therefore only appear in particle to particle interactions. The subscript*𝑝* is used for particle properties that can are exchanged with the grid. Both quantities, marked with either *𝑠* or *𝑝*, are stored in the particles. Liquid properties that are stored in the grid are denoted by *𝑙*. All other properties that are stored in the grid are written without any subscript.

5.1 Data Structures and Initialization The simulation domain is defined on a voxel grid, where each cell is flagged as *gas*, *solid*, or *gas+liquid*. Scalar fields such as temperature *𝑇*, species mass fractions *𝑌𝑖*, and pressure*𝑝* are stored at cell centers, while velocity*𝒖* is represented in a staggered marker-and-cell (MAC) grid layout with components stored at face centers [Bridson 2015]. Solid obstacles are initialized as uniformly distributed particles, which allows for particle-particle collisions. These are then sampled onto the grid and cells are flagged as a solid accordingly. This dual representation of solids enables both the particles and the grid to interact with solids. Dirichlet boundary conditions (fixed values) on solid walls are applied for velocity and Neumann (zero-gradient) conditions for pressure. Slip conditions are enforced with mirrored ghost cells [Bridson 2015]. For temperature, we impose fixed wall

Fig. 5. Four different species of small flame combustion: a clean combustion,
 common for a Bunsen burner and premixed combustion (a), a flame with a few traces of residuals that show visible glowing (b), an intermediate flame species where the bottom burns near complete combustion but water vapor and glowing residuals are already present in the upper parts of the flame (c), and an even more turbulent flame which shows large amounts water vapor and residuals, leading to colors typical for diffuse combustion (d).
268:6 • Wrede, et al.

values and apply one-sided derivatives near boundaries. The simula- tion uses a fixed timestep constrained by Courant–Friedrichs–Lewy (CFL) criteria for both advection and diffusion. SPH particles can be introduced into user-defined regions of a continuous Cartesian coordinate system and are colocated with the voxel grid. Each particle carries velocity *𝒖𝑝*, temperature *𝑇𝑝*, mass *𝑚𝑝*, and thermophysical properties such as specific heat and vaporization enthalpy. SPH-to-grid interpolation is performed using compact support kernels and normalization factors to ensure con- servative transfer of physical quantities. A list of SPH parameters is included in the Appendix (Tab. A.1.2).

5.2 Smoothed Particle Hydrodynamics Liquid behavior such as water spray, suppression, and droplet dy- namics is handled using SPH. We use pseudo-pressure and pseudo- density formulations from Clavet et al. [2005]. Particle to particle interactions are handled within the smoothing radius *ℎ𝑠*. The neigh- borhoods of the particles are denoted with *𝑁𝑠*(*𝑖*) with *𝑟𝑠𝑖𝑗*≤ *ℎ𝑠* and *𝑟𝑠𝑖𝑗*as the Euclidean distance between two particles. Each particle’s local density is estimated by
∑︁ <u>𝑟𝑠</u> <u>𝑖𝑗</u> 2 *𝜌𝑠𝑖*= 1 −*.* (12) *ℎ* *𝑠* *𝑗* ∈*𝑁𝑠*(*𝑖*)

Furthermore, a near-density term is computed with a cubic kernel. This allows for surface tension effects and particle collision avoid- ance due to the flat gradient of the quadratic smoothing kernel. It is defined by ∑︁ <u>𝑟𝑖𝑗</u>3 *𝜌𝑠* near *𝑖* = 1 −*.* (13) *ℎ* *𝑠* *𝑗* ∈*𝑁𝑠*(*𝑖*)

### These are then converted into pseudo-pressures

near near near *𝑝𝑠𝑖*= *𝑘𝑠*(*𝜌𝑠𝑖*− *𝜌𝑠₀*)*, 𝑝𝑠* *𝑖* = *𝑘𝑠 𝜌𝑠* *𝑖* *,* (14)

near where*𝑘𝑠* and*𝑘𝑠* are stiffness coefficients and*𝜌𝑠₀* is the rest density. Pseudo-pressure forces between particles are calculated as

press ∑︁ <u>𝑟𝑠</u> <u>𝑖𝑗𝑟𝑠𝑖𝑗</u> 2 *𝐹𝑠* *𝑖* = − *𝑝𝑠𝑗*1 − + *𝑝𝑠* near *𝑗* 1 − (15) *ℎ* *𝑠ℎ𝑠* *𝑗* ∈*𝑁𝑠*(*𝑖*)

and viscous damping is added through a pairwise radial velocity difference

Δ*𝒖𝑠* vis *𝑖𝑗* = (*𝒖𝑝𝑖*− *𝒖𝑝𝑗*)·*𝑟*ˆ*𝑠𝑖𝑗,* (16)

vis <u>𝑟𝑠𝑖𝑗</u> vis vis *𝒂𝑠* *𝑖* = (1 −)(*𝛼𝒖𝑠* *𝑖𝑗* + *𝛽*(Δ*𝒖𝑠* *𝑖𝑗*

)2)*𝑟*ˆ*𝑠𝑖𝑗,* (17)
*ℎ𝑠*

where *𝛼* and *𝛽* control linear and quadratic damping, respectively.

5.3 Particle-Grid Sampling To exchange information between SPH particles and the Euler- ian grid, we use a weighted sampling approach based on a simple distance kernel. This sampling uses a different smoothing radius, denoted *ℎ*, with the corresponding neighborhood *𝑁* containing all particles that satisfy *𝑟𝑖𝑗* ≤ *ℎ*. The distance here is the Euclidean distance between positions in the grid and particle positions. We
begin by defining a linear distance-based weight between particle *𝑖* and grid cell *𝑗*, i.e., <u>𝑟𝑖𝑗</u> *𝑤𝑖𝑗* =1− *ℎ*

*.* (18)
The mass of particles is used to accumulate the total liquid density contribution to grid cell *𝑗*: ∑︁ *𝜌𝑙* *𝑗* = *𝑚𝑝𝑖𝑤𝑖𝑗 .* (19) *𝑖* ∈*𝑁* ( *𝑗*)

### To normalize contributions, we compute a sampling normalization

### factor for the liquid density:

*𝜔𝑗* *𝜌* = ∑︁ *𝑤𝑖𝑗 ,* (20) *𝑖* ∈*𝑁* ( *𝑗*)

which allows us to estimate the particle mass *𝑚𝑖* by inverting the density contribution from grid to particles: −1 ∑︁ ∑︁ <u>𝑤𝑖𝑗</u> *𝑚𝑝* = © *𝑤𝑖𝑗* ª ® *𝜌𝑙.* (21) *𝑖 𝑗 𝜌* « *𝑗* ∈*𝑁* (*𝑖*)¬*𝑗* ∈*𝑁* (*𝑖*) *𝜔𝑗*

Temperature values are interpolated similarly. First, we compute a normalization factor for liquid temperature sampling: T ∑︁ *𝜔𝑗* = *𝑚𝑝𝑖𝑤𝑖𝑗 .* (22) *𝑖* ∈*𝑁* ( *𝑗*)

Then, the liquid temperature at grid cell *𝑗* is computed from nearby particles: ∑︁ *𝑇𝑙* = *𝑇𝑝 𝑚𝑝 𝑤𝑖𝑗*/*𝜔𝑗 .* T (23) *𝑗 𝑖 𝑖* *𝑖* ∈*𝑁* ( *𝑗*) To transfer liquid temperatures back from the grid to particles, we use a normalized interpolation:

∑︁ −1 ∑︁ <u>𝑚𝑝𝑖𝑤𝑖𝑗</u> ª® <u>𝑚𝑝𝑖𝑤𝑖𝑗</u> *𝑇𝑝𝑖*= © *𝑇𝑙* *𝑗*

*.* (24)
*𝜔𝑗* T *𝜔𝑗* T « *𝑗* ∈*𝑁* (*𝑖*)¬*𝑗* ∈*𝑁* (*𝑖*)

Fig. 6. Visualization of flame behavior: a flame in a contained environment

is undergoing oxygen starvation from an early stage (top row) to a later stage (bottom row). The images show the RGB rendering (a, e), the temperature field (b, f), fuel (green) and oxygen (blue) (c, g), and CO₂ (d, h). In the later stage (bottom row), the flame becomes less intense, temperature drops, and a significant decrease in oxygen concentration is observed which leads to the extinction of the flame.

The velocity of the liquid is also sampled, but only from the particles to the grid. This allows us to model different effects, like the Venturi effect and the rapid increase in air velocity after the evaporation of water. However, since we only sample in one direction, we do not need to ensure consistency between both directions, which simplifies the problem. Using the average velocity of all neighboring cells is sufficient, as we scale the liquid velocity with the sampled density when applying it to the gas velocity:

<u>1</u> ∑︁ *𝒖𝑙* *𝑗* = *𝒖𝑝𝑖.* (25) |*𝑁* ( *𝑗*)| *𝑖* ∈*𝑁* ( *𝑗*)

5.4 Droplet based Evaporation The evaporation, just like the combustion, is done on the grid. This is possible because we have bidirectional sampling of the liquid properties between the grid and the SPH system. Before the evap- oration is calculated, the liquid properties are sampled from the particles at the beginning of the grid update step and stored in the grid as the liquid properties. After the evaporation, the particle properties are sampled from the grid again before the end of the grid update step. Since we model the liquid as a spray of droplets, we can partially evaporate a liquid cell. To handle droplet-based evaporation explicitly, we define the total available energy in the liquid cell as
*𝐸𝑙* = (*𝑇𝑙* − *𝑇*amb)*𝐶𝑝𝜌𝑙𝑉𝑐* (26)

and the energy required to evaporate one droplet by 0 *𝐸𝑑* = (*𝐶𝑝*(*𝑇𝐵* − *𝑇*amb)+ Δ*𝑣𝐻*)*𝜌𝑑𝑉𝑑 .* (27)

Assuming *𝑛𝑑* total droplets, the energy balance equation becomes

*𝐸𝑙* = *𝐸𝑑𝑛𝑑, 𝜌𝑙* = *𝜌𝑑𝑛𝑑 .* (28)

The number of droplets*𝑛𝑒* = *𝐸𝑙*/*𝐸𝑑*, which can be evaporated, is then computed based on the energy budget and the resulting evaporated liquid density by *𝜌𝑒* = *𝑛𝑒𝜌𝑑𝑉𝑑*/*𝑉𝑐*. This formulation allows droplets to evaporate incrementally based on the available local energy while ensuring both energy and mass conservation across the grid and the SPH domain. After evaporation, the new mixture density ratio is computed to adjust thermodynamic properties, i.e.,

′<u>𝜌</u> *𝜌* =*,* (29) *𝜌* + Δ*𝜌𝑙*

the gas temperature is adjusted to reflect the thermal mixing with vapor at boiling point, i.e.,

*𝑇* ′ = (1 − *𝜌* ′ )*𝑇𝐵* + *𝜌* ′ *𝑇 ,* (30)

and species mass fractions *𝑌𝑖* are rescaled proportionally, i.e.,

*𝑌𝑖* ′ = *𝜌* ′ *𝑌𝑖 .* (31)

We also use the liquid velocity to model a change in gas velocity similar to the rapid expansion of evaporated water. This is done by using the evaporated density fraction and using it to interpolate between the gas and the liquid velocity:

*𝒖* ′ = (1 − *𝜌* ′ )*𝒖𝑙* + *𝜌* ′ *𝒖.* (32)

5.5 Simulation Loop The main simulation loop has been implemented according to Algo. 1. All scalar and vector fields on the grid and particles are initialized to default or user-provided values (Lines 1–4). This includes setting *𝒖*, *𝑇*, *𝜌*, *𝑌𝑖* on the grid, and initializing *𝒖𝑝*,*𝑇𝑝*,*𝑚𝑝* for each particle. Pseudo-density and near-density are computed for each particle (Eqs. 12 and 13), followed by pressure forces (Eqs. 14 and 15), viscos- ity forces (Eq. 17), and advection. Boundary enforcement at walls is handled using ghost particles and velocity reflection schemes (Lines 5–12). SPH particles contribute to the grid via weighted kernel sam- pling of mass, temperature, and velocity (Eqs. 19, 23 and 25). The weighting functions are normalized per cell to maintain conserva- tion (Lines 13–14). The gas solver runs a sequence of updates: semi-Lagrangian ad- vection, Boussinesq buoyancy, chemical reaction modeling (Eqs. 1 and 3), radiative thermal exchange (Eq. 2), and thermal as well as species diffusion. The Arrhenius equation is checked for each cell to combust the available fuel and oxygen, resulting in changes in temperature and species composition (Eqs. 4, 5 and 7). In grid cells containing a hot liquid, thermodynamic conditions are evaluated to trigger evaporation (Eqs. 9 to 11 and 26 to 31). Adjustments are made to local temperature, species mass fractions, and particle mass. Incompressibility is enforced by solving a pressure Poisson equation using a GPU-based iterative solver (Lines 15-22). The grid fields are interpolated back to SPH particles (Lines 23–24), updating their mass and temperature values using conservative kernels (Eqs. 21

268:8 • Wrede, et al.

### ALGORITHM 1: Hybrid combustion simulation.

Input: Initialized Eulerian grid with gas/solid flags; initialized SPH particle set. Output: Updated grid and particle state for each frame. 1 forall *Grid Cells* do 2 Initialize *𝒖* ← *𝒖*0, *𝑇* ← *𝑇*0, *𝑌𝑖*← *𝑌𝑖*0, *𝜌* ← *𝜌*0; 3 forall *SPH Particles* do 4 Initialize *𝒗𝑝*← *𝒗*0, *𝑇𝑝*← *𝑇*0, *𝑚𝑝*← *𝑚*0; 5 forall *Frames* do //— Lagrangian SPH Particle Step — 6 Spawn new particles; 7 forall *SPH Particles* do

|Compute density 𝜌|, near-density 𝜌|;|
|---|---|---|
|Compute pressure 𝑝 Apply pressure and viscosity forces; Update velocity and position 𝒗|, near-pressure 𝑝|;, 𝒙;|

8 *𝑝 𝑝*near 9 *𝑝 𝑝*near 10 11 *𝑝 𝑝* 12 Enforce boundary conditions; //— Particle-to-Grid Transfer — 13 forall *Grid Cells* do 14 *𝑙*, *𝑇*, *𝒖*, from SPH using kernel *𝑤𝑖𝑗*

|Sample 𝜌||;|
|---|---|---|
|//— Eulerian Grid Simulation Step —|||
|forall Grid Cells do|||
|Advection: update 𝒖, 𝑇, 𝑌 Buoyancy: apply Boussinesq force to 𝒖;|with semi-Lagrangian scheme;||
|Combustion: compute 𝑣|, 𝐽, update 𝑇, 𝑌|;|
|Diffusion: apply Laplacian on 𝑇, 𝑌 Thermal radiation: apply radiative thermal exchange;|;||
|Evaporation: update 𝜌, 𝜌|, 𝑇, 𝑇, 𝑌;||

15 16 *𝑖* 17 18 *𝑐 𝑐 𝑖* 19 *𝑖* 20 21 *𝑙 𝑙 𝑖* 22 Solve pressure projection to enforce incompressibility; //— Grid-to-Particle Transfer — 23 forall *SPH Particles* do 24 Interpolate*𝑚𝑝*and*𝑇𝑝*from grid; 25 Remove particles with expired lifetime; 26 Remove particles without any remaining mass;

Fig. 7. A parameter space exploration of the water vapor and residual reac-

tion coefficient. Starting from an entirely transparent flame (bottom left) we show the increase of residuals from left to right (horizontal axis) and the increase of vapor from bottom to top (vertical axis).

We then apply a denoising step to lower the necessary sample count for high quality renderings [Áfra 2025]. Flame coloration is determined through a combination of black- body radiation and chemiluminescence modeling [Stewart and John- son 2016]. The latter captures the spectral contributions of radicals like *𝐶𝐻*∗ (blue-green light) and *𝐶*2∗ (green light), which are in- ferred from local combustion activity and fuel richness, modulated by temperature. Residuals and soot, are tracked in a 3D texture and contribute both as light blockers at low temperatures and as emissive blackbody sources for hot gases. The soot map is used to darken the sooted surfaces, which enhances the realism of the material being exposed to combustion. For the figures rendered without path tracing, fire glow is instead further intensified by a bloom post-processing pass. Volumetric effects include colorless smoke that attenuates light and steam, which cast shadow rays to add depth and softness.

6 Results and Validation To demonstrate the capabilities of our framework, we present re- sults and validation obtained from various simulation experiments.

and 24). Particles that are fully evaporated or exceed user-defined lifespans are discarded from the simulation (Lines 25-26).

5.6 Visualization We use ray marching augmented with global illumination (GI) tech- niques to render dynamic imagery of combustion phenomena. The ray marcher samples a set of 3D scalar fields including residual his- tory for sooting, CO2, fuel, oxygen, liquid density, temperature, and vapor, to visualize complex interactions of solids, liquids and gases. Meshes are rendered via BVH-accelerated ray tracing with support for physically based material for rendering (PBR). We handle trans- parent materials like water and glass through index-of-refraction tracking and refraction modeling using Snell’s law, which allows us to render droplets and laminar flow. For scenes involving complex meshes (Figs. 1, 12, 18) we employ a Monte-Carlo path tracer, which marches through the volume grid before every bounce to integrate emission, absorption, and scattering into radiance [Pharr et al. 2016].

Fig. 8. Water extinguishing experiments: we use two nozzle types to generate a laminar (a)-(h) and a spray (i-p) type of water stream to extinguish a flame
 and show the impact of aiming the water at the top of the flame (a-d, i-l) and at the bottom of the flame (e-h, m-p). For each fire-water interaction we show the average spatio-temporal temperatures (d, h, l, p) which show the overall effectiveness of extinquishing a flame during an experiment. A laminar water stream directed at the top of a flame does not impact the fire which leads to an overall high temperature. A spray stream directed at the bottom of the fire immediately stops the fire resulting in an overall lower temperature (p).
Fig. 9. A series showing the combustion of methane and oxygen in a nitrogen environment. Oxygen is visualized in blue and fuel in green. The injected rate of
 oxygen is the same for the whole experiment, while the amount of fuel is ramped up from a mass fraction of 0.1 to 1.0 mixed with nitrogen. Additionally, a heat source is set up below the two emitters. (a, b) show the combustion with a fuel lean mixture, (c, d) show the stoichiometric mixture of fuel and oxygen and (e, f) show a fuel rich mixture caused by an injection of pure fuel mixing with pure oxygen.
Fig. 10. Liquid fuel: As we simulate the thermodynamics between gases and liquids our framework enables simulating liquid fuel. A liquid fuel (ethanol) is
 emitted into a flame (a) and ignites (b). The burning liquid collides with obstacles in the scene (c) and continues to burn until all fuel is evaporated (d).

268:10 • Wrede, et al.

Table 1. Simulation parameters for Acetylene, Butane, Cyclopropane,

Propane, Methane, and Ethylene combustion. Key quantities include fuel diffusivity *𝐷𝑓*, Arrhenius factor *𝐴*, reaction exponents *𝑎,𝑏*, heat of combus- tion Δ*𝑐𝐻𝑜*, stoichiometric coefficients*𝑞𝑓,𝑞𝑜,𝑞𝐶𝑂₂,𝑞𝐻₂𝑂*, and fuel-specific values *𝑀𝑓,𝐶𝑝,𝑘*. Note that values for *𝐷𝑓*, Δ*𝑐𝐻𝑜*, and *𝑀𝑓*are scaled by fac- tors indicated in their respective column headers. See Sec. 4 for definitions. *𝒐* *𝑫𝒇* *𝑴* 𝚫 *𝑯* *𝒄* *𝒇* Gas *𝑨 𝒂 𝒃* *𝑪𝒑 𝒌* 6 −5 − 2 (· 10 ) *𝒒𝒇 𝒒𝒐 𝒒𝑪𝑶₂ 𝒒𝑯₂𝑶*(·10 (·10 ) ) Acetylene (*𝐶*2*𝐻*2) 1*.*46 6*.*50 × 1011120*.*50 1*.*25 −1*.*0425 −2*.*0 −5*.*0 4*.*0 2*.*0 2*.*600 63 0*.*024 Butane (*𝐶*4*𝐻*10) 1*.*00 7*.*40 × 1011 0*.*15 1*.*60 −2*.*7105 −2*.*0 −13*.*0 8*.*0 10*.*0 5*.*812 1720 0*.*015 Cyclopropane (*𝐶*3*𝐻*6) 1*.*14 4*.*20 × 1011 −0*.*10 1*.*85 −1*.*8765 −2*.*0 −9*.*0 6*.*0 6*.*0 4*.*200 114 0*.*015 Propane (*𝐶*3*𝐻*8) 1*.*14 8*.*60 × 105 0*.*10 1*.*65 −2*.*0850 −1*.*0 −5*.*0 3*.*0 4*.*0 4*.*410 1670 0*.*016 Methane (*𝐶𝐻*4) 2*.*10 8*.*30 × 1012 −0*.*30 1*.*30 −0*.*8340 −1*.*0 −2*.*0 1*.*0 2*.*0 1*.*604 2230 0*.*034 Ethylene (*𝐶*2*𝐻*4) 1*.*63 2*.*00 × 10 0*.*10 1*.*65 −1*.*2510 −1*.*0 −3*.*0 2*.*0 2*.*0 2*.*805 1550 0*.*019

Specifically, we show that our method reproduces key physical behaviors in combustion, evaporation, and fluid coupling scenarios.

6.1 Results We organize our qualitative experiments into topical groups, each exploring different aspects of combustion dynamics, multiphase in- teractions, or system behavior under physically plausible conditions. The experiments range from fine-scale features such as flame struc- ture to complex scene-level behavior. In the Appendix, we include a table with parameter value ranges for user-controlled parame- ters (Tab. 3) and fixed parameters calibrated with values found in literature (Tab. 4).
*6.1.1 Combustion and Flame Behavior.* To evaluate how our frame- work handles fuel-dependent combustion dynamics, we simulate stoichiometric mixtures of various hydrocarbons. Specifically, we compare flame development 800 ms after ignition for six common fuels: Acetylene, Butane, Cyclopropane, Propane, Methane, and Ethylene (Tab. 1 for parameter values). In Fig. 4 we show the re- sulting flame and smoke plume structures. Although all cases begin with identical ignition conditions and domain setup, we set different Arrhenius equation parameter values based on the fuel and chemical properties for all gases. Acetylene (Fig. 4a) produces a tall, narrow flame with strong vertical acceleration, indicating high combustion velocity and low molecular weight. In contrast, Butane (Fig. 4b) and Propane (Fig. 4d) form broader, more turbulent plumes, consistent with their heavier hydrocarbon structure. Cyclopropane (Fig. 4c) exhibits structured vortex rings and moderate turbulence. Methane (Fig. 4e) yields a clean and vertically stable flame. Ethylene (Fig. 4f) generates more complex turbulence and brighter flame fronts due to its increased chemical reactivity. In Fig. 5 we show four different types of small flame combus- tion, illustrating the range of flame appearance our method can simulate. We calibrate combustion parameter values such as buoy- ancy, combustion heat efficiency *𝜑*, radiation coefficient *𝜖*, residual produced *𝑞𝑟𝑒𝑠𝑖𝑑𝑢𝑎𝑙*, and vorticity confinement strength for each ex- periment. Fig. 5a shows a clean combustion, typical of a Bunsen burner with premixed fuel. In Fig. 5b a flame with minor traces of glowing residuals is shown. A near-complete combustion occurs at the base while water vapor and glowing residuals appear in the upper regions (Fig. 5c). A more turbulent flame is shown in Fig. 5d, which is characterized by substantial amounts of water vapor and residuals.
Fig. 11. Simulation showing fire suppression at a window using principles

of fluid dynamics. Flames and hot gases vent from the window (a). A water stream is directed outward from the window (b), creating a high-velocity flow that induces a low-pressure zone outside (Bernoulli Principle). This draws heat, smoke, and flames out of the room while limiting air entrain- ment into the structure. The window opening acts as a constriction (Venturi Effect), accelerating the outward flow and enhancing the removal of hot gases (c). The fire is effectively suppressed as interior temperatures drop and oxygen supply is reduced (d).

To further investigate the expressiveness of our model, we con- duct a parameter space exploration by varying the water vapor (vertical axis) and the residual reaction coefficients (horizontal axis) in Fig. 7. The bottom-left corner corresponds to the minimal con- figuration where both coefficients are set to zero. Increasing the water vapor coefficient vertically (bottom to top) introduces pro- gressively more visible steam and white combustion products. In- creasing the residual coefficient horizontally (left to right) adds dark soot and incomplete combustion effects. While these coefficients do not influence the combustion process itself, they modulate the final appearance of the combustion products. This parameter space exploration illustrates our framework’s capability in smoothly cap- turing the visual transition from clean, complete combustion to sooty, oxygen-limited burns. Note that the scale of the residual co- efficient is logarithmic (0, 0.01, 0.1, 1, 10), whereas the water vapor coefficient is linear (0, 2.5, 5, 7.5, 10). To further analyze combustion dynamics under varying fuel– oxygen ratios, we simulate the combustion of methane and oxygen in a nitrogen environment while varying the fuel mass fraction. In

Fig. 9 we visualize this process using color-coded species: oxygen

in blue, fuel in green, and flame intensity in yellow-orange. Across three scenarios, the oxygen flow remains constant, while the fuel concentration increases from a lean to a rich mixture. In the first two frames (Fig. 9a, b), combustion occurs under fuel-lean conditions, producing small and intermittent flames. In Fig. 9c and Fig. 9d we show the equal mixture of fuel and oxygen, resulting in strong, vertically stable flames and optimal combustion efficiency. Finally,

Fig. 12. Two frames of a timeseries showing the extinction of a complex fire. Three vehicles are vigorously burning (a). The fire is then suppressed by a

high-pressure water jet (b), producing dense clouds of smoke and vapor. Our advanced combustion model supports multi-species thermodynamics and accurately simulates flame extinction dynamics.

Fig. 13. Ablation study of the dispersion and displacement terms of our

model. Enabling both terms causes water to evaporate while the water generates a drag on the fire and vapor (a). Disabling the displacement term leads to a vapor cloud that is not affected by the water (b). Only enabling the displacement term shows that the fire is dragged in the direction of the water (c). Disabling both terms leaves only the diffuse temperature exchange, which is too slow to have an effect on the fast interaction of flames and water.

in Fig. 9e and Fig. 9f, the excess fuel leads to a rich mixture, where incomplete combustion reduces flame visibility and promotes soot formation.

### 6.1.2 Liquid–Fire Interaction and Evaporation. We further analyze

the interaction between water jets and flame behavior by comparing laminar and spray nozzles, each aimed either at the top or bottom of the flame. As shown in Fig. 8a–d, a laminar stream directed toward the top of the flame has a limited extinguishing effect. The flame remains largely intact (Fig. 8a–c), and the spatio-temporal temperature map (Fig. 8d) shows only a localized temperature drop with high overall residual heat. In contrast, when the same laminar stream is aimed at the base of the flame (Fig. 8e–h), it disrupts the combustion zone more effectively, with Fig. 8h indicating lower core temperatures and a cooling effect extending upward. The spray nozzle produces a finer, more distributed stream that further improves the extinguishing. When directed at the top of the flame (Fig. 8i–l), it produces dense vapor and drag, but still fails

to fully suppress combustion (Fig. 8l). Finally, the spray directed at the bottom of the flame (Fig. 8m–p) results in complete flame suppression and the thermal field in Fig. 8p shows a significant reduction in overall temperature. To analyze evaporation-driven combustion, we simulate a sce- nario in which a stream of liquid fuel is injected into an existing flame. As shown in Fig. 10a, the fuel begins to evaporate upon con- tact with surrounding hot gases, forming a vapor-rich region near the jet. This vapor mixes with ambient oxygen and ignites, extend- ing the existing flame in direction of the jet (Fig. 10b). In Fig. 10c, the ignited liquid fuel continues to collide with a wall.

*6.1.3 Physics Probes and Diagnostics.* To examine how our sim- ulations can assist in fire suppression, we simulate the use of a high-velocity stream directed at a burning window opening. As shown in Fig. 11a, flames and hot gases naturally vent outward from the structure. In Fig. 11b, a water jet is introduced in front of the window, generating a fast-moving outflow that lowers the local pressure outside the window based on the Bernoulli principle. This pressure drop draws flames and hot gases out of the room. The window itself acts as a geometric constriction, and as illustrated in
Fig. 11c, this narrowing accelerates the exiting flow in accordance
 with the Venturi effect. As a result, smoke and thermal energy are more efficiently expelled. Finally, Fig. 11d shows that interior flames weaken significantly as oxygen inflow is reduced and overall tem- peratures drop. This diagnostic case demonstrates how directed airflow can passively assist fire mitigation. To study combustion under limited oxygen conditions, we simu- late a small flame undergoing progressive oxygen starvation. Fig. 6 shows visualizations at two distinct stages: early (top row, Fig. 6a–d) and late (bottom row, Fig. 6e–h). RGB renderings in Fig. 6a and 6e illustrate a clear decrease in flame luminosity and size over time. The corresponding temperature fields (Fig. 6b, f) confirm this drop, with lower peak temperatures and reduced vertical heat transport. Fuel and oxygen distributions (green and blue, respectively) in Fig. 6c and 6g show that the available oxygen diminishes significantly in the later stage. Finally, the CO2field in Fig. 6d and 6h confirms incomplete combustion and suppressed chemical reaction. This di- agnostic setup validates our model’s ability to simulate extinction phenomena resulting from local oxygen depletion.

268:12 • Wrede, et al.

(a)300 Average Sensor TemperatureOursFDS (b)700(c)Sensor Temperature Distribution (FDS) 2000 Sensor Temperature Distribution (Ours) 25014 kW60014 kW 14 kW
1000 200 500 800 400 600 150 300 100 200 400 Temperature (°C) Temperature (°C) Temperature (°C) 200 50 100 0 0 0 5 10 15 20 25 30 0.05 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1.0 1.2 1.4 0.05 1.6 1.80.1 2.00.2 2.20.3 2.40.4 2.60.5 2.80.6 3.00.7 0.8 0.9 1.0 1.2 1.4 1.6 1.8 2.0 2.2 2.4 2.6 2.8 3.0 Time (s) Sensor Height (m) Sensor Height (m)

(d) Average Sensor TemperatureOursFDS1200(e) Sensor Temperature Distribution (FDS) (f ) Sensor Temperature Distribution (Ours) 600 2000
57 kW 57 kW 57 kW 500 1000 1500 400 800 300 600 1000 200 400 500 Temperature (°C)100Temperature (°C)200 Temperature (°C) 0 0 0 5 10 15 20 25 30 0.05 0.1 0.2 0.3 0.4 0.5 0.60.05 0.7 0.1 0 0.8 0.2 0.9 0.3 1.0 0.4 1.2 0.5 1.4 0.6 1.61.00.7 1.81.20.8 2.0 1.40.9 2.21.6 2.41.8 2.6 2.0 2.6 2.8 2.22.8 3.0 2.43.0 Time (s) Sensor Height (m) Sensor Height (m)

(g) (h)1200 Sensor Temperature Distribution (FDS)(i)1500Sensor Temperature Distribution (Ours) 600 OursFDS
100033 kW 1250 33 kW 500 800 1000 750 400 600 300 400 500 200 250 Temperature (°C) Temperature (°C)200 100 Temperature (°C)0 0 0 10 20 30 40 0.05 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1.0 1.2 1.4 1.6 1.8 2.0 2.20.05 2.40.1 2.60.2 2.80.3 3.00.4 0.5 0.6 1.00.7 1.20.8 1.40.9 1.6 1.8 2.0 2.6 2.22.8 2.43.0 Time (s) Sensor Height (m) Sensor Height (m)

Fig. 14. Comparison of our simulation and FDS [McGrattan et al. 2013]. We use the experiment setup described in NBSIR 79-1910 [McCaffrey 1979] to measure

the average temperature of all sensors over a period of 30 seconds for flames with an energy of 14 kW (a) and 57 kW (d). We measure the mean and standard deviation of temperatures measured at each sensor for flames of 14 kW and 57 kW for FDS (b, e) and for our simulation (c, f). We also compared FDS with our simulation on a sprinkler setup for which we measure the average temperature (g) as well as the mean and standard deviation for FDS (h) and our simulation

(i). The results indicate that our framework is able to closely match the simulation results of FDS for both experiments.
With our multi-species thermodynamics model it is possible to realistically simulate the interaction between fire and extinguishing agents, capturing the nuanced dynamics of flame extinction and post-combustion behavior. In Fig. 16 we show the annealing process of a metal rod. Initially, a flame is applied (Fig. 16a) and gradually heats the rod (Fig. 16b, c) until it begins to glow visibly. Once the flame is removed (Fig. 16d), the rod continues to emit a glow as it slowly cools down, eventually

Fig. 15. Renderings of the scene setup for the enclosure fire experiment

described in NBSIR 79-1910 [McCaffrey 1979] (a) and a sprinkler experi-returning to its original state (Fig. 16e, f). ment (a-d) that we use to compare our model to FDS.

6.2 Validation
*6.1.4 Emergency Response Devices.* Fig. 18 illustrates a fire emer-We evaluated our combustion model qualitatively and quantitatively, gency scenario in a residential kitchen. A stovetop ignition initiates including comparisons of our simulation results with an established a spreading flame Fig. 18a, which triggers a smoke detector and solver for combustion, visual comparisons to previous approaches, activates an overhead sprinkler system Fig. 18b. Our simulation cap-and ablations studies of different components of our framework. tures the subsequent interaction between the water spray and the Runtime performance measures are shown in the Appendix (Tab. 2). hot combustion gases, showing both flame suppression and vapor generation. This example highlights the potential of physics-based
*6.2.1 Comparison to McCaffrey and FDS.* We conducted a series
fire simulation as an in silico test bed for evaluating real-world fire of comparative experiments against both real measurements of response strategies. combustion experiments as well as established combustion solvers

*6.1.5 Multiphase Scene-Level Experiments.* In Fig. 12 we show two (FDS [McGrattan et al. 2013]). The first experiment replicates the frames from a time series illustrating the extinction of a complex enclosure fire test described in NBSIR 79-1910 [McCaffrey 1979] fire scenario. In the initial frame (a), three vehicles are burning. The conducted in a 1.5m × 1.5m × 3.9m sized compartment (see Fig. 15a). combustion leads to pronounced flame structures and smoke. In the A natural gas burner with a heat release rate of approximately 14.4 subsequent frame (b), the fire is actively being suppressed by a high-to 57.5 kW was used, producing a steady diffusion flame. The flame pressure water jet, resulting in dense plumes of smoke and vapor. was placed on a pedestal of 0.72 m height. To capture the vertical

Fig. 16. Annealing of a metal rod: a flame is initiated (a) and slowly increases

the temperature of a metal rod (b) until it starts to glow (c). After the flame is turned off (d), the rod remains glowing (e) until it is entirely cooled off (f).

1.2 x 103 103
temperature profile and development of the hot gas layer 21 temper- ature sensors were installed in a vertical line at heights ranging from

0.05m to 3.0m from the pedestal. We use this data for validating the thermal and fluid dynamic behavior of our solver. In Fig. 14 we show a comparative analysis of FDS and our framework on this setup. Specifically, we analyze the average temperature recorded by all sensors over a continuous 30-second interval for flame heat release rates of 14 kW and 57 kW, as illustrated in Fig. 14a and Fig. 14d, respectively. Blue lines indicate our temperature evolution which closely conforms to the red lines resulting from the equivalent FDS simulation. Beyond average temperatures, we further assess the spatial temperature distribution by computing the mean and stan- dard deviation of the temperature at each individual sensor location. This analysis is conducted separately for the 14 kW (Fig. 14a-c) and 57 kW (Fig. 14d-f) flame scenarios for FDS and our model. These results reveal a strong correlation between our simulation outputs and those generated by FDS, thereby indicating that our framework is able to replicate complex thermal behaviors observed in standard fire scenarios. In Fig. 17, we show the centerline temperature rise (Δ*𝑇*) as a function of height (*𝑧*) above the burner for various heat release rates
(*𝑄*), comparing results from our thermodynamics model with those from FDS. The plot illustrates how temperature varies along the vertical axis of the flame for different fire sizes (14.4 kW, 33 kW,
57.5 kW), highlighting the thermal behavior within the plume. The results demonstrate that our model not only reproduces the exper- imentally observed temperature rise (NBSIR 79-1910 [McCaffrey 1979]) with high fidelity but also shows strong agreement with the FDS simulation outputs. In a second experiment, we simulate a sprinkler activation sce- nario, comparing the fire suppression dynamics, spray interaction, and temperature decay between our solver and FDS. We use the same 1.5m × 1.5m × 3.9m sized compartment with a pedestal (0.72m height) and a 33 kW flame. After 15 seconds a water sprinkler (with 300 liters/min) is started, which causes the extinction of the flame. A visualization of our setup is shown in Fig. 15a-d. The average temperatures of FDS and our simulation as well as the mean and standard deviations of individual sensors are shown in Fig. 14g-i. These results show that our framework is able to extinguish fire with similar temperature characteristics as FDS.
*6.2.2 Qualitative Validation.* To assess the visual fidelity of our method, we performed a qualitative comparison against the flame results shown in Nielsen et al. [2022]. Fig. 19 illustrates this com- parison across both large and small flame regimes. In Fig. 19a, we show the result from Nielsen et al. [2022], and in Fig. 19b, our frame- work produces a similarly detailed large-scale flame, including fine turbulent structures. This demonstrates that our model supports high-resolution flames without requiring an explicit signed distance field to track the flame front. However, in the small-flame regime,
Fig. 19c and Fig. 19d show real flames with sharp, thin flame fronts
 such as those from Bunsen burners, which are well-represented in Nielsen et al.’s [2022] method. By contrast, Fig. 19e shows that our approach is less suited for this regime due to its lack of flame front modeling, resulting in a more diffuse appearance.
ΔT (C) Flame Intermittent Plume

14.4 kW (FDS)33.0 kW (FDS)
102 57.5 kW (FDS)

14.4 kW (Ours)
33.0 kW (Ours)
57.5 kW (Ours)
2/5 ) η(z/Q 10-2 2/5 10 -1 -2/5 100 z/Q (m W)

Fig. 17. We compare FDS to our thermodynamics model by measuring the

centerline temperature rise (ΔT) versus height (z) for various heat release rates (Q). This figure illustrates how the temperature along the flame’s centerline changes with height above the burner for different fire sizes. The results indicate that our framework closely matches the measured rise in temperature as well as the simulated results of FDS.

*6.2.3 Dispersion Ablation.* To evaluate the significance of disper- sion and displacement forces in water–fire interaction, we con- duct an ablation study comparing four configurations. As shown in
Fig. 13a, enabling both the dispersion and displacement terms pro-
 duces the expected interaction: water evaporates rapidly, forming vapor, and simultaneously pushes back the flame through drag. In
Fig. 13b, we disable the displacement term while keeping dispersion
 active. This leads to a vapor cloud forming correctly, but with no coupling to the flame, which continues burning undisturbed. In
Fig. 13c, we disable dispersion but retain displacement. The flame is
 physically deflected by the incoming spray, yet vapor formation is nearly absent, indicating that momentum exchange alone is insuffi- cient for suppression. Finally, Fig. 13d disables both terms, leaving only passive heat transfer. In this case, the flame remains stable, with neither visible vapor nor dynamic suppression. This comparison illustrates that both dispersion and displacement are essential for simulating fast and realistic flame response and visible water-driven disruption.

268:14 • Wrede, et al.

Fig. 18. Emergency response devices: A stovetop in a kitchen is catching fire (a). After a while a fire detector detects the smoke and starts a water sprinkler (b)

to extinguish the fire (c).

While effective for a range of scenarios, the framework has sev- eral limitations. Solids are treated as static obstacles without thermal deformation or melting. The flame front is not explicitly modeled, leading to smoother transitions and limited accuracy in laminar or small-scale flames. Furthermore, we do not not explicitly simulate radiation in a detailed manner while heat transfer in liquids is sub- ject to diffusion artifacts due to temperature sampling. Only one fuel and liquid type is supported per simulation, with globally uniform reaction parameters; localized variation in combustion complete- ness is currently not modeled. Additionally, all species are advected

Fig. 19. Comparison to Nielsen et al. [2022]: Our method enables generating

### identically, ignoring differential effects such as CO2stratification.

larger flames (b) with a similar degree of complexity (a). As we do not explicitly compute the flame front with a signed distance field, our method

(e) does not allow simulating smaller flames (e.g. Bunsen burner) with the same degree of detail as shown for real flames (c), or (d) Nielsen et al. [2022]. 7 Discussion and Limitations We present a physically grounded framework for combustion simu- lation with support for chemical species, stoichiometric reactions, and residual formation. Conceptually, our approach is related to other species-transport fire models [Merci and Beji 2016; Nielsen et al. 2022]. However, our method enables multi-phase interactions between gas, liquid, and static solid materials, with droplet-based evaporation and species-dependent rendering that reflect underly- ing combustion chemistry. Compared to prior methods, our model advances the state-of-the-art by unifying chemically driven combus- tion with multi-phase simulation and species-aware visualization in a single framework. Unlike artistic fire models, our approach explicitly tracks species mass fractions and supports stoichiometric control over reaction products. This enables simulations that dif- ferentiate between complete and incomplete combustion based on molecular composition. The inclusion of droplet-based evaporation within a species-coupled solver allows realistic interactions between flames and water – capturing both energy exchange and visible ef- fects such as vaporization and suppression. This allows to test fire suppression strategies in the safe confines of a computer simulation. Furthermore, the physically motivated rendering pipeline ties tem- perature and species content to appearance, providing interpretable visual outputs rooted in combustion chemistry. While grounded in existing fluid simulation techniques, the integration of these com- ponents enables new types of fire–fluid interactions with a degree of physical plausibility not previously demonstrated in real-time or artist-controllable systems.
8 Conclusion and Future Work We have presented a unified, hybrid framework for simulating multi- phase combustion and fire suppression, with a combined Euler- ian and Lagrangian representation to capture complex interactions across solid, liquid, and gas phases. By explicitly modeling key combustion species and phase transitions as well as by modeling stoichiometry-aware heat release, our method supports simulating diverse scenarios, including open flame propagation, water-based flame extinction, and fire dynamics responsive to the environment. Moreover, our framework bridges the gap between high-fidelity com- bustion models and the efficiency required for interactive graphics. With a novel hybrid multi-species thermodynamics model, we see several avenues for future work. First, our current model simplifies certain chemical formulations and assumes predefined reaction pa- rameters; incorporating adaptive or learned chemical kinetics could improve realism for a broader class of fuels. Second, we would like to evaluate the different hydrocarbon fuels in more detail. A com- parison with real shapes and combustion behaviors would enable us to calibrate our approach even further. Third, enhanced mod- eling of radiative heat transfer and soot formation would further increase visual fidelity, especially in dense smoke scenarios. Another interesting direction would be to couple our solver with methods for describing geometric deformations of combustion. Finally, in- tegrating this system into real-time engines or VR environments, where user interaction and sensory feedback (e.g., heat, sound) play a crucial role, remains an exciting frontier. We believe our work lays a strong foundation for next-generation fire simulation tools that are both physically grounded and artistically expressive, applicable in visual media, safety training, and scientific visualization.

Acknowledgments We thank the reviewers for their valuable comments and suggestions. This work is supported by ERC grant 101170158 - WildfireTwins.

References

A. T. Áfra. 2025. Intel®Open Image Denoise. [https://www.openimagedenoise.org](https://www.openimagedenoise.org).
G. Aguilera and J. Johansson. 2019. Avengers: Endgame, a new approach for combustion simulations. In *ACM SIGGRAPH 2019 Talks (SIGGRAPH ’19)*. ACM, Article 39.
A. Bakhshaii and E.A. Johnson. 2019. A review of a new generation of wild- fire–atmosphere modeling. *Canadian Journal of Forest Research* 49, 6 (2019), 565–574.
R. Bridson. 2015. *Fluid simulation for computer graphics*. AK Peters/CRC Press.
R. Bridson and M. Müller-Fischer. 2007. Fluid simulation. In *ACM SIGGRAPH 2007* *Courses*. ACM, 1–81.
G. Cao, J. Railio, E. F. Curd, M. Hyttinen, P. Liu, H. M. Mathisen, D. Belkowska-Woloczko,
M. Justo-Alonso, P. White, C. Coxon, and T. A. Wenaas. 2020. Chapter 9 - Air- handling processes. In *Industrial Ventilation Design Guidebook (Second Edition)*. Academic Press, 417–496.
N. Chiba, K. Muraoka, H. Takahashi, and M. Miura. 1994. Two-dimensional visual simulation of flames, smoke and the spread of fire. *The Journal of Visualization and* *Computer Animation* 5, 1 (1994), 37–53.
S. Clavet, P. Beaudoin, and P. Poulin. 2005. Particle-based viscoelastic fluid simulation. In *ACM SIGGRAPH/Eurographics SCA*. 219–228. J L. Coen. 2013. *Modeling Wildland Fires :*. National Center for Atmospheric Research (NCAR)„ Boulder, CO :. 2013-02-04.
J. L. Coen, M. Cameron, J. Michalakes, E. G. Patton, P. J. Riggan, and K. M. Yedinak. 2013. WRF-Fire: Coupled Weather–Wildland Fire Modeling with the Weather Research and Forecasting Model. *Journal of Applied Meteorology and Climatology* 52, 1 (2013), 16 – 38.
E. Coumans and Y. Bai. 2016–2021. PyBullet, a Python module for physics simulation for games, robotics and machine learning. [http://pybullet.org](http://pybullet.org).
D. Demidov. 2019. AMGCL: An efficient, flexible, and extensible algebraic multigrid implementation. *Lobachevskii Journal of Mathematics* 40 (2019), 535–546.
R. Fedkiw, J. Stam, and H. W. Jensen. 2001. Visual simulation of smoke. In *Conference* *on Computer Graphics and Interactive Techniques (SIGGRAPH ’01)*. Association for Computing Machinery, New York, NY, USA, 15–22.
B. Feldman, J. M. Cohen, and J. F. Hughes. 2003. Animating Suspended Particle Explo- sions. *ACM Transactions on Graphics (TOG)* 22, 3 (2003), 708–715.
T. Hädrich, D. T. Banuti, W. Pałubicki, S. Pirk, and D. L Michels. 2021. Fire in paradise: Mesoscale simulation of wildfires. *ACM Trans. on Graph. (TOG)* 40, 4 (2021), 1–15.
J.-M. Hong, T. Shinar, and R. Fedkiw. 2007. Wrinkled flames and cellular patterns. *ACM* *Trans. Graph.*26, 3 (July 2007), 47–es.
Y. Hong, D. Zhu, X. Qiu, and Z. Wang. 2010. Geometry-based control of fire simulation. *The Visual Computer* 26, 9 (01 Sep 2010), 1217–1228.
C. Horvath and W. Geiger. 2009. Directable, High-resolution Simulation of Fire on the GPU. *ACM Trans. Graph.*28, 3, Article 41 (2009), 8 pages.
I. Ihm, B. Kang, and D. Cha. 2004. Animation of reactive gaseous fluids through chemical kinetics. In *ACM SIGGRAPH/Eurographics SCA*. Eurographics Association, 203–212.
J. Katan and L. Perez. 2021. ABWiSE v1.0: toward an agent-based approach to simulating wildfire spread. *Natural Hazards and Earth System Sciences* 21, 10 (2021), 3141–3160.
T. Kim, E. Hong, J. Im, D. Yang, Y. Kim, and C.-H. Kim. 2017. Visual simulation of fire-flakes synchronized with flame. *The Visual Computer* 33 (06 2017).
T. Kim, N. Thürey, D. James, and M. Gross. 2008. Wavelet Turbulence for Fluid Simula- tion. *ACM Trans. Graph.*27, 3, Article 50 (2008), 6 pages.
M. Kinateder, E. Ronchi, D. Nilsson, M. Kobes, M. Müller, P. Pauli, and A. Mühlberger.
2014. Virtual reality for fire evacuation research. In *2014 Federated Conference on* *Computer Science and Information Systems*. 313–321.
A. Kokosza, H. Wrede, D. Gonzalez E., M. Makowski, D. Liu, D. L. Michels, S. Pirk, and
W. Palubicki. 2024. Scintilla: Simulating Combustible Vegetation for Wildfires. *ACM* *Trans. Graph.*43, 4, Article 70 (July 2024), 21 pages.
K. A. Kroos and M. C. Potter. 2014. *Thermodynamics for Engineers*. Cengage Learning.
N. Kwatra, I. Essa, A. Schödl, N. Thuerey, C. Wojtan, and G. Turk. 2009. A Method for Avoiding the Acoustic CFL Condition for Compressible Flow. *ACM Transactions on* *Graphics (TOG)* 28, 5 (2009), 1–8.
A. Lamorlette and N. Foster. 2002. Structural modeling of flames for a production envi- ronment. In *Conference on Computer Graphics and Interactive Techniques (SIGGRAPH* *’02)*. ACM, New York, NY, USA, 729–735.
C. Lapointe, N. Wimer, J. Glusman, A. Makowiecki, J. Daily, G. Rieker, and P. Hamlington.
2020. Efficient simulation of turbulent diffusion flames in OpenFOAM using adaptive mesh refinement. *Fire Safety Journal* 111 (01 2020), 102934.
D. R. Lide. 1995. *CRC handbook of chemistry and physics: a ready-reference book of* *chemical and physical data*. CRC press.
R.R. Linn, S.L. Goodrick, S. Brambilla, M.J. Brown, R.S. Middleton, J.J. O’Brien, and J.K. Hiers. 2020. QUIC-fire: A fast-running simulation tool for prescribed fire planning. *Environmental Modelling Software* 125 (2020), 104616.
R. Linn, J. Reisner, J. Colman, and J. Winterkamp. 2002. Studying wildfire behavior using FIRETEC. *International Journal of Wildland Fire* 11 (11 2002), 233–246.
P. J. Linstrom and W. G. Mallard. 2025. NIST Chemistry WebBook. NIST Standard Reference Database Number 69. Retrieved May 23, 2025.
D. Liu, J. Klein, F. Rist, W. Pałubicki, S. Pirk, and D. L. Michels. 2025. FlameForge: Combustion of Generalized Wooden Structures. *ACM SIGGRAPH / Eurographics* *Symposium on Computer Animation* (2025).
S. Liu, T. An, Z. Gong, and I. Hagiwara. 2012. *burning*. Springer-Verlag, Berlin, Heidelberg, 110–120.
*Physically based simulation of solid objects’*

S. Liu, Q. Liu, T. An, J. Sun, and Q. Peng. 2009. Physically based simulation of thin-shell objects’ burning. *Vis. Comput.*25, 5–7 (April 2009), 687–696.
F. Losasso, G. Irving, E. Guendelman, and R. Fedkiw. 2006a. Melting and burning solids into liquids and gases. *IEEE Transactions on Visualization and Computer Graphics* 12, 3 (2006), 343–352.
F. Losasso, T. Shinar, A. Selle, and R. Fedkiw. 2006b. Multiple interacting liquids. *ACM* *Trans. Graph.*25, 3 (July 2006), 812–819. B J. McCaffrey. 1979. *Purely Buoyant Diffusion Flames: Some Experimental Results*. Final Report NBSIR 79-1910. National Bureau of Standards, Washington, D.C. https: //nvlpubs.nist.gov/nistpubs/Legacy/IR/nbsir79-1910.pdf
K. McGrattan, R. McDermott, C. Weinschenk, and G. Forney. 2013. Fire Dynamics Simulator Users Guide, Sixth Edition.
A. D. McNaught and A. Wilkinson. 1997. *IUPAC Gold Book: Compendium of Chemical* *Terminology*. Blackwell Scientific Publications. [https://goldbook.iupac.org](https://goldbook.iupac.org)
Z. Melek and J. Keyser. 2002. Interactive simulation of fire. *Pacific Graphics* (2002), 431–432.
W. Mell, M. Jenkins, J. Gould, and P. Cheney. 2007. A Physics Based Approach to Modeling Grassland Fires. *International Journal of Wildland Fire* (2007).
B. Merci and T. Beji. 2016. Modeling and simulation of transport phenomena in fire engineering. *Fire Safety Journal* 80 (2016), 12–22.
B. Merci and T. Beji. 2022. *Fluid mechanics aspects of fire and smoke dynamics in* *enclosures*. CRC press.
V. Mihalef, B. Unlusu, D. Metaxas, M. Sussman, and M. Hussaini. 2006. Physics based boiling simulation. 317–324.
K. Museth. 2013. VDB: High-resolution sparse volumes with dynamic topology. *ACM* *transactions on graphics (TOG)* 32, 3 (2013), 1–22.
D. Quang Nguyen, R. Fedkiw, and H. W. Jensen. 2002. Physically Based Modeling and Animation of Fire. *ACM Trans. Graph.*21, 3 (2002), 721–728.
D. Q. Nguyen, R. P. Fedkiw, and M. Kang. 2001. A Boundary Condition Capturing Method for Incompressible Flame Discontinuities. *J. Comput. Phys.*172, 1 (2001), 71–98.
M. B. Nielsen, M. Bojsen-Hansen, K. Stamatelos, and R. Bridson. 2022. Physics-based combustion simulation. *ACM Transactions on Graphics (TOG)* 41, 5 (2022), 1–21.
M. B. Nielsen, K. Stamatelos, M. Bojsen-Hansen, and R. Bridson. 2019. Physics-based combustion simulation in bifrost. In *ACM SIGGRAPH 2019 Talks*. 1–2.
Z. Pan and D. Manocha. 2017. Efficient Solver for Spacetime Control of Smoke. *ACM* *Trans. Graph.*36, 5, Article 162 (July 2017), 13 pages.
V. Pegoraro and S. G. Parker. 2006. Physically-based Realistic Fire Rendering. *EG Nat.* *Phenom.* (2006), 51–59.
N. Peters. 2000. *Turbulent Combustion*. Cambridge University Press.
M. Pharr, W. Jakob, and G. Humphreys. 2016. *Physically Based Rendering: From Theory* *to Implementation* (3rd ed.). Morgan Kaufmann Publishers Inc.
S. Pirk, M. Jarząbek, T. Hädrich, D. L. Michels, and W. Palubicki. 2017. Interactive wood combustion for botanical tree models. *ACM Trans. Graph. (TOG)* 36, 6 (2017), 1–12.
A. H. Rabbani, J.-P. Guertin, D. Rioux-Lavoie, A. Schoentgen, K. Tong, A. Sirois-Vigneux, and D. Nowrouzezahrai. 2022. Compact Poisson Filters for Fast Fluid Simulation. In *ACM SIGGRAPH 2022*. ACM, New York, NY, USA, Article 35, 9 pages.
K. Schmidt-Rohr. 2011. The heat of combustion—getting it right. *Journal of Chemical* *Education* 88, 8 (2011), 1099–1104.
K. Schmidt-Rohr. 2015. Why combustions are always exothermic, yielding about 418 kJ per mole of O2. *Journal of Chemical Education* 92, 12 (2015), 2094–2099.
J. Stam. 1999. Stable Fluids. *Proc. of ACM SIGGRAPH* (1999), 121–128.
J. Stam and E. Fiume. 1995. Depicting fire and other gaseous phenomena using diffusion processes. In *Conference on Computer Graphics and Interactive Techniques (SIGGRAPH* *’95)*. ACM, New York, NY, USA, 129–136.
S.M. Stewart and R.B. Johnson. 2016. *Blackbody Radiation: Computational Aids and* *Numerical Methods*. Taylor & Francis.
A. Stomakhin, C. Schroeder, C. Jiang, L. Chai, J. Teran, and A. Selle. 2014. Augmented MPM for phase-change and varied materials. *ACM Trans. Graph.*33, 4, Article 138 (July 2014), 11 pages.
H. Versteeg and W. Malalasekera. 2007. *An Introduction to Computational Fluid Dynamics* *e-book*. Pearson Education. [https://books.google.de/books?id=dlC8MgEACAAJ](https://books.google.de/books?id=dlC8MgEACAAJ)
C. K. Westbrook and F. L. Dryer. 1981. Simplified reaction mechanisms for the oxidation of hydrocarbon fuels in flames. *Combustion science and technology* 27, 1-2 (1981), 31–43.
Y. Zhao, X. Wei, Z. Fan, A. Kaufman, and H. Qin. 2003. Voxels on Fire. In *Proceedings of* *the 14th IEEE Visualization 2003 (VIS’03) (VIS ’03)*. IEEE Computer Society, USA, 36.

268:16 • Wrede, et al.

### A Appendix

|A.1|List of Symbols||||
|---|---|---|---|---|
|A.1.1|Model Parameters.||||
|𝒖|Velocity field (m s|)|||
|𝜈|Kinematic viscosity (m|s)|||
|𝜌|Fluid’s local density (kg m|)|||
|𝑝|Pressure (Pa)||||
|𝑇|Temperature (K)||||
|𝑇|Reference ambient temperature (K)||||
|𝒈|Gravitational acceleration vector (m s||)||
|𝑘|Thermal conductivity (W m|K|)||
|𝐶|Specific heat at constant pressure (gas) or liquid specific heat capacity (liquid) (J kg|)|||
|𝜖|Emissivity (dimensionless)||||
|𝜎|Stefan-Boltzmann constant (5.67 × 10||W m|K)|
|𝐽|Volumetric heat release rate from combustion (W m|||)|
|𝑌|Mass fraction of species 𝑖 (dimensionless)||||
|𝐷|Diffusion coefficient of species 𝑖 (m||s)||
|𝑆|Source term from phase change for species 𝑖 (s|||)|
|Δ 𝐻|Standard heat of combustion (J mol||or J kg)||
|𝑐,ℎ,𝑜,𝑛|Number of carbon, hydrogen, oxygen, nitrogen atoms in fuel molecule (dimensionless, for empirical Δ|𝐻|)||
|𝑣|Volumetric reaction rate (typically mol m||s)||
|𝐴|Arrhenius pre-exponential factor (units vary based on reaction order and concentration units)||||
|𝑛|Temperature exponent in Arrhenius equation (dimensionless)||||
|𝐸|Activation energy (J mol|)|||
|𝑅|Universal gas constant (8.314 J mol||K)||
|𝑐 ,𝑐|Molar concentrations of fuel and oxidizer, respectively (mol m||||
|𝑎,𝑏|Empirical reaction order exponents for fuel and oxidizer, respectively (di- mensionless)||||
|𝜑|Combustion heat efficiency (dimensionless)||||
|𝑞|Stoichiometric coefficient for species 𝑖 (dimensionless)||||
|𝑀|Molar mass of species 𝑖 (kg mol||)||
|𝑝|Ambient pressure (Pa) (used in ideal gas law context)||||
|𝜌|Liquid density (kg m|)|||
|𝑇|Current liquid temperature (K)||||
|𝑇|Boiling temperature (K)||||
|Δ 𝐻|Latent heat of vaporization (J kg||)||
|𝑇|Intermediate term for droplet conduction calculation (K m)||||
|𝑑|Droplet diameter (m)||||
|𝐴|Droplet surface area (m|)|||
|𝜌|Intrinsic density of the liquid making up a droplet (kg m|||)|
|Γ|Evaporation coefficient for species 𝑖 (m||kg s|)|
|𝜌|Evaporated liquid density (mass of liquid evaporated per unit volume of gas) (kg m||||
|𝐸|Total available energy in a liquid cell (J)||||
|𝑉|Volume of a grid cell (m|)|||
|𝐸|Energy required to evaporate one droplet (J)||||
|𝑉|Volume of one droplet (m|)|||
|𝑛|Total number of droplets in a cell (dimensionless)||||
|𝑛|Number of droplets that can be evaporated from a cell (dimensionless)||||
|𝒖|Liquid velocity (sampled to grid) (m s||)||
|A.1.2|Algorithmic and SPH Parameters.||||
|𝒖|SPH particle velocity (m s|)|||
|𝑇|SPH particle temperature (K)||||
|𝑚|SPH particle mass (kg)||||
|ℎ|SPH smoothing radius (for particle-particle interactions) (m)||||
|𝑟|Euclidean distance between SPH particles 𝑖 and 𝑗 (m)||||
|𝜌 ACM Trans. Graph., Vol. 44, No. 6, Article 268. Publication date: December 2025.|SPH particle local pseudo-density (sum of kernel values, dimensionless as per Eq. 12)||||

amb

*𝑝* −1

*𝑐* *𝑖* *𝑖* *𝑖* *𝑜* *𝑐*

*𝑐*

Table 2. Performance characteristics of our method. For a set of figures we

show the number of voxels, the average number of active particles (AP), *𝑎* the number of vertices (NV), the simulation time of the grid (STG), the simulation time of particles (STP), and the render time (RT). −3) *𝑓 𝑜*

|Figure|# of Voxels|AP|NV|STG|STP|RT|
|---|---|---|---|---|---|---|
|1|400×300×200|175k|27k|65ms|3ms|960ms|
|4|128×128×256|-|-|29ms|-|10ms|
|8a-d|300×150×300|6.7k|-|37ms|8ms|23ms|
|8i-j|300×150×300|6.7k|-|36ms|6ms|35ms|
|11|160|57k|3k|21ms|2ms|52ms|
|18|210××200 150××240|90 106k|1.5M|7ms|15ms|872ms|

*𝑖* *𝑖* amb *𝑙* *𝑙* *𝐵* *𝑣* 0 *𝑑𝑐* *𝑑* *𝑑* *𝑑* *𝑖* *𝑒* −3) *𝑙* *𝑐* *𝑑* *𝑑* *𝑑* *𝑒* *𝑙*

*𝑝* *𝑝* *𝑝* *𝑠* *𝑠𝑖𝑗* *𝑠𝑖*

−1 2 −1 −3

−2 −1 −1

K −1

−8 −2 −4 −3

2 −1 −1 −1 −1

*𝑐* *𝑜* −3 −1

−1 −1 −1

−1

−3

−1

2 −3 3 −1 −1

−1

−1

*𝜌* *𝑠* near *𝑖*

*𝑝* *𝑠* *𝑖* *𝑝* *𝑠* near *𝑖* *𝑘* *𝑠* *𝑘* *𝑠* near *𝜌* *𝑠* 0 *𝒂* *𝑠* press *𝑖*

Δ*𝒖𝑠*vis *𝑖𝑗* *𝑟*ˆ *𝑠𝑖𝑗* *𝒂* *𝑠* vis *𝑖* *𝛼* *𝛽* *ℎ* *𝑟* *𝑖𝑗* *𝑤* *𝑖𝑗* *𝜌* *𝑙* *𝜔* *𝜌* *𝑗𝑗*

*𝜔*T*𝑗* *𝑇* *𝑙𝑗*

A.2 SPH particle near pseudo-density (sum of kernel values, dimensionless as per Eq. 13) SPH particle pseudo-pressure (specific energy form) (m2s−2) SPH particle near pseudo-pressure (specific energy form) (m2s−2)
2 −2 SPH stiffness coefficient (e.g., related to speed of sound squared) (m s) SPH near-stiffness coefficient (m2s−2) SPH reference pseudo-density number (dimensionless) Acceleration on SPH particle *𝑖* due to pseudo-pressure forces (m s−2) (as- suming *𝐹𝑠* press *𝑖*in text is*𝑚𝑖𝒂𝑠* press *𝑖*) Pairwise radial velocity difference for SPH viscosity (m s−1) Unit vector from SPH particle *𝑖* to *𝑗* (dimensionless) Viscous acceleration on SPH particle *𝑖* (m s−2) SPH linear viscosity damping coefficient (s−1) SPH quadratic viscosity damping coefficient (m−1) Smoothing radius for particle-grid sampling (m) Euclidean distance between SPH particle *𝑖* and grid cell center *𝑗* (m) Linear distance-based weight for particle-grid sampling (dimensionless) Total liquid density contribution to grid cell *𝑗* from SPH particles (kg m−3) Sampling normalization factor for liquid density at grid cell *𝑗* (dimension- less) Sampling normalization factor for liquid temperature at grid cell *𝑗* (kg) Liquid temperature at grid cell *𝑗* from SPH particles (K)

### Runtime Performance

Fire-X: Extinguishing Fire with Stoichiometric Heat Release

A.3 Parameter Tables In Tab. 4 we provide values for parameters that depend on the physical and chemical properties of materials, liquids and gases – these parameters are fixed. In Tab. 3 we provide values for parameters that we used to generate the results in this paper. These parameters can be configured to account for various different scenarios such as for complete combustion, different liquid spray configurations, etc.
Table 3. Configurable Parameters
 Simulation Parameters Delta Time Ambient Temperature Particle System Parameters Particle Capacity Update Multiplier Smoothing Radius Stiffness Near Stiffness Rest Density (pseudo) Linear Impulse Quadratic Impulse Grid Parameter Grid Size Grid Length Pressure Iterations Thermal Parameters Radiation coefficient Density temperature coupling limit Liquid Parameters Liquid Droplet Diameter Liquid Displacement Factor Vorticity confinement Strength Lower velocity threshold Upper velocity threshold Lower temperature threshold Combustion Parameters Carbon dioxide reaction coefficients Water vapor reaction coefficients Residual reaction coefficient Heat Efficiency Particle Emitter Parameter Mass Velocity Lifetime Frequency Temperature Spray Angle Grid Emitter Parameter Fuel Mass Fraction Oxygen Mass Fraction Nitrogen Mass Fraction Temperature Velocity
Value 1 / 120 s

300.0 K Value 131072 - 1048576 1 - 4
0.06 - 0.4 m
0.004 - 0.005
0.01 - 0.02
1.0 - 300.0
0.0
0.4 Value 64×64×64 - 200×300×400
0.1 - 10.0 m 64 - 128 Value
0.0 - 6.0 300 - 3000 K Value
0.0005 - 0.005 m
0.0 - 0.4 Value
0.0 - 50.0
0.0 - 0.1
0.0 - 5.0 301 K Value
0.0 - 10.0
0.0 - 10.0
0.0 - 10.0
0.0 - 1.0 Value
0.1 - 1.0 kg
0.0 - 10.0 m s−1
0.0 - 120.0 s
10.0 - 100.0 Hz
300.0 K
0.0 - 180.0◦ Value
0.1 - 1.0
0.1 - 1.0
0.1 - 1.0 300 - 1500 K
0.0 - 10.0 m s−1
Gas Parameters Density Species diffusion coefficients Specific heat capacity Thermal conductivity Liquid Parameters Thermal conductivity Specific heat capacity Boiling Temperature Heat of vaporization Solid Parameters Thermal conductivity Specific heat capacity Boiling Temperature Combustion Parameters Activation energy Arrhenius pre-exponential factor Arrhenius temperature exponent Lower heat of combustion Fuel reaction exponent Oxygen reaction exponent Fuel reaction coefficients Oxygen reaction coefficients Nitrogen reaction coefficients Fuel molar mass Fuel heat capacity Fuel thermal conductivity

- 268:17
Table 4. Fixed Parameters

Reference Air (depending on temperature) Usually in Air or Nitrogen [Lide 1995] Mixture temperature averaged [Linstrom and Mallard 2025] Mixture at ambient temperature [Linstrom and Mallard 2025] Reference Water or Ethanol [Linstrom and Mallard 2025] Water or Ethanol [Linstrom and Mallard 2025] Water or Ethanol [Linstrom and Mallard 2025] Water or Ethanol [Linstrom and Mallard 2025] Reference Aluminum or Iron [Linstrom and Mallard 2025] Aluminum or Iron [Linstrom and Mallard 2025] Aluminum or Iron [Linstrom and Mallard 2025] Reference Fuel type [Westbrook and Dryer 1981] Fuel type [Westbrook and Dryer 1981] 0 [Westbrook and Dryer 1981] Fuel type [Schmidt-Rohr 2015] Fuel type [Westbrook and Dryer 1981] Fuel type [Westbrook and Dryer 1981] Fuel type Fuel type 0 Fuel type [Linstrom and Mallard 2025] Fuel type temperature averaged [Linstrom and Mallard 2025] Fuel type at ambient temperature [Linstrom and Mallard 2025]

