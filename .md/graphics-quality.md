# Graphics quality

The [texture sampling audit](texture-sampling-audit.md) records backend sampler
differences, mip-chain behavior, alpha coverage comparisons, and the distinction
between distance bias and resident memory. DX11 uses 8x anisotropy; DX12 and Vulkan
currently use trilinear material sampling. Continuous-motion and isolated
rendered-shadow comparisons pass on all three APIs. The completed audit also
records a reproducible diagonal-light shadow-coverage failure on all three;
it does not grant blanket shadow-coverage parity or approve a cutout replacement.

Scenes serialize `graphicsQuality` with preset IDs Reference=0, High=1, Medium=2, Low=3 and independent geometry, vegetation, viewDistance, texture, material, shadows, and renderScale overrides. Negative overrides inherit policy. Effective multipliers are finite and bounded; they cannot increase authored budgets. Missing settings load Reference, including when reusing a SceneSettings object.

Asset meshes, material overrides, scene texture LOD and authored scene scale remain the source data. Preset policy is applied to copies immediately before rendering, not baked into assets or the static material cache. Reference uses identity multipliers. Geometry scales resident LOD thresholds, vegetation and view distance cap bounds visibility against the camera far plane, texture controls distance mip bias, material controls distant simplification, and shadows reduce caster distance, receiver distance and filter cost. Texture quality changes sampling, not GPU texture residency. Existing asset visibility limits still apply.

The scene Properties panel displays presets, independent override multipliers, and resolved values. Editor frame JSONL includes quality_preset and all seven quality_* multipliers beside graphics_backend and actual viewport metrics. Offscreen Scene/Game editor targets apply the effective render scale through their common View sizing path for all backends. This uses spatial upscaling by the editor UI; it does not implement temporal reconstruction or dynamic scaling.

Validation: build GraphicsQualityTests and GraphicsQualityBackendTests with BUILD_TESTING=ON. The first checks reference compatibility, independent texture/material policy, serialization, legacy reset and nonfinite input. Run the backend executable with dx11, dx12, or vulkan plus an output prefix. It renders all four presets at three distances, asserts nonempty captures, and writes mask images and coverage JSON for comparison. The capture scene checks masked texture rendering; geometry, vegetation, shadow and viewport scaling require additional representative scene captures before claiming full visual parity.

Initial Debug verification: Engine, Editor and GraphicsQualityTests built successfully; policy/serialization tests passed. The masked capture audit completed on DX11, DX12 and Vulkan for all four presets. Coverage is unchanged between presets within each backend. DX12 and Vulkan coverage matched exactly (22800, 3572, 607 pixels at distances 3, 8, 20); DX11 measured 23183, 3525, 604. Thus this audit confirms preserved alpha coverage but does not establish identical raster output across all three APIs. Artifacts are in `.local/quality-captures/` and build logs use `.local/quality-*.log`.
