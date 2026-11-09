# Foliage card audit

2026-11-08: `Scripts/Assets/audit_foliage_cards.py` measures empty alpha-card
surface area independently of triangle count. Run:

```powershell
python Scripts/Assets/audit_foliage_cards.py --output .local/foliage-overdraw/card-area.json
python .local/tests/test_foliage_cards.py
```

The current asset inventory contains 72 oak prefabs with 72 masked mesh parts,
including impostors. Estimated empty surface fractions range from **40.65% to
81.39%**. For Big_oak_tree_001, the estimates are 66.47% at LOD0, 65.70% at
LOD1, 64.81% at LOD2 and 49.65% on the impostor. Similar triangle counts therefore
do not describe the amount of empty card surface.

The estimator uses equal-area triangle quadrature at 64 and 256 samples per
triangle, native UV0, repeat addressing, nearest base-mip texture alpha and the
material alpha cutoff. It weights each triangle by its mesh-local surface area,
including both triangles of a card. The maximum difference between the two
sampling densities is **1.51 percentage points**; this is a convergence diagnostic,
not a statistical error bound. Prefabs, meshes, materials and base textures are
SHA-256 fingerprinted before measurement and checked afterward. The artifact
records each part, both estimates and the complete input identities.

These estimates exclude prefab transforms, projected area, material alpha/tint
overrides, filtering, mip selection, wind, visibility, depth rejection and
overlapping surfaces. They do not measure actual shader invocations, discarded
GPU fragments, memory traffic or time, and must not be interpreted as recoverable
frame cost. Impostors and geometric canopies remain separate asset representations.
The software sampling convention follows the existing canopy asset screener;
actual renderer captures remain required.

Four regression checks cover fully opaque/empty textures, triangle-area versus
whole-texture occupancy, negative repeated UVs and invalid sampling density.
No renderer or asset defaults changed.

The foliage-overdraw TODO remains open. Next measure projected card overlap and
empty-space contribution with matched color and shadow GPU controls, retaining
geometry, camera, wind, LOD and effective settings. Compare tighter cards and
interior foliage candidates with coverage and motion validation. Evaluate draw
ordering or a prepass only with measured total-pass savings, including extra
submissions and geometry. Existing material studies and zero-fragment controls
provide useful baselines but do not independently attribute these costs.

## Matched GPU control measurements

2026-11-08: captured the existing Release `ForestPerformanceAudit.exe` with
assertions disabled, shadows and asynchronous workload counters on DX11, DX12
and Vulkan. All three use the same executable hash and asset/shader input hash.
Each backend has its own driver/build/content/settings provenance, recording
hashes and verified unchanged pre/post inputs. Commands, using a fresh directory:

```powershell
python Scripts/run_forest_performance.py .local/foliage-overdraw/dx12-shadow --backend dx12 --width 1920 --shadows --counters
python Scripts/compare_foliage_overdraw.py .local/foliage-overdraw/dx12-shadow
```

Repeat with `dx11` and `vulkan` in both arguments. Existing captures occupy these
directories; use new paths to repeat. The full harness records 1,188 frames per
backend. The new comparison validates 288 selected color samples and 216 selected
shadow samples per backend: 36 per control/resolution after 32 warmup frames.
Both 1920x1080 and 960x540 use the authored entrance camera and frozen simulation.
Color is isolated to leaf/card meshes, including impostors, with the same LOD,
shader states, active material slots, draws and exact executed VS/IA counts.

| Backend | Resolution | Leaf color ms | Zero-fragment ms | Unlit, alpha retained ms | Cutoff zero ms |
|---|---|---:|---:|---:|---:|
| DX11 | 1920x1080 | 6.636 | 2.014 | 4.671 | 5.416 |
| DX12 | 1920x1080 | 6.402 | 1.929 | 4.612 | 5.353 |
| Vulkan | 1920x1080 | 5.712 | 2.030 | 5.118 | 4.664 |
| DX11 | 960x540 | 3.934 | 2.022 | 3.107 | 3.564 |
| DX12 | 960x540 | 3.828 | 1.879 | 3.006 | 3.410 |
| Vulkan | 960x540 | 3.695 | 2.027 | 3.289 | 3.403 |

The zero-fragment control uses an empty scissor with unchanged shaders and
geometry; counters verify zero PS invocations. At 1080p the measured incremental
raster/fragment/depth/color residual is 4.622/4.473/3.682 ms for DX11/DX12/Vulkan.
This residual does not isolate overlapping cards, alpha sampling or wasted depth
work. Unlit retains alpha tests but changes lighting work. Cutoff zero changes
alpha sampling, coverage, early rejection and later fragment work simultaneously;
it fills canopy holes and is not a proposed replacement or pure texture-cost test.
For example DX12 PS invocations fall from 36.53 to 21.70 million with cutoff zero;
this is executed workload, not an overdraw ratio or measured texture reads.

| Backend | Color resolution | Reference shadow ms | Unlit shadow ms | Cutoff-zero shadow ms |
|---|---|---:|---:|---:|
| DX11 | 1920x1080 | 1.095 | 1.038 | 1.024 |
| DX12 | 1920x1080 | 0.975 | 0.973 | 0.949 |
| Vulkan | 1920x1080 | 1.036 | 1.043 | 1.023 |
| DX11 | 960x540 | 1.039 | 1.004 | 1.016 |
| DX12 | 960x540 | 0.972 | 0.972 | 0.940 |
| Vulkan | 960x540 | 1.031 | 1.031 | 1.016 |

Shadow timings average the existing staggered refresh schedule, not four freshly
rendered cascades every frame. The comparator requires exactly matching joint
VS/IA workload histograms and frequencies: four workloads, nine samples each.
This avoids rejecting legitimate cascade variation or accepting unequal refresh
distributions. Empty color scissor is not treated as a shadow vertex-only control.
These shadow scopes reflect the harness's caster policy, not a proven isolated
leaf-only full-atlas cost. Color resolution does not specify shadow resolution.
DX12 submits diagnostic shadow/color work as separate renderer frames; wall FPS
from this harness is not production Editor FPS.

Artifacts are `.local/foliage-overdraw/{dx11,dx12,vulkan}-shadow/`, each containing
`provenance.json`, the raw recording, screenshots, existing summary and new
`foliage-ablation.json`. These are single-process sequential controls per backend,
without alternating-order repetitions; small shadow deltas and backend timing
differences are descriptive, not accepted speedups. No assets or quality policy
changed. Seven new comparison tests and eleven existing forest-comparison tests
pass, covering identity/geometry changes, cascade distribution, repeated samples,
nonfinite timing and missing controls.

Remaining work: directly measure projected overlap/coverage and depth rejection;
isolate alpha texture work with a coverage-preserving control in both passes;
then evaluate tighter cards/interior foliage and any ordering/prepass candidates
with repeated timing and wind/coverage validation. The asset empty-area estimate
and these GPU controls must not be multiplied to predict savings.

## Projected overlap screen

2026-11-08: `Scripts/Assets/audit_foliage_overlap.py` screens the four installed
`Large_oak_tree_*_Canopy.prefab` leaf meshes. It fingerprints the exact prefab,
material, mesh and base-texture bytes during discovery and verifies them after
measurement. Unsupported transforms, vertex-alpha materials, alternate UV sets
and nonunit material alpha multipliers are rejected. Run:

```powershell
python Scripts/Assets/audit_foliage_overlap.py --output .local/foliage-overdraw/projected-overlap.json
python .local/tests/test_foliage_overlap.py
```

The artifact contains 32 cases: four canopies, four fixed views and two viewport
sizes (128 and 256 square). Perspective ground/elevated views and orthographic
low/high light-direction screens use explicit recorded azimuth/elevation and
mesh-local bounds. Light views are generic asset screens, not the scene sun,
shadow atlas or renderer cascades. Perspective cameras use 50-degree FOV and a
distance of three bounding-sphere radii. Orthographic view width is 2.2 radii.

At 256 square pixels, the screen reports:

| Installed canopy | Ground accepted layers/pixel | Elevated layers/pixel | Low-light layers/pixel | High-light layers/pixel |
|---|---:|---:|---:|---:|
| Large oak 001 | 2.540 | 3.052 | 2.892 | 2.594 |
| Large oak 002 | 2.476 | 2.462 | 2.459 | 2.432 |
| Large oak 003 | 2.665 | 3.006 | 2.607 | 2.793 |
| Large oak 004 | 2.306 | 2.025 | 2.362 | 2.100 |

The denominator is the union of alpha-covered leaf pixels, excluding background.
Across these views, **54.03–71.03%** of that union has multiple alpha-covered
triangle layers. **64.74–67.80%** of raw projected card-triangle samples fail the
base-mip alpha test. Empty-space and overlap counts are separate: rejected alpha
samples do not count toward accepted layers. Rasterization is two-sided with
top-left shared-edge ownership, perspective-correct UV interpolation, pixel-centre
sampling and repeat addressing. A card's shared diagonal is counted only once.

Doubling screen size changes matched accepted-layer ratios by at most **2.24%**
relative, empty sample fractions by **1.03 percentage points**, and multiply
covered fractions by **1.12 percentage points**. These comparisons reveal sampling
sensitivity; they are not error bounds or native-resolution renderer validation.

All layers are counted before depth rejection. `excess_accepted_layers` is accepted
samples minus their union; it is potential hidden coverage, not measured wasted
fragment work. Counts are independent of submission order and include coplanar
overlap. The screen excludes bark/other occluders, other trees, authored forest
placements, wind, material overrides, mip filtering and GPU scheduling. It cannot
predict early/late depth behavior, fragment-quad work or elapsed GPU cost. Do not
multiply these ratios by the prior GPU timings to estimate recoverable time.

Eight synthetic checks validate shared-edge ownership, winding, stacked layers,
triangle-order independence, empty alpha, perspective interpolation, camera-plane
rejection and degenerate triangles. No geometry, renderer policy or authored
quality changed. Direct GPU alpha/depth attribution and coverage/wind-validated
candidate evaluation remain open.

## Alpha/depth control and prepass protocol

`Scripts/run_foliage_prepass_study.py` adds an opt-in experiment, with three
independent processes per backend and alternating reference/prepass order:

```powershell
python Scripts/run_foliage_prepass_study.py .local/foliage-overdraw/prepass-study-equal --record-rejected
```

Use a new directory when repeating. The runner freezes the executable/DLLs and
CMake cache, retains per-process input/recording/image hashes and verifies repeated
build, backend, environment, platform and content identities. Each process measures
1080p and 540p, with full shadow refresh and the same camera/frozen simulation.
The comparator rejects image differences by default. `--record-rejected` retains
the explicit failed quality gate and difference counts while continuing timing
collection; it never marks the candidate quality accepted. `--resume` revalidates
completed captures and continues the same frozen runtime after interruption.

The alpha-mask wrapper calls the existing `PSShadowMain` alpha/fade/clip logic
with the unchanged color vertex entry point, and outputs a saturated diagnostic
mask. It retains native leaf texture, filtering, UV selection, cutoff, dither and
wind geometry. The renderer explicitly recognizes this wrapper's vertex contract,
preserving the reference batching, GPU instance culling and LOD selection. Mask
activation is checked before measurement; background pixels must match the
reference. The mask versus same-shader zero-fragment pair measures combined
raster/alpha/depth/color cost, not pure texture-fetch time.

The candidate first renders that alpha-only pass, then the full object shader
with read-only **equal** depth. The normal opaque pipeline uses strict less,
which cannot shade prepopulated depth; the equal-depth state is an explicit,
nonserialized Scene diagnostic option, disabled by default. Pipeline activation
is checked before measurement. Both passes retain alpha tests. The diagnostic
prepass also writes color, so this is a conservative two-color-pass experiment,
not an optimized color-write-disabled depth-only implementation.

Acceptance requires unchanged reference geometry for mask/control cases, exactly
twice the executed VS/IA work for the candidate, both passes' physical submissions,
matching complete shadow work and byte-identical final reference/candidate images.
The combined `TreeLeaves` GPU scope contains both color passes; never report just
the second shading pass as the candidate cost. Frame-wall means include CPU work
and full shadow diagnostics and are not production FPS. DX11 screenshots use its
pre-Present request/read API; its tone mapping differs from the explicit backends'
scene readback, so comparisons remain within each backend.

Exploratory `prepass-dx12-1`, `prepass-proof-dx12`, `prepass-dx12-v2`, `prepass-study`
and `prepass-study-v2` directories are rejected diagnostics. They exposed an
overly strict mask activation check (colored sky and DX11 tone mapping), changed
custom-shader geometry policy, and missing equal-depth state respectively. They
are not accepted performance or quality evidence. Their manifests and failed
proof images remain available to explain the gates.

### Completed measurement and rejection

The `prepass-study-equal/study.json` artifact has `complete=true`: all nine
processes finished with unchanged inputs/executables and matching repeated/backend
build identities. The comparator verifies **2,592 color and 2,592 shadow workload
samples**, 54 hashed images and 18 final-image pairs. Each resolution/backend has
three independent processes, including reversed reference/prepass order.

Every final-image pair fails exact equality. Difference counts repeat identically
within each backend; the unchanged-background mask check reports zero mismatches.
This passes that limited mask check, not a moving/wind or all-layer coverage test.

| Backend | Resolution | Changed final pixels | Maximum RGB channel difference |
|---|---|---:|---:|
| DX11 | 1920x1080 | 8,262 | 48 |
| DX12 | 1920x1080 | 7,257 | 39 |
| Vulkan | 1920x1080 | 7,346 | 39 |
| DX11 | 960x540 | 2,182 | 26 |
| DX12 | 960x540 | 1,868 | 50 |
| Vulkan | 960x540 | 1,879 | 50 |

The equal-depth option restores the intended foliage drawing, but does not make
the two-pass output identical. Equal depth can allow different tie handling from
strict less; this study did not trace winning primitives and does not establish
the cause of the residual differences. No renderer defect is asserted.

Measured **combined two-pass minus reference** leaf GPU time follows. Positive
values are slower. Ranges cover the three paired process means, not confidence
intervals; rejected images mean even negative deltas are not accepted savings.

| Backend | Resolution | Paired median ms | Paired range ms |
|---|---|---:|---|
| DX11 | 1920x1080 | +0.316 | -0.282 to +1.665 |
| DX12 | 1920x1080 | -1.675 | -1.793 to +5.498 |
| Vulkan | 1920x1080 | +2.802 | -4.248 to +10.009 |
| DX11 | 960x540 | +2.217 | +1.725 to +2.753 |
| DX12 | 960x540 | +2.363 | +0.507 to +3.518 |
| Vulkan | 960x540 | +11.482 | +2.977 to +11.855 |

All reference/mask/zero-fragment cases execute 4,648,644 VS invocations and
1,549,548 primitives. The two-pass case executes exactly twice each, and total
color submissions rise from 102 to 204. These are measured selected leaf/card
workloads, not forest source-triangle counts. The combined query scope includes
both passes. Legacy per-render CPU fields and configured material/shader-path
counts describe the final shading call; use frame-wall means for complete CPU
cost and count the additional mask shader separately. Uninstrumented sky/display
GPU work is not included in the leaf scope. These full-refresh, isolated forest
diagnostics are not comparable to historical steady Editor FPS.

The same-shader alpha-mask versus zero-fragment residual provides the following
combined raster/alpha/depth/color diagnostic. It cannot isolate texture-fetch cost.

| Backend | Resolution | Residual median ms | Process range ms |
|---|---|---:|---|
| DX11 | 1920x1080 | 2.558 | +0.660 to +3.492 |
| DX12 | 1920x1080 | 0.029 | -1.257 to +3.120 |
| Vulkan | 1920x1080 | 0.983 | +0.655 to +6.899 |
| DX11 | 960x540 | 1.608 | -2.475 to +1.710 |
| DX12 | 960x540 | 2.727 | -0.160 to +2.840 |
| Vulkan | 960x540 | 1.378 | +0.897 to +7.935 |

Negative residuals are retained, exposing substantial timing variability and
overlap/noise rather than being clamped into a physical stage cost. No consistent
1080p gain or pure alpha-texture cost is established. Every 540p pair is slower.
Reject this prepass candidate; preserve authored quality and default rendering.
A color-write-disabled depth-only implementation was not measured and must not
inherit these numbers or an acceptance claim.

The Release audit builds. All 46 selected Python regressions pass, including 12
new prepass-comparison checks for geometry/submission accounting, pipeline
activation, image/background gates and explicit recording of rejected quality.
The main foliage-overdraw task remains open for a coverage-preserving isolated
alpha-texture/depth attribution and tighter-card/interior foliage candidates with
renderer, motion and wind validation. This completed prepass experiment supplies
a reproducible rejection, not a default optimization or sustained-FPS claim.

## Mip-aware transparent-border crop screen

2026-11-08: screened a global rectangular border crop before generating any
geometry. All four installed canopies share the 4096x4096 leaf atlas
`Texture 6 6.png`, with alpha cutoff 0.32. The base-mip alpha reaches the right
edge; repeat-addressed bilinear filtering also protects the left edge. A
conservative one-texel support guard proposes UV bounds
`[0, 0.0087890625, 1, 0.993408203125]`. This removes 1.538% of the texture
rectangle, **not** that fraction of the native card surface or GPU work.

```powershell
python Scripts/Assets/screen_foliage_trim.py --output .local/foliage-overdraw/trim-screen.json
python .local/tests/test_foliage_trim.py
```

The screen mirrors `Texture.cpp`'s linear RGBA8 alpha reduction, including float32
accumulation/rounding and floor-sized odd mips. It checks the full 13-level chain
with pixel-centre bilinear/repeat sampling. It evaluates removed native triangle
surface samples at 256 and 1,024 equal-area points per triangle. Mesh positions,
UVs, materials and textures are unchanged. The report fingerprints 13 inputs,
including the estimator and native mip-generation source, and verifies them
afterward; output/input aliasing is rejected.

| Installed canopy | Estimated native leaf surface removed, 1,024 samples/triangle | Result |
|---|---:|---|
| Large oak 001 | 0.03421% | Reject: visible removed sample at mip 9 |
| Large oak 002 | 0.03439% | Reject: visible removed sample at mip 9 |
| Large oak 003 | 0.03447% | Reject: visible removed sample at mip 9 |
| Large oak 004 | 0.03441% | Reject: visible removed sample at mip 9 |

The coarser quadrature estimates removal at 0.03102–0.03127%; the small area
estimate is sampling-sensitive. Rejection witnesses occur at both sampling
densities for every canopy. For canopy 001, triangle 6 contains the removed
native UV point `(0.1616612071, 0.0033524777)`: base alpha is zero, but the mip-9
bilinear alpha is **0.354329**, above the **0.32** cutoff. The artifact records
triangle IDs, barycentric coordinates, mesh-local positions and alpha witnesses.

Reject this unrestricted border crop under the strict all-mip preservation
screen. This is a potential coverage loss on native triangle surfaces, not a
measured visible forest hole: occlusion, actual derivative/mip demand, anisotropic
footprints, camera routes and wind are not evaluated. Integer-mip witnesses are
enough to reject this strict candidate; absence of a sampled witness would not
prove safety. The model does not establish that mip 9 occurs at this point on
the authored route. A crop restricted to a proven mip range would need separate
renderer/coverage validation. No geometry candidate was generated or installed,
and no timing/FPS benefit is claimed.

The native meshes already omit much of the atlas border, explaining the small
surface reduction despite larger texture-rectangle reduction. This does not rule
out shape-aware trimming around interior transparent regions. Such candidates
must account for filtering across mips, repeated UVs, extra geometry, attribute
interpolation and wind before GPU color/shadow measurements.

Eight new regressions cover alpha mip rounding, odd-size handling, bilinear texel
centres/repeat seams, opposite-edge crop protection, a coarse-mip rejection of
base-transparent geometry, unchanged full bounds, quadrature and invalid inputs.
All 54 selected Python checks pass. The main overdraw TODO remains open.

## Adaptive interior-cut candidates

2026-11-08: built 20 isolated candidates: four installed canopies at five adaptive
subdivision depths. Reproduce in a fresh directory under `.local`:

```powershell
python Scripts/Assets/build_foliage_interior_trim.py --output .local/foliage-overdraw/interior-trim-final --depths 1 2 3 4 5
python .local/tests/test_foliage_interior_trim.py
```

The final evidence is `.local/foliage-overdraw/interior-trim-final/report.json`,
with `complete=true` and `approved_for_installation=false`. Its 40 hashed artifacts
are 20 native meshes and 20 prefabs, grouped by depth/variety. Bark references,
materials and authored scene/LOD policy are retained; no candidate is installed.
Each row identifies the installed canopy prefab as its required baseline.

The cutter subdivides source triangles at edge midpoints. A subtriangle is
removed only when its UV bounding box cannot touch any texel reaching the alpha
cutoff, at any mip, including wrapped bilinear neighbours. The integral-image
support bound is conservative; trilinear blends cannot exceed both endpoint
mips at the same UV. Unchanged subdivision branches collapse back to the source
triangle, avoiding needless geometry. Thirteen triangles owning native extrema
are protected in each variety, and exact source position extrema are checked.
Each variety has 30 unique UV triangle patterns, so plans are reused across
repeated geometry without changing authored placements or instancing.

| Maximum subdivision depth | Total leaf triangles, four varieties | Source triangle ratio | Planned native leaf surface removed, variety range |
|---|---:|---:|---|
| Original / depth 1 | 33,408 | 1.000x | 0% |
| Depth 2 | 33,408 | 1.000x | 0% |
| Depth 3 | 44,240 | 1.324x | 0.0736–0.0840% |
| Depth 4 | 117,354 | 3.513x | 0.4297–0.4521% |
| Depth 5 | 231,130 | 6.918x | 0.7022–0.7319% |

Depth-1/2 mesh files are byte-identical to their installed leaf sources. Deeper
files pass native v7 layout/size/finite-vertex checks, artifact hashes verify,
and original bounds remain exact. Removal fractions come from the subdivision
plans and source triangle areas, rather than projected pixel counts. These are
source geometry counts, not submitted/surviving GPU geometry or timing results.

Set these candidates aside at the geometry screen: less than 0.74% of native
surface is removed even at nearly seven times the leaf triangle count. This is
a poor tradeoff for this conservative midpoint/bounding-box algorithm; it is
not a measured GPU slowdown or a limit on other shape-aware trimming methods.
The support bound can retain transparent regions and therefore understates the
area a more precise contour method could remove. No timing/FPS saving is claimed.

Native attributes are interpolated at inserted vertices; skeletal/mode fields
must be constant on their source triangle. This does not establish shaded-image
or wind equivalence: vertex normalization and wind deformation can be nonlinear.
The support screen also does not cover anisotropic sample footprints, actual
forest visibility, motion or rendered shadows. These candidates receive no
renderer/quality acceptance, and no GPU suite is warranted for the screened
tradeoff. A future candidate must pass those checks before adoption.

For future hardware comparisons, use the installed `*_Canopy.prefab` baseline
recorded here. `ConfigureCanopy` retains its historical original `*_LOD1.prefab`
default; the contour follow-up below adds an explicit installed-baseline mode
and verifies all four selected prefab paths in each capture log.

Nine new regressions cover empty/opaque support, repeat seams, coarse-mip
protection, barycentric partition/area accounting, branch collapse, protected
extrema and unsupported animation/UV/vertex data. All 63 selected Python checks
pass. The remaining overdraw work includes tighter contour methods or simpler
interior foliage, exact footprint/coverage validation and measured color/shadow
cost; the authored assets and rendering defaults remain unchanged.

## Filtered UV contours: triangle-preserving DX12 pilot

`Scripts/Assets/build_foliage_contour.py` stages tighter shapes using a 512-square
UV support grid. Each cell bounds mathematical bilinear alpha over its entire
area at every native mip; their union also bounds trilinear blends. Supported
cells intersecting a source triangle define a convex hull. Barycentric clipping
preserves interpolated native attributes, while the same 13 extrema-owning
triangles per variety remain protected. This screen does not establish GPU
filter precision, anisotropic footprints, nonlinear shading or wind equivalence.

Reproduce the 12 staged candidates with:

```powershell
python Scripts/Assets/build_foliage_contour.py --output .local/foliage-overdraw/contour-triangle-study --sizes 512
python .local/tests/test_foliage_contour.py
```

Use a fresh output directory; the generator refuses to overwrite evidence.
The report hashes all 24 mesh/prefab artifacts and records unchanged inputs,
`complete=true` and `approved_for_installation=false`.

| Clip policy | Leaf triangle ratio | Native surface removed, variety range |
|---|---:|---:|
| One cut retaining one triangle | 1.000x | 1.308–1.334% |
| Best single cut | about 1.832x | 5.01–5.03% |
| Full supported-cell hull | 6.70–6.72x | 7.44–7.46% |

Only the first policy proceeds to a renderer pilot: all 33,408 leaf triangles
remain, and all four complete canopies retain 36,297 triangles including bark.
The larger candidates remain screened experiments without measured GPU results.

`Scripts/run_canopy_study.py --baseline installed` selects the installed canopy
prefabs and rejects captures missing any of their four exact paths in the log.
`--runtime-source build/Debug/Release` freezes the locally built Release capture
executables/DLLs. Completed manifests hash inputs, candidate assets, runtimes,
PPM images and timing recordings; the timing summarizer verifies recorded data.
The historical original-baseline mode remains available for earlier studies.

The DX12 pilot used the `triangle-512` candidate, GPU validation and 48 matched
960x540 images: entrance/overlook, 60/30-degree FOV and 12 camera positions each.
Simulation is frozen, authored oak wind is zero, and shadows refresh fully.
Four repeat pairs have zero changed pixels. Across candidate comparisons, the
largest fraction of pixels changing by more than three RGB levels is 0.01852%;
the maximum mean channel delta is 0.003269, maximum channel delta is 191, and
maximum temporal changed fraction is 0.03434%. The worst inspected difference
changes a leaf-edge pixel to sky: small aggregate error is not exact coverage.

Quality evidence is `.local/foliage-overdraw/contour-dx12-quality/`:
`quality-provenance.json`, `analysis.json` and `worst-edge.png`; original captures
and comparison storyboards are under the candidate's `renderer/dx12/1` folder.
This is a pilot, not quality acceptance. Active wind, 1080p image comparisons,
DX11 anisotropy and Vulkan remain unvalidated; no candidate is installed.

Capture commands (use fresh evidence directories and the built Release targets):

```powershell
python Scripts/run_canopy_study.py .local/foliage-overdraw/contour-triangle-study/triangle-512 --backends dx12 --quality-only --baseline installed --runtime-source build/Debug/Release --study-output .local/foliage-overdraw/contour-dx12-quality
python Scripts/run_canopy_study.py .local/foliage-overdraw/contour-triangle-study/triangle-512 --backends dx12 --timing-only --baseline installed --runtime-source build/Debug/Release --study-output .local/foliage-overdraw/contour-dx12-timing-verified
```

Nine contour regressions cover conservative sampled bounds, repeat seams,
coarse-mip support, opaque identity, triangle/cell intersection, clipping area,
retained supported points, winding and invalid inputs. Six added canopy checks
cover installed-baseline logs and complete, untampered recording provenance.
All 81 selected Python checks pass. Both Release capture targets build.

The verified timing suite completes three independent alternating-order DX12
processes at 1920x1080, four views and 180 measured frames per variant/view:
4,320 measured frames, all with valid color/shadow GPU and workload queries.
Exact matched-frame color VS/primitive counts and per-view shadow VS/primitive
histograms agree in every process. Each process logs all four installed baseline
paths; content, candidate and frozen-runtime identities remain unchanged.

| View | Paired candidate-minus-baseline tree + full-shadow time, processes 1 / 2 / 3 (ms) | Leaf PS change range | Shadow PS change range |
|---|---|---|---|
| Entrance | +8.332 / -3.713 / -5.389 | -3.70% to +10.03% | -2.57% to +0.59% |
| Overlook | -4.755 / +12.074 / +1.885 | -16.12% to +15.83% | -2.42% to -1.52% |
| Zoom | -2.816 / +5.788 / -16.205 | -20.54% to +15.96% | -2.42% to -1.53% |
| Motion | -4.256 / -5.544 / +3.241 | -12.06% to +11.23% | -2.49% to -1.37% |

Every view has mixed paired timing signs. Color fragment counts also change
sign across runs; they do not demonstrate a stable reduction in alpha texture
work. Shadow fragment counts are consistently lower for the three elevated
views, but entrance is mixed, and these counts are hardware shader invocations,
not accepted leaf coverage or an isolated estimate of texture/depth cost.
No repeatable timing benefit or production FPS improvement is established.
Do not adopt this candidate: retain its staged artifacts as an unsuccessful
pilot, with wind and cross-backend quality acceptance incomplete.

Timing evidence is `.local/foliage-overdraw/contour-dx12-timing-verified/`:
`timing-provenance.json` (`complete=true`), `timing-summary.json`, `analysis.json`
and three hashed recordings/logs. The earlier timing attempt under the quality
directory is incomplete because it lacked baseline-path logging; it is excluded.
The main overdraw TODO remains open for a different candidate or attribution
method; authored assets, wind and rendering defaults remain unchanged.

## Interior contribution screen: reject sparse-view redundancy pruning

`Scripts/Assets/screen_foliage_interior.py` measures whether installed leaf
triangles can be removed while retaining sampled alpha coverage. It reuses the
software overlap rasterizer with an optional accepted-pixel visitor; existing
count-only results are unchanged. This is frozen, mesh-local, nearest-base-mip,
pre-depth screening. It excludes bark, forest placements, filtering, wind and
renderer shadow cascades, and reports triangles rather than inferred leaf cards.

```powershell
python Scripts/Assets/screen_foliage_interior.py --output .local/foliage-overdraw/interior-contribution-screen.json
python .local/tests/test_foliage_interior.py
```

The output must be a fresh file under `.local`. Eight training projections use
the earlier ground/elevated perspective and low/high orthographic light views,
each at 128 and 256 square. Triangles owning native position extrema are protected.
The greedy plan considers low sampled contributions first, with source-index tie
breaking, and removes a triangle only if every accepted pixel is still covered
by another remaining triangle in every training projection. Counts are updated
after each removal, preventing joint removal of the last covering triangle.

Eight disjoint 256-square holdouts use four other perspective directions and
four other orthographic light directions. These views are evaluated after the
plan is frozen, without repairing the plan against their results. Across four
varieties this produces 32 training and 32 holdout cases.

| Variety | Source leaf triangles | Planned retained triangles | Removed | Holdout lost leaf pixels, fraction of reference union |
|---|---:|---:|---:|---:|
| 001 | 7,830 | 4,258 | 45.62% | 6.19–13.94% |
| 002 | 7,836 | 4,553 | 41.90% | 5.71–10.69% |
| 003 | 11,730 | 5,768 | 50.83% | 8.83–15.02% |
| 004 | 6,012 | 3,792 | 36.93% | 5.24–10.65% |

All training unions are exactly preserved at their sampled pixel centres, yet
every holdout loses coverage. Reject this sparse-view redundancy strategy before
generating replacement meshes or running GPU timings. The planned 33,408 to
18,371 triangle reduction is not a usable optimization, a hardware work saving,
or evidence that other interior simplification methods must fail. These losses
are software projection witnesses, not observed holes in a rendered forest.

Evidence: `.local/foliage-overdraw/interior-contribution-screen.json` has
`complete=true`, `inputs_unchanged=true`, `approved_for_installation=false`, input
hashes, removed source indices and all per-view coverage/sample counts. Input
hashes and every zero-loss training case were independently checked after capture.
Only an index plan/report is written; no candidate mesh or prefab is produced.

Eight new tests cover sequential shared coverage, uniqueness in any training
view, protected triangles, unseen holdout contribution, inconsistent counts,
duplicate triangle samples, missing training data and reconstruction of existing
raster counts through the visitor. All 89 selected Python checks pass. Assets,
wind, scene layout and renderer defaults remain unchanged; the overdraw audit
remains open for broader coverage constraints or a different measured method.

## Triangle ordering screen: fixed entrance order is direction-dependent

`Scripts/Assets/screen_foliage_order.py` replays projected triangle samples
against an ideal per-sample early-depth model using LESS_EQUAL, matching the
renderer's depth comparison. Alpha-rejected samples never update depth. The
model counts card samples eligible after depth and accepted depth writes;
these are not hardware shader invocations, texture reads or GPU milliseconds.
It assumes depth rejection before alpha evaluation and excludes quad/helper
lanes, hierarchical depth behavior, scheduling, bark, forest instances, wind,
filtering and mip selection. Actual hardware need not realize its savings.

```powershell
python Scripts/Assets/screen_foliage_order.py --output .local/foliage-overdraw/ordering-screen.json
python .local/tests/test_foliage_order.py
```

The fresh report records 48 cases: four installed canopies, eight perspective
camera directions and four orthographic light directions at 256 square. Four
orders replay exactly the same sample lists: native, fixed centroid-depth order
for entrance azimuth 17/elevation -12, per-view centroid front-to-back and
per-view centroid back-to-front. The two per-view orders are diagnostic proxies,
not optimal bounds or implemented runtime candidates. Final sampled depth is
exactly equal under all four orders in every case; this does not establish
shaded-image equality, particularly for equal-depth triangles with different
attributes. The existing raster counts remain unchanged with visitors disabled.

| Variety | Fixed entrance order: eligible sample change range | Fixed order lower / higher cases | Per-view front-to-back: change range |
|---|---|---|---|
| 001 | -41.87% to +76.50% | 4 / 8 | -43.02% to -20.92% |
| 002 | -27.07% to +68.12% | 4 / 8 | -41.36% to -20.53% |
| 003 | -36.69% to +86.99% | 4 / 8 | -41.68% to -22.02% |
| 004 | -28.43% to +55.00% | 4 / 8 | -38.28% to -14.74% |

Set aside the fixed entrance order as a general forest asset policy: it increases
modeled eligible work in 32 of 48 cases. This result does not rule out other
static orders. Per-view front-to-back reduces modeled work in all cases, while
back-to-front increases it by 27.12–108.73%; direction matters in this model.
A view-dependent runtime candidate would need to account for independently
rotated/placed instances, camera and shadow views, sorting/submission overhead,
cache behavior and temporal/shaded correctness before any adoption or GPU claim.
No replacement meshes are generated, no runtime sorting is added and no GPU
timing benefit is claimed by this screen.

Evidence: `.local/foliage-overdraw/ordering-screen.json` has `complete=true`,
`inputs_unchanged=true`, `approved_for_installation=false`, hashed source inputs,
view definitions and all four sets of counts per case. Hashes were independently
verified after the completed run. An initial strict-LESS run was discarded by
the changed-input guard when the comparison rule was corrected; it produced no
report and is excluded. Eight new regressions cover order dependence, alpha
rejection, hidden transparent samples, equal depth, invalid permutations/depth,
empty triangles and perspective-correct visitor depths. All 97 selected Python
checks pass. Assets, wind, scene layout and rendering defaults remain unchanged.

## Direction-bank feasibility: six orders merit an indexed runtime experiment

`Scripts/Assets/screen_foliage_order_bank.py` screens precomputed triangle
permutations for six cardinal local directions and fourteen directions including
eight cube diagonals. It reuses the prior ideal LESS_EQUAL early-depth replay
and its limitations; this is not GPU timing, alpha-texture attribution or image
acceptance. The same 48 camera/light cases choose the nearest bank direction
by dot product. Orders sort triangle centroids toward that direction, retaining
every source triangle and vertex attribute. The report stores permutation hashes
and metrics, without producing replacement assets or installing renderer changes.

```powershell
python Scripts/Assets/screen_foliage_order_bank.py --output .local/foliage-overdraw/directional-order-bank-screen.json
python .local/tests/test_foliage_order_bank.py
```

| Variety | Six-order eligible sample change versus native | Fourteen-order change versus native |
|---|---|---|
| 001 | -42.56% to -20.61% | -42.56% to -20.61% |
| 002 | -41.07% to -19.24% | -41.07% to -20.34% |
| 003 | -41.38% to -21.64% | -41.38% to -21.64% |
| 004 | -38.09% to -14.28% | -38.09% to -14.28% |

Both banks lower modeled eligible work in all 48 cases and preserve exact final
sampled depth. Fourteen orders lower counts further in 24 cases and match six
orders in the other 24, with 2.33 times the bank storage. These are
identity-instance projections. Transform tests verify
that a world view direction maps to the local sorting axis via the transpose of
the instance linear transform, including rotations, reflections and nonuniform
scale; an inverse direction transform would give the wrong centroid sort for
nonuniform scale. This does not test a forest of transformed instances or their
inter-tree occlusion, nor does equal depth establish equal shaded color.

The current persistent forest path binds one shared vertex range for each
LOD/source range and issues nonindexed instanced/indirect draws in
`SceneRendering.cpp` (`submitAutomaticGroup`'s persistent-placement branch).
Its tile sort preserves metadata identity; it is not leaf-triangle depth sorting.
One permutation therefore cannot serve all independently placed/rotated trees
and all camera/shadow views optimally. A direction-bank implementation would
need per-view instance classification and draw groups, with independent shadow
directions; up to six nonempty groups could replace one group per existing
material/LOD/range/view. Actual submission counts and CPU costs are unmeasured.

| Four-variety leaf payload | Native expanded vertices | Six orders | Fourteen orders |
|---|---:|---:|---:|
| Expanded vertex storage | 25.23 MiB | 151.40 MiB | 353.27 MiB |
| Hypothetical uint32 index banks | shared native vertices | 2.29 MiB | 5.35 MiB |

Expanded-bank figures are the total payload of the ordered copies; retaining
an additional native copy would add 25.23 MiB. Index figures count only the
permutation indices, exclude allocator/instance/culling metadata and assume an
indexed path that the current persistent forest branch does not implement.
They are design estimates, not measured resident memory. Avoid expanded copies
as the starting implementation. Six indexed orders are a promising runtime
experiment, requiring culling/indirect indexed-draw integration, memory and draw
measurements, camera/light bank transitions, active-wind and shaded-image checks,
and paired cross-backend timing before any adoption. No savings are established
on hardware by these software counts.

Evidence: `.local/foliage-overdraw/directional-order-bank-screen.json`, with
`complete=true`, `inputs_unchanged=true`, `approved_for_installation=false`,
hashed scripts/assets/renderer source, all case counts and bank storage estimates.
All input hashes were independently verified after completion. Eight added tests
cover bank vectors/opposites, direction selection, stable complete permutations,
rotation, nonuniform scale, reflection and invalid transforms/directions.
All 105 selected Python checks pass. Runtime, assets and authored defaults remain
unchanged; the indexed runtime experiment remains open.
