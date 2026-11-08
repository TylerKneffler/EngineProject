# Screen-size mesh LOD

Mesh and TerrainObjectLayer now expose serialized **Screen size LOD**, pixel
thresholds, fractional hysteresis, and **Override with authored distances**.
The default remains distance selection. The forest still uses authored 28/90 m
visible transitions and its independent fixed LOD1 shadow proxy.

The shared selector in `Core/Rendering/MeshLodSelection.h` projects all eight
bounds corners through the full world/view/projection matrix. FOV, zoom,
orthographic size, aspect, scale and actual render-target dimensions all affect
the result. The larger projected pixel axis selects a band. Bounds are not
clipped at screen edges; eye/near-plane crossings conservatively retain LOD0.
The helper accepts either clip-depth convention; the renderer uses zero-to-one.

The renderer supports the existing three resident static geometry bands,
including ordinary runtime meshes, resident instance sets, matching three-band
GrassField tiles and material-changing prefab bands. Prefabs keep a shared bound enclosing all parts and bands,
expanded for maximum foliage wind. Runtime instances use the union of their
source geometry bounds and the existing conservative grass/wind envelope.
Selection therefore does not shrink when the current geometry or wind phase
changes. The standalone runtime API also supports longer resident chains.
Streaming/deformed integrations must provide an authored envelope covering
unloaded geometry or animation; no bounds are inferred from an instantaneous pose.

Hysteresis defaults to 10 percent: coarsen below 90 percent of the lower pixel
boundary and refine above 110 percent of the upper boundary. Large camera cuts
can skip bands. History belongs to the scene, camera/view and object/placement
set, expires after 240 unused graphics frames, and clears with the scene.
Instance sets have a distinct transient LOD identity even without a saved
storage ID. Every part and band of a prefab shares one placement decision.

CPU selection emits contiguous runs into the existing persistent placement
pool; GPU frustum culling and indirect submission continue to operate on those
candidates. Disabling compute consumes the same decisions. Placement transforms
are not reuploaded just because the camera moves. The opt-in CPU projection,
history lookup and run metadata have a cost; this is not a claimed forest
performance win. Incompatible custom/deformed/portal rendering and editor mesh
inspection retain the existing authored policy. Screen selection does not
introduce additional temporal blending; existing appearance/range fades remain.

Distance override restores the existing authored visible selection. Shadow
passes never consume visible screen history, projected thresholds or selected
runs. They retain their existing distance/fixed-proxy policy, including the
forest's fixed LOD1. The color shader skips distance-based geometry clipping
only for screen-selected color draws; shadow coverage and alpha testing remain
independent.

Validation commands:

```powershell
cmake --build build --config Release --target MeshLodSelectionTests ScreenLodBackendTests OakBackendCapture
ctest --test-dir build -C Release -R '^(MeshLodSelection|ScreenMeshLod_.*|ScreenForest_.*|TerrainSharedPrefabs)$' --output-on-failure
$env:ENGINE_OAK_SCREEN_LOD='1'
$env:ENGINE_OAK_CAPTURE_SCALE='0.5' # also 1 and 2
build/Release/TerrainPrefabAndOakTests.exe dx12
python Scripts/compare_screen_mesh_lod.py .local/screen-lod-validation
```

Numerical tests cover ground/elevated views, zoom, 0.5/0.75/1/1.5 render scales,
wind expansion, both depth conventions, orthographic projection, clipping,
camera jitter, hysteresis, invalid thresholds and camera cuts. Device tests
check the actual rendered color of distinct LOD geometries with CPU/GPU culling
and automatic batching on/off, including explicit distance override and distinct
unsaved placement sets. Forest diagnostics compare ground/elevated cameras,
60/30-degree FOV, a moving sequence, CPU/GPU paths and ground-only shadow images.
Capture artifacts are published only after rendering, avoiding the shader watcher.
Unchanged-repeat captures quantify render-order noise separately from policy
differences. Capture logs report selected candidate-placement counts before GPU frustum culling; screenshots and
matched comparisons are under `.local/screen-lod-validation`.

The authored forest transitions are retained. A useful projection policy is not
evidence that the illustrative 300/80-pixel thresholds preserve canopy quality
or improve performance. Replacing 28/90 m still requires choosing asset-specific
thresholds and passing silhouette, coverage, motion and performance acceptance.

## Validation result (2026-11-07)

Release Engine and ProjectScripts builds succeed. The numerical suite, rendered
mesh tests on DX11/DX12/Vulkan, shared-prefab regression and actual forest
comparisons pass. Separate-camera history and distinct unsaved instance sets
are covered. Forest captures include ground/elevated views at 60/30-degree FOV,
a 12-position moving sequence, CPU/GPU paths, and DX12 render-target scales
0.5/1/2 (480x270, 960x540, 1920x1080). Every matched view selects monotonically
more detail as resolution increases.

For the elevated 60-degree view, candidate placement counts before GPU frustum
culling are:

| Render scale | LOD0 | LOD1 | LOD2 |
| --- | ---: | ---: | ---: |
| 0.5 | 1010 | 288 | 750 |
| 1 | 1138 | 431 | 479 |
| 2 | 1319 | 400 | 329 |

Ground-shadow comparisons have zero changed pixels above three RGB levels in
all captured backend/view/scale combinations. CPU/GPU choices agree. Unchanged
repeat images expose occasional backend/order noise, so exact whole-scene byte
equality is not claimed. The override comparison uses that repeat control.

Reviewed side-by-side captures show a clear change to elevated canopy appearance
with the illustrative 300/80-pixel policy. This is not an accepted forest quality
replacement: keep the authored 28/90 m transitions and fixed shadow LOD1.
Evidence: `.local/screen-lod-final-tests.log`, `.local/screen-lod-view-tests.log`,
`.local/screen-lod-script-final-tests.log`, `.local/screen-lod-capture-final-tests.log`, `.local/screen-lod-scale-0.5.log`,
`.local/screen-lod-scale-2.log` and
`.local/screen-lod-validation/comparison.json`. The comparison script generates
PNG contact sheets in distance/override/screen/CPU order alongside the raw PPMs.
