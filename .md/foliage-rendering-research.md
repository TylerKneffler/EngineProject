# High-FPS terrain, foliage, and material rendering: research applied to EngineProject

Research checked 2026-11-09. Recommendations below synthesize primary technical sources with the renderer and oak meadow source tree. EngineProject-specific timings are diagnostic measurements from its local logs; external papers and engine documentation explain techniques, not promised speedups for this project.

## What current evidence says about this forest

The strongest local signal is pixel cost, not just tree triangle count. The supplied DX12 Debug Editor log on an RTX 4090 has 1,097 valid frames averaging 20.62 ms tree leaves, 2.11 ms bark, 0.50 ms terrain, 1.85 ms shadows, and 25.34 ms total GPU time. A separate current-source Release audit at 960x540 put leaves near 4.8 ms and bark near 1.5 ms in its overlook route. These runs use different build/configuration and resolution, so compare within each run, not across them. Pipeline pixel-shader invocations are not a direct overdraw count, but they are useful alongside a dedicated overdraw visualization.

A controlled shadow-LOD trial matters because it disproves a common shortcut: selecting a lower triangle LOD for distant tree shadows did not lower total shadow cost. With full directional shadow refresh on every measured frame, the distance-selected impostor used about 25–31% fewer shadow vertices but 70–94% more fragment work and 38–69% more shadow GPU time than fixed LOD1 across entrance, overlook, and moving routes. The lower-detail impostor covers more shadow pixels. Keep the oak scene at fixed shadowLod=1 pending a lower-coverage shadow proxy.

The scene already has several useful modern building blocks:
- `SceneInstanceBatching.cpp` uses persistent placement data, GPU instance culling, and indirect draws where the backend supports them; CPU fallback remains.
- `TerrainPrefabInstances.cpp` packs trees by species/LOD and a 256 m world-space group, sharing placement transforms across resident LOD bands.
- `TerrainObjectLayer` exposes optional screen-size LOD selection and hysteresis. The oak scene currently authors distance cutoffs 28/90 m and does not enable screen-size LOD.
- Terrain far-field code already supports tile screen-error thresholds, LOD hysteresis, far-vegetation impostors, and batched far cells. A new geometry-clipmap terrain system is not the first experiment to try.
- Dedicated shadow-only mesh chains are supported, so visible LOD and shadow proxy can be tested independently.

## Techniques and practical tradeoffs

### 1. Reduce foliage pixel work before replacing the renderer

Masked leaf cards can remain expensive even when they have few triangles: every covered sample evaluates the alpha test and often shading, while overlapping cards multiply coverage. Epic's current Nanite guidance explicitly warns that masked-out pixels still cost close to drawn pixels and that traditional card foliage can be slower with Nanite. The same fill-rate principle applies to this raster renderer. See [Epic's Nanite foliage guidance](https://dev.epicgames.com/documentation/unreal-engine/working-with-naniteenabled-content) and its [foliage overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage).

For this engine, first add an overdraw/coverage view or controlled shader ablation for the existing `Foliage.hlsl` route. Then test compact cards with tighter transparent borders, simpler distant leaf shading, and mip-aware alpha coverage. Test an alpha-tested depth prepass only where the saved repeated shading exceeds the extra geometry/depth pass; it can lose when depth complexity is low. Compare GPU time, pixel invocations, silhouette, foliage density, and animation at identical camera/exposure settings.

### 2. Make LOD selection screen-aware and preserve canopy area

Distance-only LOD gives different screen sizes as tree scale, FOV, and output resolution change. Projected-size or screen-space-error thresholds with hysteresis keep work aligned with what can be seen and reduce threshold flicker. Dither/alpha-tested cross-fades can hide transitions, but overlapping transition LODs can increase fill cost. NVIDIA's SpeedTree chapter describes alpha-tested fizzle cross-fades and their coverage caveats; [the chapter](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering) is still a useful implementation reference. Epic's foliage documentation also discusses cluster LOD and distance fades, with the caveat that whole clusters can transition together: [Foliage Mode](https://dev.epicgames.com/documentation/unreal-engine/foliage-mode-in-unreal-engine).

EngineProject already has `screenLodSelection` and hysteresis in generic mesh and terrain-object paths. The oak layer leaves it off. Make a reversible audit candidate that enables screen-size selection for the oak chain, compare it against 28/90 m distance LOD on entrance/overlook/zoom/moving routes, and tune projected thresholds per asset scale. Evaluate color and silhouette continuity as well as GPU time. Avoid enabling a screen-size mode globally until foliage-specific behavior is validated.

### 3. Use spatially coherent GPU culling, but tune cluster size empirically

GPU frustum/occlusion culling plus indirect draws can avoid CPU submission of invisible instances. Microsoft's [D3D12 indirect-drawing and GPU-culling guide](https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing-and-gpu-culling-) documents the command-buffer pattern. The SIGGRAPH 2024 NanoMesh presentation describes an adaptive cluster pipeline with instance and cluster frustum/HZB/cone culling, LOD error selection, and demand-driven streaming: [Seamless Rendering on Mobile](https://advances.realtimerendering.com/s2024/content/Cao-NanoMesh/AdavanceRealtimeRendering_NanoMesh0810.pdf).

This project already does per-placement GPU visibility and LOD selection within batches, so the near-term question is the 256 m spatial group size and visible subset quality. Benchmark smaller coherent groups (for example 64/128/256 m) under dense-canopy and overlook routes. Measure culling and dispatch overhead, submitted instances, pixel/vertex work, and pop-in; smaller groups are beneficial only if tighter bounds save more raster cost than they add in management and batches.

### 4. Separate shadow geometry and optimize projected coverage

A shadow LOD should minimize shadow-map coverage and alpha-tested fragment work, not merely triangles. NVIDIA's SpeedTree work covers cascaded shadow maps, LOD-aware updates, leaf cutouts, and cross-fades in [GPU Gems 3](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering). For EngineProject, author a dedicated shadow-only chain: trunk/major branches plus a sparse canopy proxy with controlled opacity/coverage. Keep the visible tree LODs independent. Compare full shadow refresh and cached behavior, caster vertices/primitives/fragments, GPU shadow time, ground contact, and moving-view stability on DX11, DX12, and Vulkan.

### 5. Use impostors only when their pixel footprint wins

Impostors trade geometric work for image-space work; they do not make cost disappear. NVIDIA's [True Impostors chapter](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-21-true-impostors) explicitly notes view-angle error and fill-rate/overdraw limits. For the oak's far band, compare the current billboard against multi-view/octagonal or layered impostors and a compact low-density proxy. Measure projected opaque/masked coverage, fragments, color continuity, shadow footprint, and view-angle error. The local 55 m transition test shifted canopy ROI mean brightness substantially; the 0.85 tint trial did not materially fix it. Therefore bake/texture/mip/shading and transition behavior should be separated rather than guessed at by changing material tint.

### 6. Two additional alternatives: hierarchical leaf LOD and GPU-generated trees

**Hierarchical leaf-cluster LOD (BroadLeaf-style)** is an alternative to switching the entire tree between coarse prefab bands. BroadLeaf describes organizing explicit leaf geometry hierarchically so LOD selection and culling can discard foliage at finer granularity while maintaining smooth transitions; see the [GDC 2023 session description](https://www.gdcvault.com/play/1028728/BroadLeaf-Real-Time-Cinematic-Rendering). For this project, prototype only the oak leaf layer and compare whether finer cluster rejection reduces submitted geometry and pixel coverage enough to offset hierarchy traversal and extra work. This is promising for irregular visibility and mid-distance trees, but may add CPU/GPU management and can create canopy holes if the hierarchy does not preserve aggregate coverage.

**GPU procedural tree generation** is a more radical alternative aimed at geometry storage and authoring/runtime generation. The 2025 Eurographics/High-Performance Graphics paper [Real-Time GPU Tree Generation](https://gpuopen.com/download/Real-Time_GPU_Tree_Generation.pdf) uses GPU work graphs and mesh nodes for on-GPU procedural geometry, culling, animation, and continuous LOD. Its reported results are from its test scene and AMD Radeon RX 7900 XTX, and should not be treated as a forecast for EngineProject. Work graphs/mesh nodes are not a portable drop-in for this renderer's DX11/DX12/Vulkan support; first prove a narrow API/device path and assess fallback, procedural asset conversion, wind, shadows, deterministic output, and editor tooling. Because the present evidence identifies raster/pixel work as the dominant oak runtime cost, treat this as a separate memory/startup and long-term architecture investigation, not the first frame-time optimization.
### 6. Treat virtualized geometry and clipmaps as conditional, larger projects

Nanite foliage shows one modern direction: repeatable assemblies, cluster culling, and near-pixel voxel representations preserve aggregate canopy area at distance. Its documentation also labels the foliage work experimental and warns about masked-card overdraw, WPO cost, content memory, and unsupported behavior. It is a research reference, not a drop-in path for a renderer that must support DX11, DX12, and Vulkan. See [Nanite Foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage).

GPU geometry clipmaps are useful for huge heightfield terrain because nested regular grids update incrementally around the viewer; NVIDIA's [GPU Gems 2 clipmap chapter](https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry) details the approach. EngineProject already has screen-error far terrain, so only investigate clipmap replacement if frame captures show terrain geometry or streaming—not oak leaves—dominates the target scene.

## Recommended experiment order for this repository

1. Add foliage overdraw visualization and collect per-LOD GPU counters on the existing deterministic routes.
2. Compare current oak distance LOD against reversible screen-size LOD thresholds, with matched color/silhouette captures and moving views.
3. Sweep tree batch spatial group sizes around the existing 256 m value; retain GPU instance culling.
4. Build one dedicated lower-coverage shadow proxy and measure full-refresh shadow time and quality.
5. Only then evaluate alternative far impostors and simplified far materials, isolating mip/albedo, shading, and shadows.
6. Keep Nanite/mesh-shader/voxel foliage and terrain clipmaps as explicit optional research branches unless cross-backend measurements justify their implementation cost.

For every experiment, record backend, GPU/driver, build, resolution, camera path, exposure, LOD policy, shadow refresh state, warmup/sample counts, frame/GPU-stage distributions, pixel/vertex counters, memory, and matched captures. Require repeatable net frame-time gains plus acceptable image quality; counts alone are not success criteria.

