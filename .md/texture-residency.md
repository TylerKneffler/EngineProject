# Texture residency investigation

Investigated 2026-11-05 against the current texture upload paths.

## Current behavior

`Texture::Load` decodes ordinary images to RGBA8 and generates a full CPU mip chain. KTX2 Basis content is transcoded to RGBA32 rather than uploaded as GPU block compression. `Texture::PrepareImmediate` uploads all ordinary texture mips. Float panoramas already skip existing top mips until the longest dimension is at most 2048, retaining original CPU pixels for lighting projection.

DX11 creates an immutable texture with the supplied dimensions and mip count. DX12 creates a committed resource and staging footprints for the supplied chain. Vulkan creates an image and stages all supplied levels. The shared factory accepts a smaller base image and remaining mip chain, so a static resident resolution limit can use the existing interface across these backends. Shader mip bias and view mip restrictions alone do not shrink these allocations.

`Texture::Acquire` caches by path and color space with a long-term lifetime. Multiple trees share these assets; multiplying texture bytes by tree count would overestimate residency. Separate sRGB and linear uses can create separate textures. Destroying scene instances alone is not a texture eviction policy.

## Oak inventory

Reproduce with:

```powershell
python Scripts/Assets/audit_texture_residency.py Engine/Core/Assets/Models/Woodland/Oaks/Textures
```

| Source textures | Dimensions | Full chain MiB | 2K cap MiB | 1K cap MiB |
|---|---|---:|---:|---:|
| Texture 1, 2, 3 (combined) | 1024 x 2048 each | 32 | 32 | 8 |
| Texture 4, 5, 6 (combined) | 4096 x 4096 each | 256 | 64 | 16 |
| Total | | 288 | 96 | 24 |

These are uncompressed RGBA8 mip payload estimates counting each PNG once, not a GPU memory measurement. They exclude allocation alignment, staging, CPU copies, other scene assets and duplicate color-space bindings. The audit reads PNG headers and uses the engine's halving rule through 1 x 1, including rectangular and non-power-of-two dimensions.

## Recommended next implementation

Add an opt-in static maximum resident dimension at upload time, using the panorama's top-mip skipping approach. Keep original source dimensions and CPU/editor reads intact. Start with a global startup setting so shared textures have one unambiguous policy; default to unlimited for ordinary textures and preserve the existing panorama cap. Avoid adding a distance-dependent resident cap to individual materials while resources remain shared by path and color space.

Recreating textures on every distance transition would incur decode/upload stalls and resource churn. Dynamic streaming additionally needs projected-size demand aggregated across all uses, a memory budget, hysteresis, asynchronous staging, frame-safe resource/descriptor replacement, and explicit cache eviction. DX12 descriptor lifetime and Vulkan in-flight resources need particular review before runtime replacement.

A static cap also reduces the finest alpha-mask resolution, unlike the existing shader bias policy. Compare close foliage silhouettes and masked shadows before choosing oak defaults. Validate each backend's uploaded dimensions, remaining mip count and GPU allocation, CPU terrain/painting reads, provider switching, and reload behavior. Benchmark upload time and steady GPU memory separately from frame time; initial CPU decoding and mip generation remain full-size with this approach.

## Static resident cap

The upload cap is implemented as a process-wide startup environment setting:

```powershell
$env:ENGINE_TEXTURE_MAX_DIMENSION = '2048'
# Launch the editor or game from this shell.
```

Unset or `0` leaves ordinary textures unlimited. Use a positive unsigned integer; malformed, negative and overflowing values are ignored. The setting is captured at the first texture upload and requires a process restart to change. It applies to textures uploaded through `Texture::PrepareImmediate`, including queued publication, across all factories. Render targets and textures created directly by a backend are outside this policy.

Only existing top mips are omitted: a 1024 x 2048 texture capped at 1024 uploads as 512 x 1024. Non-power-of-two limits select the first available mip within the limit. At least one mip remains. Float panoramas retain their 2048 ceiling and may be reduced further by a smaller global cap. Source dimensions, generated CPU mip chains, editor reads and lighting projection stay unchanged. Reloads and provider changes reuse the startup policy.

This reduces GPU texture payload and upload data, not initial CPU decode/mip generation. Alpha masks also use the reduced resident resolution, so the cap remains opt-in. Dynamic streaming, measured backend allocations and foliage image comparisons remain follow-up work.

Validation: `TextureResidencyTests` records the shared factory upload and compares every uploaded byte with the expected source mip suffix. Seven process-isolated CTest cases cover unlimited, 1024, 1, above-source, malformed, negative and overflowing limits, including preserved CPU reads, repeated preparation, provider switching and reload. This verifies factory inputs rather than actual backend allocation sizes or rendered alpha coverage.

## Backend validation results (2026-11-05)

`TextureResidencyBackendTests` uploads all six oak textures through each real renderer and renders the leaf atlas as a double-sided, unlit masked card with alpha cutoff 0.32. The 512 x 512 fixture uses camera distances 3, 8 and 20 and shader LOD off/on. Each cap runs in a separate process. The comparison gate is coverage within 5% of baseline and silhouette intersection-over-union at least 0.90.

All 36 capped silhouettes (two caps, three backends, six views) matched their own backend's baseline pixel-for-pixel: coverage ratio 1.0 and silhouette overlap 1.0. DX11 baseline coverage was 22,797 / 3,571 / 607 pixels at the three distances; DX12 and Vulkan were 22,800 / 3,572 / 607. Shader LOD did not change these silhouettes.

| Backend | Uncapped allocation MiB | 2K allocation MiB | 1K allocation MiB |
|---|---:|---:|---:|
| DX11, Intel UHD | unavailable | unavailable | unavailable |
| DX12, RTX A2000 Laptop | 288.188 | 96.188 | 24.188 |
| Vulkan | 288.012 | 96.012 | 24.012 |

DX12 sizes use `GetResourceAllocationInfo` for the actual committed texture descriptions. Vulkan sizes use the actual images' memory requirements, which equal the dedicated allocation sizes passed to `vkAllocateMemory` by `VulkanCreateImage`. These include resource alignment but exclude staging, descriptors, other scene resources and CPU data. They describe allocated GPU memory, not instantaneous physical residency. DX12 adapter usage deltas also matched the dedicated allocation sums.

DX11 does not expose a per-texture allocation-size query through this harness. Intel UHD's combined local/nonlocal DXGI process usage remained 13,901,824 bytes before and after uploads for every cap. This is inconclusive and must not be interpreted as zero-sized textures or proof of memory savings. Summary JSON represents the unavailable size as `null`; the validation script returns failure while that allocation gate remains unresolved.

Both test targets built successfully, including a Vulkan-enabled Release configuration in `build/Debug`. Reproduce the captures from the repository root:

```powershell
cmake --build build/Debug --config Release --target TextureResidencyBackendTests -j 4
python Scripts/Assets/validate_texture_residency.py build/Debug/Release/TextureResidencyBackendTests.exe
# Re-analyze saved captures without repeating GPU runs:
python Scripts/Assets/validate_texture_residency.py build/Debug/Release/TextureResidencyBackendTests.exe --reuse
```

Raw logs, 54 binary silhouette masks, allocation readings and `summary.json` are written under `.local/texture-residency-validation`. The audit target is guarded by source existence and remains optional.

Oak defaults remain unchanged. This fixture validates ordinary projected leaf-card coverage; it does not establish quality for magnified close-ups, whole-canopy views, masked directional shadows or every adapter/driver combination. Resolve the DX11 allocation measurement and review those quality cases before selecting an oak default.
