# Independent material cost study

Run `Scripts/run_forest_performance.py` with a fresh directory, `--backend dx12
--width 960 --counters`, then `Scripts/compare_material_costs.py <directory>`.
The existing static audit now includes three tree-only cases at the same camera,
viewport, LOD distances, geometry, shadows and frozen simulation time:

| Case | Color mip bias | Simplify distant shading |
|---|---:|---|
| material-reference | 0 | false |
| material-mips | 4 | false |
| material-cheap | 0 | true |

All use the existing 20–80 metre smooth transition, alpha mip bias zero and
`disableAlphaAtFar=false`. They retain authored alpha cutoff, wind, light sources,
shadow settings and shader paths. These are transient diagnostic overrides.
The cheap case fades normal influence, roughness/metallic maps, occlusion,
reflection and direct specular through the existing material-detail control.
Map fetches disappear only at the fully distant endpoint; the transition still
executes them. Diffuse lighting remains active. This is distance based; the
diagnostic does not implement projected-size selection.

`audit_oak_lods.py` inventories source material slots separately from unique
material assets and triangles. Ordinary native oak LODs have two source slots
(bark and leaves), each with four object-map fetch sites: base, normal,
metallic/roughness and occlusion. At the fully simplified endpoint each has one
base fetch. Impostors have one slot and one base fetch. Sharing the MR/AO image
does not merge their shader reads. These source-level counts exclude shadow,
environment, cookie, IES and parallax fetches, discarded fragments, filtering
taps and compiler/hardware behavior; they are not measured memory transactions.
Alpha-tested mip-only cases can add a separate color fetch after the unchanged
alpha fetch. A lower mip alone never removes normal/MR/AO work or a draw.

The recorder reports active instanced material slots, material component count,
shader path count and actual physical draws separately. Source slots are not
physical draws: spatial batching, geometry LOD bands, visibility and shadow
cascades affect submissions. Shader paths identify source programs, not compiled
PSOs. The three controls use the same Object shader with runtime material flags;
they do not introduce a compiled shader variant. Color and shadow entry points
remain separate passes.

The comparison requires unchanged source slots, paths, physical draw count and
candidate triangles, plus equal executed VS/IA workload and at least 20 valid
resolved GPU samples per case. It reports measured bark and leaf pass times,
without interpreting texture mips as reduced shader complexity. Run repeated
processes in alternating order before attributing a small timing difference.
Inspect captured images across the distance transition before adopting a cheaper
policy; numerical continuity of lighting controls does not establish perceptual
equivalence or a wind animation test.

## DX12 measurement, 2026-11-07

Release, entrance camera, 960×540, shadows off, 32 warmup and 36 measured frames
per case. Corrected capture: `.local/material-cost-dx12-v2/`; input/executable
identity and all 36 fresh GPU/workload samples per case validate. The earlier
`.local/material-cost-dx12/` capture did not enable the material override and is
invalid for attributing material cost; exclude it.

| Case | Bark GPU ms | Leaves GPU ms | Physical draws | Active instanced slots | Shader paths |
|---|---:|---:|---:|---:|---:|
| Reference | 1.375 | 3.368 | 85 | 340 | 1 |
| Mip bias only | 1.393 | 3.318 | 85 | 340 | 1 |
| Cheaper distant shading | 1.351 | 3.277 | 85 | 340 | 1 |

The comparisons verify equal executed VS invocations and IA primitives. Leaf PS
invocations average 8.833 / 8.837 / 8.835 million respectively; invocation counts
can vary with fragment scheduling and depth rejection despite identical alpha
inputs. They do not measure texture reads. The mip-only case changes texture
sampling footprint and can perform an extra base-color read to preserve alpha;
it reduces neither material slots, shader complexity nor draw count.

The source program is unchanged in all cases: **zero added compiled shader
variants**, one shared Object color program with dynamic material branches.
Per-source LOD slot/read inventories are in `.local/material-cost-assets.json`.
The 340 generated spatial slots submit only 85 physical draws in this view.
Do not equate their counts.

Reference and cheap post-measurement images were inspected at the capture
resolution: no obvious canopy holes or abrupt lighting discontinuity appeared.
Wind parameters and vertex code are identical; simulation is frozen for controlled
timing, so this is not a moving-wind visual acceptance run. The smooth 20–80 m
material blend preserves the existing continuous lighting transition and alpha
policy, with distant diffuse illumination retained.

These are single-process sequential diagnostics with a small candidate delta,
not a demonstrated production speedup. Keep authored defaults. Repeated,
alternating captures and a moving route at production resolution with shadows
are required before adopting a more aggressive material policy. The study
establishes independent measurement and a bounded candidate evaluation.

Validation: Release `ForestPerformanceAudit` builds; 11 existing forest
comparison regressions and two material-comparison tests pass. The new tests
reject changed draws, material slots, shader paths and executed geometry.

## Completion protocol

`python Scripts/run_material_study.py .local/material-study-new` reproduces the
completed study. It freezes the Release executables and their DLLs, then runs
three independent processes on each of DX11, DX12 and Vulkan at 1920x1080 with
shadows enabled. Case order alternates reference/mips/cheap and cheap/mips/reference.
Each case has 32 warmup and 36 measured frames. Every material comparison requires
matching executed color and shadow geometry, material slots, material feature
states, shader paths, physical draws and complete input/build identity. Fresh
sample IDs are required independently for color and shadow queries.

The material-only diagnostic invalidates the shadow cache before every shadow
pass to measure the same complete cascade workload. It measures a deliberately
expensive full shadow refresh, not production cached-shadow FPS. DX12 submits
shadow and color as separate renderer frames and records both resolved samples.
Other backends retain their existing same-frame diagnostic path. Do not infer a
cross-backend draw-count improvement from different submission paths.

The first completion attempt in `.local/material-study-complete/` is excluded:
its shadow-geometry gate correctly rejected rotating partial cascade refreshes.
The corrected capture uses the full-refresh control.

Three material feature states are distinguished separately from shader source
paths: opaque bark, masked detailed leaves, and masked base-only impostors.
They share Object `VSMain`, `PSMain` and `PSShadowMain` shader entry points; the
material policy adds no compile-time permutation. Pipeline raster/depth states
and runtime material branches are not counted as distinct shader programs.

| Source material | Color map reads, reference | Fully cheap far endpoint | Mip-only surviving pixel | Shadow alpha reads |
|---|---:|---:|---:|---:|
| Bark | 4 | 1 | 4 | 0 |
| Detailed leaf | 4 | 1 | 5 | 1 |
| Impostor | 1 | 1 | 2 | 1 |

These are source-level map sample calls for the authored oak paths, excluding
lighting/environment reads and filter taps. At equal alpha/color bias, masked
materials reuse the base sample. With different biases, surviving pixels fetch
color separately to keep alpha coverage unchanged. Transition pixels still fetch
detail maps; only the fully distant endpoint skips them. Runtime GPU memory
transactions are not inferred from this table. The complete source inventory is
`.local/material-cost-assets-complete.json`.

Visual validation uses lit, tangent-equipped oak alpha cards with authored normal,
metallic/roughness and occlusion textures, independent material overrides and zero
mip bias. The isolated fixture uses a 3-8 m transition at 512x512, with its moving
camera covering 1.2-10.2 m; the forest timings retain the 20-80 m policy at 1080p.
Static views cover 0/60/80 degrees and distances around both policy
boundaries. The moving path crosses the whole transition twice, followed by
fixed-camera wind. Paired frames share simulation time exactly. The test requires
identical alpha masks and wind turnover, unchanged near-endpoint RGB, an exercised
far lighting change, and a maximum per-frame change in mean material-only lighting
difference of one 8-bit code value per covered channel. The deliberate camera
teleport into the fixed-camera wind segment is excluded from that continuity gate.
This aggregate image check supplements inspection; it is not a per-pixel perceptual
equivalence claim.

Rendered shadow checks compare reference and cheap masks at all three angles and
near/mid/far distances. Opaque-card controls must reveal cutout holes, and advancing
wind must leave the existing static-caster shadow policy unchanged. These are
isolated rendered checks of the shared material shader, not a claim that lower
quality produces identical color everywhere.

## Completed repeated timings

Evidence: `.local/material-study-complete-v2/`. All nine processes use the same
frozen executable, CMake cache and asset/shader content hashes. There are 972
fresh resolved color samples and 972 independently resolved shadow samples.
Each color sample executes 1,410,504 bark and 4,673,502 leaf VS invocations;
each forced full-shadow sample executes 20,459,370 VS invocations and 6,819,790
IA primitives. These counts match across all three material cases and all runs.
Each case records 170 physical draws, 340 instanced material slots, three material
feature states and one shader source path on each backend.

Times are median [minimum-maximum] of three independent process means, in ms.

| Backend | Case | Bark | Leaves | Full shadow refresh |
|---|---|---:|---:|---:|
| DX11 | Reference | 1.941 [1.918-2.207] | 7.445 [7.160-7.448] | 6.232 [5.814-6.627] |
| DX11 | Mips only | 1.933 [1.932-2.079] | 7.376 [7.051-7.456] | 6.028 [5.971-6.229] |
| DX11 | Cheap shading | 2.012 [1.928-2.161] | 7.541 [6.839-7.567] | 6.017 [5.815-6.224] |
| DX12 | Reference | 2.003 [1.940-2.122] | 6.993 [6.911-7.215] | 5.861 [5.634-6.470] |
| DX12 | Mips only | 2.034 [2.023-2.419] | 7.066 [6.934-7.236] | 5.602 [5.580-5.770] |
| DX12 | Cheap shading | 2.018 [1.990-2.046] | 6.852 [6.787-6.856] | 5.952 [5.760-6.524] |
| Vulkan | Reference | 1.745 [1.699-1.921] | 6.384 [6.286-6.697] | 6.857 [6.731-6.932] |
| Vulkan | Mips only | 1.909 [1.756-1.912] | 6.562 [6.291-6.700] | 6.754 [6.612-6.948] |
| Vulkan | Cheap shading | 1.867 [1.850-1.944] | 6.428 [6.187-6.592] | 6.821 [6.467-7.291] |

The candidate does not establish a consistent cross-backend pass-time benefit.
The small DX12 leaf difference does not generalize to the other APIs, and the
three-process ranges overlap. Paired per-process deltas are retained in
`timing-summary.json`; differences between medians are not substituted for those
deltas. Shadow shader behavior is unchanged, so fluctuating shadow timings are
not reported as savings from cheaper material shading. Background load, clocks,
query overhead and execution overlap can still vary. Keep authored defaults.
This completes the bounded material evaluation without making a production FPS
or runtime texture-memory claim.

## Completed visual acceptance and validation

All three APIs pass 81 static mask pairs, 540 paired camera/wind frames and 27
rendered shadow mask pairs in total. Near-endpoint RGB is identical; the far
endpoint has a nonzero lighting change on every backend. Wind mask turnover is
identical between reference and candidate, and all 54 shadow wind comparisons
preserve the existing static-caster policy. Maximum mean material-lighting effect
step is 0.594 / 0.455 / 0.455 code values per covered channel for DX11 / DX12 /
Vulkan, below the predefined one-code limit. The maximum mean material-only
lighting difference is 8.306 / 6.263 / 6.263 code values; this is an intentional
quality difference, not an unchanged-color claim.

The 1080p forest reference/candidate images and an isolated transition capture
were inspected: no obvious holes or silhouette changes appeared. Timings and
visuals share the same asset/shader content identity, and every recorded visual
artifact hash verifies. Full results are in `still/summary.json`,
`motion/summary.json`, `visual-provenance.json` and `completion-validation.json`
under `.local/material-study-complete-v2/` (`complete=true`).

Release `ForestPerformanceAudit` and `TextureSamplingBackendTests` build
successfully (`.local/material-completion-build-shadow.log`). Twenty Python
regression tests pass: eleven existing forest comparison tests, four material
timing/comparison tests and five material visual tests. They reject stale samples,
changed geometry/draws/material states, mismatched builds/environment, a no-op
candidate, changed alpha coverage, incomplete motion and abrupt lighting changes.
Authored materials, textures, geometry, wind and production quality defaults are
unchanged by the completed study.
