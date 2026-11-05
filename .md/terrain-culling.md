# Terrain culling regression validation

## Synthetic automated checks

`TerrainCullingBackendTests` is a synthetic regression fixture, not the oak meadow scene. It creates a continuous sloped height field using the real terrain manager, streamed cells, disk caches and far tiles. Its route crosses X and Z cell boundaries and returns to the starting window.

Each backend must pass these checks:

- All nonvertical surface triangles face upward and match the shared analytic height within 0.0001 scene units. Vertical skirts are excluded from the upward-area assertion.
- Full-resolution near cells and coarse far LODs are resident, with no reported coverage gaps. Three stable streaming iterations allow far visibility to update after deferred near-cell uploads.
- Close and wide orthographic captures have no background pixels in the tested interior, covering near-cell seams and coarse far-tile transitions.
- Viewing from below produces no visible terrain pixels, even with double-sided materials enabled.
- Returning to the starting window restores terrain from disk cache.
- Near-only, far-only and combined terrain casters each darken a lit receiver under a directional light. Repeated combined-shadow captures remain stable across reused frames.

On 2026-11-05, all three CTest cases passed: `TerrainCulling_dx11`, `TerrainCulling_dx12` and `TerrainCulling_vulkan`. Each tested streaming window contained 10,212 upward surface triangles, zero image coverage gaps and zero underside pixels. The logs include cache-hit and shadow-pixel counts.

## Vulkan defect exposed by the checks

Partial directional-cascade refreshes call `BeginDepthOnlyPass` with a negative clear depth to retain the atlas. Vulkan selected the clearing render pass and supplied a negative depth, effectively clearing preserved depth to zero. This made the receiver fully shadowed, so adding terrain casters could not darken it further.

Vulkan now selects the existing load render pass for this preservation request. Positive clear requests retain their clearing behavior. The full backend matrix passed after this change; the Vulkan shadow comparison failed before it.

## Reproduction

```powershell
cmake --build build/Debug --config Release --target TerrainCullingBackendTests -j 4
ctest --test-dir build/Debug -C Release -R '^TerrainCulling_' --output-on-failure
```

The referenced build configuration has Vulkan enabled. CMake registers the Vulkan case only when that backend is enabled. Test sources live in `.local/tests`, have narrow ignore exceptions, and targets are guarded by source existence. Captures and logs under `.local/terrain-culling` are synthetic diagnostics.

## Actual oak scene checks

`OakBackendCapture` loads `Engine/Core/Assets/Scenes/terrain/oak_meadow.scene` with its terrain, native tree geometry, authored materials and grass scripts. It captures the authored entrance camera and established overlook camera at 960 x 540. The harness also compares directional shadows on/off and checks the actual terrain's underside against a background capture with other geometry hidden.

The saved oak scene currently disables sun and terrain shadows. Normal entrance/overlook captures preserve those authored settings. Separate diagnostic captures temporarily enable material shadow casting and the directional sun, using a light resolution scale of 1; all original flags are restored in memory afterward. Underside diagnostics isolate the actual oak terrain, force its material double-sided, and compare its pixels against the same sky/background with terrain hidden. Saved assets are not changed.

The capture harness explicitly waits for all component material textures to become GPU-ready and refills the frame buffers afterward. A fixed frame count alone was insufficient on the faster Vulkan path: asynchronous 4K leaf-atlas decoding could still be pending, producing white leaf cards in an early capture.

```powershell
cmake --build build/Debug --config Release --target OakBackendCapture -j 4
ctest --test-dir build/Debug -C Release -R '^OakBackend_' --output-on-failure
```

Actual scene images and logs go to `.local/oak-backend-validation`, separate from the synthetic fixture. Files containing `diagnostic` isolate terrain or background and are not normal oak scene views. The harness changes no saved scene or material settings.

All three actual-scene cases passed on 2026-11-05 after adding the texture-readiness guard: `OakBackend_dx11`, `OakBackend_dx12` and `OakBackend_vulkan`. Both cameras produced visible directional-shadow differences in the diagnostic toggle, and each isolated underside matched its background with zero differing color channels. The Vulkan entrance, overlook and enabled-shadow images were visually reviewed after textures became ready. This completes six passing backend cases across the synthetic and actual-scene checks; it does not imply pixel-identical lighting between renderer backends.
