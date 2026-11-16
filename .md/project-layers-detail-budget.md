# Project layers and the detail budget

## Layers

Project layers are object categories, like Unity's layers. The list lives in
the project settings (Project Preferences > Layers) and is saved in the
project file under `<Layers>`. Every object belongs to one layer:
`Object::layer` is an index into the list, and the Inspector shows it as a
Layer dropdown, for single objects and multi-selections.

- Index 0 is always `Default`. Objects store indices, so renaming a layer
  keeps its objects. Reordering or removing a layer renumbers the layers
  after it, as in Unity.
- A scene writes `layer` only for objects off layer 0, so existing scene and
  prefab files are unchanged.
- There are at most 32 layers.

Default layers:

| # | Layer      | Distance scale | Budget weight |
|---|------------|----------------|---------------|
| 0 | Default    | 1              | 1             |
| 1 | Terrain    | 1              | 0.5           |
| 2 | Trees      | 1              | 1             |
| 3 | Grass      | 1              | 1.25          |
| 4 | Props      | 1              | 1             |
| 5 | Characters | 1              | 0 (exempt)    |
| 6 | Water      | 1              | 0 (exempt)    |
| 7 | UI         | 1              | 0 (exempt)    |

At runtime the list and the budget live in `Engine::Rendering::DetailBudget`
(`Core/Rendering/DetailBudget.h`). The editor and the game configure it from
the project settings; the editor updates it whenever the settings change.
Code can look layers up by name with `FindLayerOrDefault("Trees")`.

## Detail budget

Instead of fixed LOD distances, every layer's LOD distances are scaled so
that rendering stays within two budgets:

- **GPU geometry time:** the sum of the shadow, terrain, grass, opaque, tree
  bark, tree leaves and foliage-depth GPU stages. The default budget is
  6 ms.
- **CPU render time:** the time spent in `Scene::Render` for every pass in a
  frame (cameras and shadows). The default budget is 4 ms.

The scale comes from a cost model when one is available, and from a
reactive controller otherwise.

### Cost model (predictive)

The game view hands every visible instanced LOD band (trees, rocks, logs) to
a background job about ten times a second (`DetailCostModel.h`). The job
predicts what those instances would cost at every global scale:

- Placements hidden behind terrain cost nothing: they are tested against
  the view's occlusion buffer (`OcclusionBuffer.h`, the terrain drawn this
  frame rasterized at 256 × 144). The same buffer skips hidden objects and
  whole hidden instanced tiles at draw preparation.
- Each remaining placement is projected and processed front to back over a 64 × 36
  screen coverage grid. A placement counts only for the part of it not
  already covered by placements in front of it; foliage covers half of
  what is behind it, solid meshes all of it. Trees inside a forest, behind
  its first rows, therefore cost almost nothing.
- Each LOD band adds its triangles, times that visibility, for the range of
  scales at which the band is the one drawn.

The budget is a cost in these visibility-weighted triangles. The scale is
the largest the budget affords for the current view (up to
`maximumScale`), so detail spreads as soon as the view allows it. For
example, one near tree and a hundred open-field trees far away is cheap,
so the far trees get full detail. A dense view pulls detail in before the
GPU time even moves.

GPU feedback calibrates the budget:

- Terrain and grass time do not change with LOD distances, so the budget
  compares the remaining geometry time with what is left of the GPU budget.
- Over budget, the cost budget shrinks.
- Under budget, it is at least what is drawn now, and it grows (when it
  is what limits the scale) while there is more than 5% headroom.
- The CPU render budget does not limit the cost model. Recording cost
  barely depends on instanced LOD distances, and letting it hold the
  budget pinned detail at the minimum.

Benchmark frames log the model (`detail_model_active`,
`detail_cost_budget`, `detail_predicted_cost`, `detail_target_scale`,
`detail_placements`, `detail_occluded`).

The model only covers instanced LOD bands. Everything else scales along
with them, by its layer weight.

### Reactive controller (fallback)

Used when there is no recent cost curve, for example in a scene without
instanced LOD bands. The tighter of the two budgets decides:

- The global scale moves toward `scale × sqrt(budget / cost)`. Cost grows
  with the area kept at high detail, which is why the square root is used.
- Between 85% and 100% of the budget, the scale holds steady, so it does not
  hunt around the target.

### Both modes

- Both costs are smoothed (0.25 s) before the controller uses them.
- The scale eases over `responseSeconds` (1.5 s) and stays within
  `minimumScale` to `maximumScale` (0.6 to 8).
- The scale LODs use (`AppliedScale`) follows a copy of the controller
  output quantized to 12% steps, with hysteresis. It glides toward each
  step at up to 12% a second when gaining detail and 25% a second when
  shedding it. Jumping a whole step at once would move every switch distance
  together and pop a ring of objects at the same moment. At a 60 m switch,
  the glide moves the switch ring at 7–15 m/s, about the camera's own
  speed, and instanced LODs dissolve per placement.
- `ENGINE_DETAIL_SCALE=<scale>` pins the global scale and stops
  adaptation, for comparing LOD bands.

Each layer's scale is:

    layer.lodDistanceScale × globalScale ^ layer.budgetWeight

A weight of 0 exempts a layer; a larger weight makes it shed (and gain)
detail first.

### What the scale drives

- **Every LOD'd mesh**, instanced or not:
  - The draw's LOD switch distances. The GPU instance cull, the
    per-placement band selection in the pixel shader and CPU LOD selection
    all read the same scaled values.
  - Cull-after-last-LOD distances.
  - The CPU rejection of instanced LOD bands that cannot draw.
  - The existing quality `geometry` setting multiplies in as before.
- **Terrain far tiles:** the allowed screen error is divided by the Terrain
  layer's scale, so far-tile LOD switches move with it.
- **Grass:** generation reach is multiplied by the grass object's layer
  scale, clamped to 0.5–2×. Generation is CPU work the GPU budget does not
  see.

### Terrain layer assignment

- Terrain cells, terrain batches and distant tiles go in `Terrain`.
- Terrain object layers go in `Grass` if they carry a GrassField, and in
  `Props` otherwise. `TerrainManager::SetObjectLayerSetup` can override this:
  the landscape demo puts its forest in `Trees`.
- Instanced prefab parts and grass tiles inherit their layer object's layer.

### Settings

They are under Project Preferences > Layers > Detail Budget, and in the
project file:

```xml
<DetailBudget Enabled="true" GeometryBudgetMs="6" CpuRenderBudgetMs="4"
  MinimumScale="0.6" MaximumScale="8" ResponseSeconds="1.5"
  TextureBudgetEnabled="true" TextureBudgetMiB="1024" ExtendedTextureBudget="0.5" />
```

The live scale and both measured costs show under the settings. Benchmark
frames also log them (`detail_scale`, `detail_geometry_ms`,
`detail_cpu_render_ms`).

GPU timing must be on for the budget to adapt. DX12 and Vulkan always
collect timings. On DX11 the budget switches timing on while it is enabled.
Without valid timings, the scale holds at its last value.

### Landscape demo, 1080p, RTX 4090, uncapped

| Backend | Frame time | Settled scale | CPU render | GPU geometry |
|---------|------------|---------------|------------|--------------|
| DX12    | 5.9 ms     | about 1.0–1.3 | 2.6 ms     | 1.9 ms       |
| DX11    | 6.2 ms     | about 0.9     | 2.9 ms     | 1.8 ms       |
| Vulkan  | 6.9 ms     | about 1.1     | 3.0 ms     | 2.1 ms       |

After a load, heavy streaming frames dip the scale for a few seconds before
it recovers.

## Detail view

Scene render mode **Detail**, the fifth entry in the scene view's render-mode
menu (`--render-mode 4` in `GameBackendCapture`), colours every pixel from
red (full detail) to blue (least). It takes the lower of two terms:

- **Geometry:**
  - Instanced LOD bands rate by the band drawn: LOD0 red, LOD1 green,
    impostor blue. This is what the detail budget moves.
  - Other meshes (terrain cells, far tiles, props) rate by their mean
    triangle edge on screen. An edge of up to 3 pixels is full detail; 96
    pixels is none.
- **Texture:** texels of the finest resident base-color mip per screen
  pixel. One or more texels per pixel is full detail; one texel per 8 pixels
  is none. This shows streaming, distance mips and textures too small for
  the view alike.

With `ENGINE_OCCLUSION_DEBUG=1`, the Detail view also shows occlusion
culling: everything the occlusion buffer would hide is drawn anyway and
painted magenta. Magenta on screen means something visible would have been
culled. `ENGINE_OCCLUSION_DEBUG=2` marks everything tested, to check that
the overlay works.

In the landscape demo (DX12, 1080p), with the budget settled at about 1.4:

- Trees are LOD0 to about 85 m. Rocks are LOD0 to about 50 m (authored at
  35 m, the cheapest place to buy more detail).
- Near terrain reads blue to cyan. Its grid is 0.5–1 m, so its triangles
  cover 50–100 pixels next to the camera; near-camera relief mapping adds
  detail by shading, which this view does not rate. Mid-range terrain is
  green to yellow and distant hills orange.
- At a pinned scale of 8, everything near is red, but GPU geometry costs
  13 ms against the 6 ms budget.

## Texture budget

Video memory for textures, modelled on GTA V's texture budget and its
Extended Texture Budget slider. It is under Project Preferences > Layers >
Texture Budget, below the detail budget.

- **Base Texture Budget (MiB):** a pool that is always available, whatever
  the GPU. The default is 1024.
- **Extended Texture Budget (%):** adds this share of the GPU's spare video
  memory on top of the base pool. The default is 50%.
  - Spare memory is the OS budget for the process
    (`QueryVideoMemoryInfo`), minus everything that is not texture (buffers,
    render targets, terrain) and minus a reserve of 10%, or at least 512 MiB.
  - It is sampled about once a second. Changes under 256 MiB are ignored, so
    mips are not evicted and reloaded back and forth.
- **Enable Texture Budget:** off keeps every texture fully resident, as
  before.

The pool feeds the texture streamer (`Texture::StreamFrame`):

- Textures that cover the most screen pixels keep their finest mips first.
- When the pool is full, the rest fall back to coarser mips. Nothing
  disappears: every texture keeps its smallest mips. In GTA V, by contrast,
  an exhausted pool can leave textures missing.
- Textures that cannot stream (no mips, CPU-retained) count against the pool
  but are never reduced.

The live line under the settings shows textures in use, the pool (base plus
extended) and process VRAM. Benchmark frames log `texture_budget_mib`,
`texture_resident_mib` and `vram_usage_mib`.

`ENGINE_TEXTURE_STREAMING_BUDGET_MB` still overrides the project setting.
It fixes the pool, and 0 turns streaming off, which is useful for reference
captures.

On an RTX 4090 at 50%, the landscape demo gets a pool of about 10.9 GB and
uses about 220 MiB. So nothing is reduced: frames are pixel-identical to
streaming off, apart from wind and dither noise. A 128 MiB pool still draws
every texture, at coarser mips.

## Not yet covered

- Screen-size LOD thresholds (`screenLod1Pixels`) do not scale yet; only
  distance-based LODs do.
- Terrain cells inside `loadedRadius` are always full resolution, so for
  near terrain only the far tiles respond.
- Per-layer GPU costs are not measured separately. The weights only express
  priority.
- Occlusion only weights the cost model. Hidden placements are still drawn:
  the GPU instance cull has no per-placement occlusion test (for example a
  hierarchical depth buffer), so a deep forest still pays for its hidden
  trees, at whatever band the scale gives them.
- The cost model uses one global scale. Cheap objects (rocks, about 1,400
  triangles at LOD0) and expensive ones (oaks) share it through their layer
  weights, rather than each getting the detail its cost would allow.
