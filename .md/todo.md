# Engine TODO

Last updated: 2026-11-03.

## Editor Play performance and Stop freezes

- [x] Overlap editor Play simulation with graphics submission through owned Scene/Game view command packets, bounded owner handoffs and GPU resource retirement.
- [x] Reuse completed render packets and command storage; expose simulation work, completion wait, submission overlap and packet recording in Performance recordings and evaluation.
- [ ] Extend worker preparation beyond current culling/packing jobs to the remaining render-item, material and command-recording work from immutable inputs.
- [ ] Validate editor scaling on matched moving/streaming routes across DX11, DX12 and Vulkan; keep stationary measurements separate from gameplay gains.
- [x] Retain the authored scene in memory during Play and restore its original objects, selection, history and edit sessions on Stop.
- [ ] Prepare the separate runtime scene incrementally so Play startup does not block while cloning large terrain/prefab graphs.
- [ ] Retire the stopped runtime scene with bounded owner-thread cleanup after GPU completion; remove the remaining destruction hitch.
- [ ] Cache versioned immutable prefab documents across repeated instances, with file-change invalidation and independent per-instance state.
- [ ] Prepare runtime-copy configuration snapshots from owned data and publish at the owner commit boundary without blocking UI for first serialization.
- [ ] Record separate Play/Stop fence, GPU-wait, snapshot, deserialization and publication timings alongside full-recording transition stalls.

## Foliage landscape baseline and performance gates

- [x] Capture fresh-process DX11 foliage-off, grass-shadows-off, and full-foliage baselines on fixed and moving routes. Record mean/P95/P99 frame intervals, resolved GPU stages, CPU render work, instance residency, and upload/build activity; keep Debug and Release separate. See `foliage-landscape-baseline-20261103.md`.
- [ ] Extend the matched meadow/woodland benchmark with a camera turn, sprint, teleport, and streaming boundaries. Repeat Debug and Release at 790 x 589 and 1920 x 1080 on DX11, DX12, and Vulkan with the same quality settings; report tile-build/upload spikes, visible tiles and placements, triangles, VRAM, and frame P50/P95/P99.
- [ ] Choose the 100 FPS target scene, hardware, resolution, and quality tier. Capture GPU overdraw plus projected coverage, triangle counts, masked pixel area, and LOD overlap for each current grass species and mesh LOD. Check the unexpectedly high 1920 x 1080 foliage-off terrain cost before interpreting that A/B result.
- [ ] Tighten foliage mesh and mask coverage, simplify middle/far clumps, and blend distant coverage into terrain color. Verify stable crossfade coverage and silhouettes in pixel comparisons.
- [ ] Add per-species color distance, shadow caster distance, and density scale to opt-in quality presets; evaluate a short near-field shadow band and simplified next-band caster. Keep camera/shadow fades aligned and compare full-foliage P95 and GPU color/shadow time on the same route.
- [ ] Sweep tile sizes against visible placement count, dispatch/metadata cost, and P95 frame time. Try hierarchy or depth occlusion only if a GPU capture shows missed culling.

## Foliage assets and terrain editing

- [ ] Add reusable foliage species assets with mesh LODs, material, density, placement filters, wind, interaction, and shadow policy; migrate existing GrassField and TerrainObjectLayer scenes without changing their placements.
- [ ] Store per-cell density/biome masks, painted add/erase overrides, exclusion paths, and stable seeds with terrain edits, saves, bakes, and streamed cells.
- [ ] Add terrain object-layer rename, reorder, selection, and visibility controls.
- [ ] Add foliage paint, erase, and replace tools with species selection, density, spacing, scale, rotation, slope, height, and terrain-layer filters.
- [ ] Render ordinary object layers from mesh or prefab references; allow individual plants with scripts or colliders.
- [ ] Generate deterministic foliage from terrain height, slope, layer weights, and species rules; refresh only affected cells after edits.
- [ ] Stream compact foliage cells and tiles around viewers with prefetch, hysteresis, and per-frame generation/upload/residency/retirement budgets. Promote only nearby plants needing collision or scripts.
- [ ] Validate camera crossing, sprint, teleport, terrain edit, save/reload, and Play/Stop for stable placements, no visible holes, bounded memory, and reported peak build/upload time.

## Foliage rendering

- [x] Add shared mesh instancing, tile bounds culling, and placement buffers across DX11, DX12, and Vulkan.
- [ ] Add grass, broadleaf, flower, and shrub materials with masked textures and two-sided lighting.
- [ ] Add vertex-shader wind with pinned roots, varied phases, gusts, and stable pause/export behavior.
- [ ] Add bounded actor-driven grass bending and recovery in vertex shaders.
- [ ] Improve foliage transmission, species color variation, and ground-to-tip shading.
- [ ] Match alpha masking and wind deformation between camera and shadow passes; apply the planned per-species shadow distance and density presets.
- [x] Add smooth grass LOD transitions and distance fade into ground coverage.

## Scenes and quality controls

- [x] Expand the grass stress scene to 21 x 21 terrain cells with grass across the 336 x 336 m field.

- [ ] Extend the flat grass scene with wind, interaction toggles, an exclusion path, and tile/instance/draw counters.
- [ ] Add a representative meadow/woodland scene and foliage quality presets based on measured performance.
- [ ] Extend species placement to shrubs and trees with nearby collision, mesh LODs, distant coarse instances or impostors, and cell HLOD only where silhouettes need it.
- [ ] Author distant terrain and vegetation color together; reduce wind detail with distance and screen size, including shadows. Measure vista silhouette, draw work, and resident memory as world size grows.
- [ ] Keep placement, terrain seams, shadows, LOD transitions, and saved edits consistent across editor, gameplay, export, and all renderers.
