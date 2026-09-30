# Engine roadmap

Last audited against the codebase: 2026-09-29.

Status convention:

- `[x]` Implemented and present in the repository.
- `[ ]` Not implemented or not complete enough to meet the stated acceptance criteria.
- `[ ] PARTIAL` Useful infrastructure exists, but the complete feature or validation target is still missing.
- `DEFERRED` Intentionally blocked on a prerequisite or product decision.

## Next priorities

### Image-based lighting, reflections, and indirect lighting

- [ ] HIGH: Convert equirectangular HDRIs into cached cubemaps with diffuse irradiance and prefiltered specular products.

### HDR, exposure, atmosphere, and materials

- [x] Linear FP16 scene color and a configurable composition pass replace UNORM scene lighting and in-material output transforms across DX11, DX12, and Vulkan.

### Offline video export

#### Deterministic simulation

- [x] Route every existing export-relevant update through the explicit scene/export clock and remove wall-clock animation and script timing.
- [X] HIGH: Give the video exporter an off-screen render target and asynchronous readback instead of capturing the DX11 game swap chain.
- [ ] HIGH: Render an explicit frame at `t = 0` before advancing simulation.
- [ ] HIGH: Add repeatability tests that compare multiple exports of the same scene.

#### Physics quality

- [ ] CRITICAL: Separate output frame rate from physics simulation rate.
- [ ] HIGH: Add an export physics rate, accumulator substeps, maximum substeps, and rigid-body solver iterations.
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
