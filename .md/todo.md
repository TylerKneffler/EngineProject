# Engine TODO

Last updated: 2026-11-03.

## Performance tasks (Oak Meadow / shadows)

- [x] Lower shadow resolution scale clamp floor in [RealtimeLightingPipeline.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Rendering/Lighting/Pipelines/Realtime/RealtimeLightingPipeline.cpp) so scene-authored values below `0.25` can actually take effect for aggressive perf presets.
- [x] Add/verify project-level low-performance shadow preset tuning in [LightingData.h](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Model/LightingData.h) (directional resolution, cascade count, directional distance, grass caster distance, PCF radius) for large foliage scenes.
- [x] Optimize PCF shader path in [Object.hlsl](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Shaders/Object/Object.hlsl) by specializing small-radius kernels and removing unnecessary loop/branch work per pixel.
- [x] Add shadow-caster proxy workflow for heavy foliage/terrain (low-poly or primitive caster meshes) and route runtime shadow pass to proxies where enabled.
- [x] Investigate temporal/tiled shadow update strategy to avoid full cascade redraw every frame in [SceneRendering.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Scene/Rendering/SceneRendering.cpp).
- [x] Revisit terrain shadow path in [SceneRendering.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Scene/Rendering/SceneRendering.cpp) to reduce forced double-sided shadow cost where safe.

## Modern renderer performance roadmap

- [x] Implement true tile/page-based directional shadow updates (dirty-region invalidation + per-page redraw) instead of whole-atlas refresh in [SceneRendering.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Scene/Rendering/SceneRendering.cpp).
- [x] Add explicit shadow caster budgeting (max casters per cascade + priority scoring by distance/screen size/importance) in [SceneRendering.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Scene/Rendering/SceneRendering.cpp).
- [x] Add far-distance shadow replacement for foliage/trees (proxy casters + optional no-shadow beyond strict cutoff) wired through [Mesh.h](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Compoonents/Obj/Mesh.h) and [SceneRendering.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Scene/Rendering/SceneRendering.cpp).
- [x] Add HLOD/impostor pipeline for distant vegetation clusters and integrate with terrain streaming in [TerrainManager.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Compoonents/Obj/TerrainManager.cpp).
- [x] Add distance-based material/shader quality tiers (disable expensive features by band: normals/parallax/subsurface/reflections/shadow filtering) in [LightingData.h](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Model/LightingData.h) and [Object.hlsl](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Shaders/Object/Object.hlsl).
- [ ] Add dynamic internal render scale / temporal upscaling mode for heavy scenes with quality target controls (75%/67%/50%) across renderer backends.
- [ ] Add hard scene budgets and runtime diagnostics (visible triangles, shadow casters, overdraw proxies, material complexity counters) with alerts in [PerformanceView.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Editor/Core/View/Views/PerformanceView.cpp).
