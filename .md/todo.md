# Engine roadmap

Last audited against the codebase: 2026-09-29.

Status convention:

- `[x]` Implemented and present in the repository.
- `[ ]` Not implemented or not complete enough to meet the stated acceptance criteria.
- `[ ] PARTIAL` Useful infrastructure exists, but the complete feature or validation target is still missing.
- `DEFERRED` Intentionally blocked on a prerequisite or product decision.

## Next priorities

## Implemented foundation

### Runtime, projects, and editor

- [x] C++17 engine library, standalone game runtime, editor application, project files, project loading, project hub, standalone builds, portable exports, versioned Windows packages, and quick-start documentation.
- [x] Scene objects, transforms, reusable components, lifecycle dispatch, scene serialization/deserialization, synchronous and asynchronous scene loading, scene switching, and play-mode scene restoration.
- [x] Game, Scene, Hierarchy, Properties, Assets, Console, Preferences, Terminal, and Problems views with dynamically managed panels, persistent ImGui layouts, and a package-neutral editor UI facade.
- [x] Editor camera controls, object picking, hierarchy creation/deletion/reparenting, drag-and-drop, prefab workflows, component workflows, and configurable undo/redo history.
- [x] Hierarchy Ctrl-toggle and Shift-range multi-selection. The Properties view shows common components, displays mixed values as `-`, and broadcasts edits to every selected object.
- [x] Asset records with source paths, stable IDs, import settings, browser assignment workflows, and throttled/filesystem-notified script refresh instead of recursive scanning every frame.
- [x] DX11, DX12, and Vulkan renderer selection for editor and game hosts, including startup probing and DX11 editor fallback.
- [x] One deterministic scene/export clock now drives gameplay, skeletal and sprite animation, physics, cloth, camera tracks, moving portals, controller movement, and audio transport. Export-relevant

### Assets, animation, UI, audio, and physics

- [x] glTF import through the engine model representation, including skinning, animation, morph targets, vertex colors, UV1, and FBX conversion into the same representation.
- [x] Materials and PBR inputs, alpha modes, normal/height/occlusion/emissive textures, HDR/EXR/TGA/KTX2 processing, mip generation, and corrected advertised texture support.
- [x] Semantic texture color spaces: base-color and emissive textures use sRGB; metallic/roughness, normal, height, occlusion, light/environment data remain linear on DX11, DX12, and Vulkan.
- [x] GPU morph blending with persistent delta/weight buffers and conservative bounds on all three renderers.
- [x] Cached animation-node/joint bindings, reusable pose/sample/palette storage, revision-aware skeleton bindings, and dynamically growing ordinary/skinned/bone buffers instead of fixed 512/64 limits.
- [x] 2D scene editing, sprites, sprite sheets, sprite animation, retained UI layout, UIImage modes, and runtime TTF/OTF text.
- [x] WAV/OGG/MP3 decoding, audio components, mixing, and spatial audio integration.
- [x] Rigid bodies, primitive and mesh colliders, cloth/soft-body simulation, cached cloth meshes, component revision-based rebuilds, and physics/editor transform synchronization.

### Rendering and performance

- [x] Dirty-driven editor rendering, scene edit revisions, cached transform/world matrices, shared per-frame Scene/Game render discovery, precomputed render keys, and cached component-picker entries.
- [x] Persistent and range-bounded light/object/bone uploads, DX11 persistent constant-buffer arena support, deferred-safe DX12/Vulkan growth, and used-range object uploads separated from portal capacity.
- [x] Packed indexed terrain vertices with separate CPU, upload-shadow, and GPU memory reporting.
- [x] Non-blocking frustum/occlusion behavior on DX11, DX12, and Vulkan, including deferred query recycling and conservative fallback when results are unavailable.
- [x] Non-blocking GPU timestamp telemetry for terrain, portal, opaque, and transparent stages on DX11, DX12, and Vulkan, with CPU submission/presentation reported separately.
- [x] Central cache storage used by textures and reusable simulation resources; Vulkan texture-system shutdown drains GPU work and invalidates wrappers before device destruction.
- [x] Scene lighting renders to linear FP16 targets on DX11, DX12, and Vulkan. A dedicated fullscreen composition pass applies exposure, tone mapping, and final sRGB encoding for game output and editor views.

### Portals and spatial mapping

- [x] Symmetric portal connection/disable handling, canonical validated aperture frames, source-to-target relative transforms, bidirectional crossing, parity-aware pose mapping, and rigid-body velocity/orientation remapping.
- [x] Post-physics swept traversal with aperture contact tests, anti-jitter traversal state, deterministic overlapping-volume ownership, and matrix-overlay ownership/priority rules.
- [x] Multiple stencil-isolated portal views, recursive views with cycle limits, scissoring, target-plane clipping, mapped cameras, and connection validation.
- [x] Persistent local/remote split render instances, separate collision pieces, welded cap generation, restoration, and bounded rebuild behavior.
- [x] Unified rendering/physics/raycast/audio/camera/gameplay spatial-query contract and finite recursive portal-ray segmentation used by editor picking.
- [x] HIGH: Gate portal teleportation with a swept collision-shape-versus-inset-aperture test. Use the solid rim as physical feedback instead of testing only the rigid-body centre.
- [x] HIGH: Define explicit portal content-region ownership through a scene layer, scope root, or spatial chart, then restrict rendering and queries to the connected region.

### Directional shadows and realtime lighting

- [x] Directional light replacement for the legacy Ambient type with backward-compatible serialization.
- [x] Serialized per-light shadow controls and project-wide shadow/cascade/PCF/distance/portal policies exposed in the editor.
- [x] Reusable one-to-four-cascade directional shadow atlas with practical splits, transition blending, texel snapping, tile-safe PCF, receiver-normal bias, and far-distance fade.
- [x] Matching DX11, DX12, and Vulkan depth targets, transitions, descriptors, depth passes, resource reuse, and deferred-safe shadow draw data.
- [x] Shadow casters support static meshes, alpha masks, double-sided materials, GPU skinning, GPU morphs, terrain, matrix layers, spatial mappings, and portal-view atlas policies.
- [x] Deterministic directional-shadow selection and GPU data for shadow indices, cascade matrices/splits, bias, filter scale, and shadow strength.
- [x] CPU-built Forward+ clustered light lists for main views with configurable tiles, logarithmic depth slices, caps, matching shader consumption, and safe global-list portal fallback.
- [x] GGX importance-filtered HDR environment mip generation and a generated split-sum BRDF integration LUT across DX11, DX12, and Vulkan.
- [x] Serialized None/ACES/Reinhard output operators and exposure applied consistently in the current material/sky shader path.
- [x] Fixed-timestep DX11/DX12/Vulkan GPU image tests for directional shadows, alpha masks, skinning, morphs, terrain, moving casters, and bounded backend parity. Tests wait for terrain generation and skip when a required backend is unavailable.
- [x] HIGH: Add deterministic light importance sorting for global/portal fallback lists so irrelevant first-in-scene lights cannot displace visible lights.

### Video export baseline

- [x] Direct-to-FFmpeg DX11 export for MP4, WebM, and MOV with fixed output rate, fixed scene update delta, configurable dimensions/duration/codec/quality, camera-track completion, and synchronous RGBA capture.
- [x] The exporter configures the scene clock before `Start()` and advances integer-indexed fixed frames; animation, sprite animation, first-person control, moving portals, physics, cloth, camera tracks, gameplay, and audio consume that clock.

## Active backlog

### Animation and general render performance

- [ ] MEDIUM: Replace linear animation-layer node-mask searches with indexed bitsets and share compatible channel samples between layers.
- [ ] MEDIUM: Remove duplicate per-frame light collection/upload and retain reusable scene scratch arrays, opaque sort storage, component lookup results, and no-portal fast paths.
- [ ] HIGH: Complete mixed-LOD terrain border audits, then batch compatible terrain draws using instancing, indirect submission, or combined ranges.

### Portal correctness and tooling

- [x] CRITICAL: Replace the policy-only portal parity test with actual DX11, DX12, and Vulkan GPU-frame validation covering aperture depth/stencil isolation, recursive views, occlusion, and distinct target-side content without a skybox fallback.
- [ ] HIGH: Expose a Bullet-backed portal raycast-hit API and test regular objects, split pieces, aperture rims, and recursive portal hits.
- [ ] MEDIUM: Add bounded adaptive nonlinear-warp ray paths for gameplay/physics; a single origin/tangent Jacobian cannot represent curved paths or volume-boundary crossings.
- [ ] MEDIUM: Support indexed meshes, submeshes/material slots, skinned meshes, and morph changes in CPU portal cuts. The current triangle-stream clipper cannot preserve all imported topology.
- [ ] MEDIUM: Define ownership of scripts, joints, children, animation, and serialization when a connection disappears while an object remains split.

### Shadows

- [ ] MEDIUM: Define realtime/baked mixed-lighting behavior so static receivers do not double-shadow while dynamic objects retain realtime shadows.
- [ ] HIGH: Add shadow debug views for atlases, cascades, bounds, selected lights, caster counts, and occupancy.
- [ ] MEDIUM: Add shadow telemetry for CPU submission, GPU time, draw/triangle counts, allocation size, and static-cache reuse.
- [ ] HIGH: Add visual regressions for shadow bias, cascade seams, shimmer, far fade, thin/reversed geometry, large coordinates, portals, and spatially mapped casters/lights.
- [ ] HIGH: Stress maximum realtime lights and shadow budgets; verify deterministic degradation without stalls or per-frame resource churn.
- [ ] MEDIUM: Document supported shadow types, quality controls, costs, and known limitations in the README.

### Light model and scalability

- [ ] HIGH: Add spot lights with range attenuation, inner/outer cone angles, serialization, editor gizmos, PBR evaluation, and shadow policy.
- [ ] MEDIUM: Add practical rectangular or disk area lights using LTC or another documented approximation.
- [ ] HIGH: Add camera-aware deterministic importance sorting for bounded fallback light lists.
- [ ] HIGH: Move Forward+ list construction to GPU compute, report overflow, and allocate deferred-safe per-portal-view cluster buffers.
- [ ] HIGH: Define physical light units and inverse-square attenuation with a smooth range cutoff and compatibility mode.
- [ ] MEDIUM: Add per-object light layers/channel masks.
- [ ] MEDIUM: Add directional/spot cookies and optional IES profiles.
- [ ] MEDIUM: Add Kelvin temperature input with RGB override and consistent intensity/exposure behavior.

### Image-based lighting, reflections, and indirect lighting

- [ ] HIGH: Convert equirectangular HDRIs into cached cubemaps with diffuse irradiance and prefiltered specular products.
- [ ] HIGH: Add local box/sphere reflection probes with priorities, influence volumes, box projection, blending, baking, runtime selection, and debug views.
- [ ] HIGH: Add scene reflection-probe capture with recursion safeguards.
- [ ] MEDIUM: Add SSR with hierarchical depth tracing, roughness-aware resolve, temporal stabilization, edge fading, and probe/HDRI fallback.
- [ ] HIGH: Add SSAO/GTAO with normal/depth-aware denoising and temporal stability.
- [ ] HIGH: Add baked irradiance probes/light-probe volumes for dynamic objects.
- [ ] MEDIUM: Add probe interpolation, visibility/leak prevention, relocation/classification, debug visualization, and deterministic out-of-volume fallback.
- [ ] MEDIUM: Evaluate DDGI only after static probe infrastructure exists and has explicit budgets/fallbacks.
- [ ] MEDIUM: Allow emissive materials to contribute to baked/probe indirect lighting; ordinary realtime emissive remains non-illuminating.
- [ ] MEDIUM: Add bent-normal/visibility data for environment diffuse and specular occlusion.

### HDR, exposure, atmosphere, and materials

- [x] Linear FP16 scene color and a configurable composition pass replace UNORM scene lighting and in-material output transforms across DX11, DX12, and Vulkan.
- [ ] HIGH: Add manual/automatic exposure with luminance metering, adaptation rates, limits, and deterministic editor/export overrides.
- [ ] MEDIUM: Add bloom sourced from unclipped HDR luminance.
- [ ] MEDIUM: Add SDR/HDR display color management: paper white, peak luminance, gamut mapping, and swap-chain capability checks.
- [ ] MEDIUM: Add linear-space height/distance fog consistently to main, portal, reflection, and transparent rendering.
- [ ] MEDIUM: Add bounded froxel volumetric fog with light/shadow injection and temporal reprojection.
- [ ] LOW: Add atmospheric sky/sun scattering with authored HDRI fallback.
- [ ] HIGH: Add energy-compensated multiple scattering for rough metallic/specular BRDFs.
- [ ] MEDIUM: Add clear-coat, sheen, anisotropy, transmission, and IOR lobes incrementally.
- [ ] HIGH: Define a lit transparent-material path with lights, reflections, fog, and shadows beyond simple back-to-front blending.
- [ ] MEDIUM: Add scene-color/depth refraction with thickness/absorption and safe portal fallbacks.
- [ ] MEDIUM: Add decals for localized material and lighting detail.

### Lighting quality, diagnostics, and validation

- [ ] HIGH: Add per-feature lighting presets with explicit costs instead of coupling unrelated work only through distance bands.
- [ ] HIGH: Add debug modes for diffuse/specular, normals, roughness, metallic, direct/indirect light, emissive, AO, probes, clusters, luminance, and overdraw.
- [ ] MEDIUM: Add GPU telemetry for cluster construction, occupancy/overflow, reflections, AO/GI, and HDR post-processing.
- [ ] HIGH: Add reference scenes and cross-backend comparisons for every light type, BRDF extremes, HDRI/IBL, probes, AO, transparency, exposure, and portal/spatial rendering.
- [ ] PARTIAL: General parity tolerances exist for the current shadow/animation/terrain fixtures; define per-feature tolerances and add NaN, binding-hazard, black-frame, stale-data, and fallback assertions.
- [ ] MEDIUM: Document lighting architecture, coordinate/unit conventions, feature compatibility, performance tiers, and fallback behavior.

### Offline video export

#### Deterministic simulation

- [x] Route every existing export-relevant update through the explicit scene/export clock and remove wall-clock animation and script timing.
- [X] HIGH: Give the video exporter an off-screen render target and asynchronous readback instead of capturing the DX11 game swap chain.
- [ ] HIGH: Render an explicit frame at `t = 0` before advancing simulation.
- [ ] HIGH: Add repeatability tests that compare multiple exports of the same scene.

#### Physics quality

- [ ] CRITICAL: Separate output frame rate from physics simulation rate.
- [ ] HIGH: Add an export physics rate, accumulator substeps, maximum substeps, and rigid-body solver iterations.
- [ ] MEDIUM: Expose CCD/contact quality and cloth/deformable iteration controls.
- [ ] HIGH: Verify equivalent physics outcomes across output frame rates.

#### Rendering quality and readback

- [ ] HIGH: Separate encoder quality from render quality in CLI/editor settings.
- [ ] HIGH: Add an off-screen export render target and asynchronous GPU readback.
- [ ] HIGH: Add spatial supersampling/MSAA, temporal accumulation, samples per frame, subframe sampling, and shutter controls.
- [ ] MEDIUM: Add export-specific shadow, lighting, reflection, and post-process quality.
- [ ] HIGH: Add FP16/FP32 render targets and genuine HDR/high-bit-depth output.

#### Caching, output, and recovery

- [ ] HIGH: Add a simulation bake/cache for rigid bodies, cloth, particles, and animation with compatibility metadata and reuse across camera/lighting changes.
- [ ] HIGH: Support PNG and EXR image sequences.
- [ ] HIGH: Write an export manifest with frame range/settings/completion state and resume missing or selected frames.
- [ ] MEDIUM: Encode completed sequences as a separate final step while preserving direct-to-FFmpeg as the fast path.

#### Audio and validation

- [ ] HIGH: Render audio against the export clock, write offline audio, maintain synchronization, and mux it into MP4/WebM/MOV instead of using `-an`.
- [ ] HIGH: Test exact frame counts/timestamps for integer and fractional frame rates, camera-track completion, and scene transitions.
- [ ] MEDIUM: Test long-render interruption, resume, and failed-frame recovery.
- [ ] HIGH: Test SDR/HDR color conversion and bit depth for H.264, VP9, ProRes, PNG, and EXR.
- [ ] HIGH: Test long-duration audio/video synchronization.

## Deferred feature gates

- [ ] DEFERRED: Point-light shadows. First add texture arrays/cubemaps and comparison samplers, accept the six-face cost, define radial depth, culling, and a strict per-frame budget, then add point-shadow regression coverage.
- [ ] DEFERRED: Dynamic diffuse GI. Reconsider only after static irradiance probes, visibility/leak handling, diagnostics, and explicit GPU budgets are in place.
