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

The tighter of the two decides. When there is spare time, detail moves
farther out; when either budget is exceeded, detail pulls in.

- The global scale moves toward `scale × sqrt(budget / cost)`. Cost grows
  with the area kept at high detail, which is why the square root is used.
- Both costs are smoothed (0.25 s) before the controller uses them.
- The scale eases over `responseSeconds` (1.5 s) and stays within
  `minimumScale` to `maximumScale` (0.6 to 2.5).
- Between 85% and 100% of the budget, the scale holds steady, so it does not
  hunt around the target.
- The scale LODs use (`AppliedScale`) follows a copy of the controller
  output quantized to 12% steps, with hysteresis. It glides toward each
  step at up to 4% a second when gaining detail and 12% a second when
  shedding it. Jumping a whole step at once would move every switch distance
  together and pop a ring of objects at the same moment.

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
  MinimumScale="0.6" MaximumScale="2.5" ResponseSeconds="1.5"
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
