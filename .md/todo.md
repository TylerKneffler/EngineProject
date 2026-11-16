# Engine TODO

## Occlusion culling for all objects

### Route

The backends do not expose the main depth buffer to shaders, and compute
passes bind only buffers (three fixed bindings: input, output, placement
pool). A GPU depth pyramid would need that plumbing in DX11, DX12 and
Vulkan first.

So occlusion starts as a **CPU occlusion buffer**
(`Engine/Core/Rendering/OcclusionBuffer.h`), built for the current frame:

- No one-frame lag, and no trouble with objects coming into view in fast
  turns.
- It works on every backend.
- Occluders are the terrain meshes drawn this frame (their vertices stay on
  the CPU), rasterized at 256 × 144. Other meshes are added later (step 4).

### Done (stage A)

- [x] **Occlusion buffer:**
  - Terrain is rasterized on worker threads with near-plane clipping, row
    bands and centre sampling.
  - A 3×3 erosion keeps partly covered silhouette pixels from hiding
    anything.
  - A max-depth mip pyramid answers box and sphere tests in at most 9
    reads.
  - Bounds crossing the near plane or wholly off screen count as visible.
- [x] **Build:**
  - It runs as a job started right after the camera matrix, alongside the
    cascade and lighting preparation. Shadow views do not use it.
  - It is off for editor views, portal views and warp volumes, like the
    existing terrain tests.
  - `ENGINE_VISIBILITY_OCCLUSION_BUFFER=0` turns it off.
- [x] **Ordinary objects and whole instanced tiles:** their world bounds are
      tested in `PrepareVisibleItems`, next to `HiddenByTerrain` /
      `HiddenBySolid`. Skinned, morphing, offset, blended, preview, portal
      and warped items are skipped.
- [x] **Detail budget:** the cost model drops placements hidden behind
      terrain before its disc coverage grid, so they cost nothing.
- [x] **Benchmark fields:** `occlusion_occluders`, `occlusion_triangles`,
      `occlusion_rasterized`, `occlusion_build_ms`, `occlusion_tested`,
      `occlusion_hidden`.
- [x] **Unit tests:** `.local/tests/OcclusionBufferTests.cpp` (CMake target
      `OcclusionBufferTests`, guarded like the other local tests). It covers
      hidden and visible cases, silhouettes, the near plane, off-screen
      bounds and the empty buffer. `.local` is ignored by git, so a narrow
      `.gitignore` exception is needed to commit it.

### Measured (landscape demo start view, 1080p, RTX 4090)

| Backend | Build | Triangles in / rasterized | Items tested / hidden |
|---------|-------|---------------------------|-----------------------|
| DX12    | 0.7–1.3 ms | 97k / 24k | about 976 / 23 |
| DX11    | 1.3 ms     |           | 942 / 23 |
| Vulkan  | 1.2 ms     |           | 976 / 23 |

- With the detail scale pinned at 1, frames with occlusion on and off are
  identical apart from wind in the near grass, so nothing visible is
  dropped.
- The render thread pays about 0.9 ms, waiting on the build.
- In this meadow view, terrain hides only about 2% of tested items, and
  GPU geometry time does not change measurably. The landscape's hills are
  low (at most about 120 m), and most hidden placements sit inside tiles
  that are partly visible, so stage B is where the gain should come from.
- With the budget adapting, hidden placements count as free, so the
  budget settles higher (scale 1.41 against 1.25). The grass layer's
  spawn radius grows with it, and that CPU work is outside the budget.
  The demo is currently CPU-bound at about 21 ms a frame, with GPU geometry
  at about 5.5 ms.

### Done (stage B)

- [x] **Fourth compute read binding:**
  - DX11: `t2`.
  - DX12: a fifth root parameter, SRV `t2`.
  - Vulkan: binding 3, with the compute descriptor pool grown to four
    buffers per set. The extra binding is cleared whenever a compute
    pipeline is set.
- [x] **GPU upload:** the occlusion buffer goes up once per view as a float
      array (`OcclusionBuffer::SerializeForGpu`), using a ring of upload
      buffers per prepared frame slot. An empty buffer (level count 0) is
      bound for shadow passes and when occlusion is off.
- [x] **Instance cull:** `InstanceCull.hlsl` (`IsBoxOccluded`, matching
      `IsBoxHidden`) tests every placement in the per-placement path (trees,
      rocks, grass tiles) and every instance in the automatic path, in
      camera passes only. `ENGINE_VISIBILITY_OCCLUSION_GPU=0` turns this off
      and keeps the CPU tests.
- [x] **Debug overlay:** `ENGINE_OCCLUSION_DEBUG=1` with the Detail render
      mode keeps everything occlusion would hide and paints it magenta, so
      any magenta on screen is something visible that would have been
      culled. `=2` marks everything tested, as a check that the overlay
      works.

#### Results (landscape demo start view, detail scale pinned at 1)

- **Placements hidden:** LOD0 foliage placements selected fall from about
  15,500 to about 900 (94% hidden behind terrain). DX12, DX11 and Vulkan
  agree.
- **No visible errors:** the debug overlay shows no magenta pixels in the
  start and west views on all three backends. The control mode paints the
  whole ground and every tree magenta.
- **GPU time:** tree leaves fall from 0.71–0.86 ms to 0.52–0.54 ms, and
  foliage depth from 1.24–1.27 ms to 1.04–1.19 ms.
- **Not comparable yet:** total frame times. The explorer's start position
  currently varies from run to run, so paired runs do not share a camera.

### Next

#### Build cost

- [ ] Cut the build below 0.5 ms:
  - Skip occluders whose projected bounds are tiny.
  - Cache transformed terrain vertices while the camera has not moved.
  - Start the job earlier (before frame preparation) so the render thread
    no longer waits.
- [ ] Coarser terrain beyond about 300 m: rasterize the far tiles only, or
      a per-cell height grid lowered to each cell's minimum so it stays
      conservative.

#### More occluders

- [ ] Large opaque meshes (rocks, logs, buildings) as occluders, chosen by
      projected size and solidity. Use their lowest LOD, shrunk or with
      erosion, so occluders never stick out past the real surface.
- [ ] Keep foliage out: leaves only partly hide what is behind them.

#### Disocclusion (only if a GPU depth pyramid is added later)

The CPU buffer is built for the current frame, so it has no one-frame lag.
This step only applies if the GPU route (below) is taken.

- [ ] Two-phase culling: draw what was visible, rebuild the pyramid from
      partial depth, then re-test what was rejected.

#### Detail budget

- [ ] Replace the disc coverage grid's foliage estimate with real per-placement
      visibility, now that stage B exists (a GPU histogram of visible cost
      against LOD scale).
- [ ] The explicit-mesh cull path (`selection.y == 1`) is not tested yet:
      its output record has no free field for the debug mark.
- [ ] Make the explorer's start position deterministic for captures, so
      paired on/off runs can be compared frame for frame.
- [ ] Extend the cost model to ordinary LOD'd meshes, not only instanced
      bands.

#### GPU depth pyramid (later, optional)

- [ ] Make the main depth buffer readable, bind textures in compute, build
      the pyramid on the GPU from last frame's depth, and test with the same
      helper. Every opaque surface then hides things. Needs the
      disocclusion step above.

### Open questions

- Terrain vertex shaders: if terrain is ever morphed or displaced on the
  GPU (geomorphing, heavy relief), the CPU vertices no longer match the
  drawn surface. Erosion covers about one occlusion pixel (about 7 screen
  pixels); anything larger needs a margin.
- Water is alpha-blended and is not an occluder. Confirm any future
  occluder source leaves it out.
