# Engine TODO

Last updated: 2026-11-03.

## Foliage rendering performance

- [x] Add timings within render-item preparation for material/shader preparation, mesh state, bounds, LOD metadata, and draw-item construction. Baseline: 9.87 ms foliage-only versus 0.14 ms terrain-only at 1920 x 1080 in Debug DX11.
- [x] Cache prepared rigid instanced render records, including foliage. Invalidate on geometry, placement, or transform edits; refresh material state, fades, and GPU LOD parameters on reuse. Preserve full preparation for CPU LOD transitions, deformation, previews, and portal/warp paths.
- [x] Read prepared transform revisions during the stable render phase so cached records avoid repeated parent-chain validation.
- [x] Share shader source resolution and file checks across materials using the same authored path and terrain/mesh variant each frame; retain per-frame hot reload and direct script edits.
- [x] Cache normalized material validation inputs and resident texture preparation by texture/provider state. Detect direct script edits, pending uploads, invalidation, replacement, and provider changes without relying solely on revision notifications.
- [x] Reuse resident foliage placement buffers with GPU instance visibility, LOD selection and indirect counts. Register each source once per view, cache camera-independent tile records, and reuse them across LODs and shadow cascades; keep coarse CPU tile rejection and explicit CPU fallback paths.
- [x] Build changed instance placement and tile render records from immutable snapshots, with worker-local output for large batches and owner-side publication/upload. Preserve retained-reader lifetimes and skip worker scheduling for unchanged data and small edits.
- [ ] Isolate remaining ordinary material/object record assembly from mutable material, probe and graphics-resource caches before expanding worker preparation beyond the instance record paths.
- [x] Reduce average foliage render-item preparation below 2 ms and validate 100+ FPS in the stationary full editor/gameplay loop at 1920 x 1080 with unchanged visual quality. Debug DX11 result: 0.68 ms item preparation, 162.0 FPS, frame P95 7.12 ms.
- [ ] Validate sustained frame budgets in Debug and Release across stationary, moving, and streaming cases. Repeat terrain-only, foliage-only, and combined benchmarks; report frame P50/P95/P99, CPU preparation, simulation/waits, GPU stages, and upload/build spikes. Address remaining moving-route and shadow submission spikes.

## Shadow stability

- [x] Fit directional shadow cascades in camera-local space with double-precision projection rays and exact slice depths. Keep radius and world texel-grid phase stable under fixed-position yaw/pitch and translation; record cascade radii.
- [x] Fix camera-sorted shadow record uploads to cover the highest changed persistent slot. Add DX11 GPU readback checks for moving/rotating cameras, one/four cascades and ordinary/native packets; record shadow enable, cascade and atlas state for future captures.
- [ ] Investigate reported shadow flicker while moving; verify repeated-frame pixels, cascade boundaries, grass shadow-distance transitions, residency changes, and native command recording on DX11/DX12/Vulkan.
- [x] Add real shadow-receiver fade coverage and repeated-frame pixel checks for ordinary/native-recorded rendering on DX11/DX12/Vulkan. Fix the small fixture's receiver normals and depth bias so it measures visible shadows.
- [x] Restore DX12 standalone game color/depth targets, viewport and scissor after depth-only shadow passes.
- [x] Replace grass tile shadow-distance toggles with GPU distance fading and per-instance shadow-range culling while retaining resident placement buffers. Reusable meshes expose transient caster range/fade settings; color-pass visibility stays independent.

## Editor frame overhead

- [x] Record UI construction by panel in the Performance view, recordings and evaluation reports, including panel totals and remaining editor UI work.
- [x] Skip clipped percentage-grid, history-graph and asset-thumbnail drawing while preserving layout and interaction geometry.
- [ ] Reduce remaining Assets and Performance panel construction work and reproduce the interactive 4.69 ms UI cost. Matched 1080p captures show 1.07 to 1.02 ms total UI work; Assets and Performance account for most of it. Hardware sampling already runs in the background.
- [ ] Reduce remaining render-command recording work using cached bindings and immutable preparation snapshots; the same capture averages 3.10 ms in packet recording.

## Current frame-budget priorities

- [x] Partition command recording into compute, scene snapshot/resource retention, view recording and remaining bookkeeping; expose the breakdown in Performance view, recordings and evaluation reports.
- [x] Cache directional-only clustered-light grids per GPU frame-slot buffer and layout. Skip recurring grid construction/uploads; invalidate on local lights, layout changes and buffer replacement.
- [ ] Reduce remaining view command preparation/recording using cached bindings and immutable draw descriptions (latest matched capture: 3.04 ms total recording, 2.95 ms inside views).
- [ ] Rerun the 1080p moving route after the shadow fixes, then reduce shadow caster/cascade coverage, repeated culling and submission spikes while preserving nearby shadows.
- [ ] Cache recurring Assets and Performance panel formatting/layout work (latest stationary costs: 0.49 ms and 0.42 ms).
- [ ] Replace recurring shader-source resolution/checks with change-driven invalidation while retaining hot reload, direct path edits and missing-file recovery (latest preparation cost: 0.33 ms).
