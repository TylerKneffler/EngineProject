# GPU shadow-distance fading and stability checks - 2026-11-03

Continued the foliage GPU-update work and tracked the reported shadow flicker. Removed a concrete source of shadow popping: tile-wide shadow eligibility toggled as the viewer crossed the layer shadow-distance boundary.

## Implemented

Grass tiles retain stable casting eligibility according to layer settings. Each mesh supplies a transient caster range/fade width; the layer sets these for grass and the scene grass-caster budget remains a cap. Generic rigid meshes can use the same runtime range settings.

The compute culler rejects shadow placements fully outside the range. Boundary-crossing placements remain available for a world-anchored pixel fade. The shadow shader scales complementary geometry/LOD dither intervals together, preserving their union as shadow opacity changes. Color-pass visibility and placement residency are independent. Main and portal shadow records enable the fade. CPU object bounds remain a conservative coarse filter; CPU no longer changes tile casting eligibility based on viewer distance.

Object GPU records gained one matching float4 on CPU/HLSL (1504 bytes); allocation and capacity checks use the updated stride. Fixed editor captures keep resident placements with zero repeated placement uploads.

Found and fixed a separate DX12 standalone-game defect while strengthening validation: EndDepthOnlyPass left the shadow framebuffer bound. The game renderer now supplies a host restoration callback that rebinds the scene color/depth targets and its viewport/scissor without clearing or transitioning the scene color again. This is independent of the reported DX11 flicker.

## Verification

The older comparison fixture did not prove that receiver shadows were visible. Its receiver normals opposed its triangle winding and its bias was inappropriate for its small geometry. Corrected those and added checks for full, partial and absent shadow coverage, plus repeated stationary captures through ordinary and native command recording.

Measured image differences from the no-caster image (sum of RGB channel differences; smaller means less shadow):

| Backend | Full shadow | Partial fade |
| --- | ---: | ---: |
| DX11 | 64205 | 22922 |
| DX12 | 74866 | 24199 |
| Vulkan | 75924 | 24331 |

All backends pass fade endpoints, intermediate coverage, complete removal beyond range, stable repeated frames, GPU/reference instancing, LOD/fade comparisons and sparse high-slot cascade tests. GrassPerformance also passes. Final four-test CTest run: 4/4 passed, 78.36 seconds. The final build additionally enables the same fade flag in portal shadow records; portal-specific visual validation remains open.

## Performance captures

Debug DX11, fixed 1920 x 1080. Editor Play capture uses 60 warmup frames followed by 180 measured frames:

- Average: **147.8 FPS / 6.766 ms**.
- P95: **7.467 ms**; P99: **8.058 ms**.
- Render-item preparation: 0.663 ms.
- Command recording: 3.431 ms.
- Measured GPU stages: 1.942 ms.

All measured editor frames are below 10 ms, with 3,422 grass instances, 45 candidate draws, 14 physical color draws and 24 shadow draws. Recording has no dropped/failed records. The prior stationary capture was 162 FPS / 6.172 ms; this capture is slower (6.766 ms), chiefly showing more command-recording time (3.431 versus 2.819 ms). These are single separate runs, so no isolated speedup or exact cost attribution is claimed. This change improves shadow transitions rather than establishing an FPS gain.

Moving direct-render helper: 240 measured frames, **87.3 FPS / 11.454 ms**, P95 19.227 ms, P99 29.488 ms. It excludes editor UI/full physics and still has spikes. Sustained moving/streaming 100+ FPS remains open.

## Remaining investigation

The distance pop is addressed and the stationary fixtures are stable. This does not prove that every instance of the user-reported flicker is fixed: moving-camera cascade edges, residency changes and the actual affected scene still need confirmation. The flicker TODO remains open. Release and streaming validation and remaining CPU preparation work also remain open. Added the latest interactive recording findings to TODOs: UI construction 4.69 ms and command recording 3.10 ms.

Captures and final tests: `build/Debug/shadow-fade-verification-20261103/` (`editor-1080p.jsonl`, `editor-evaluation.md`, `moving-on.jsonl`, `ctest-final.log`). Builds: `build/Debug/shadow-fade-final-build.log`. Current user layout and asset index were held in memory and restored after the runs.
