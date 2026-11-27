# Smoother LOD exchanges (research, 2026-11-27)

User, 2026-11-27: "research some ways to handle the changing of LODs for a smoother exchange... right
now there is a lot of abruptness to it and it's all around when running the flight run".

## What still changes in one step (transition monitor, FlightTourLow at 250 m/s, per km)

| Change | Per km | How it changes today |
|---|---|---|
| Far terrain node set swap (`TerrainManagerFarVolume`) | 28 | the whole distant set in one frame, no fade |
| Volume chunk level change (`VolumeTerrain::SwapIn`) | ~650 | 0.35 s complementary dither of two different surfaces (screen threshold since 2026-11-27) |
| Far forest cluster <-> single trees (`farClusterDistance`) | ~12 | one frame |
| Cell handed back to far impostors (unload) | ~53 | one frame (behind the camera, >= 4 cells) |
| Grass tile LOD | ~2750 | time-based fade (`fadeDuration`) |
| Tree LOD bands | continuous | distance dither, band widened with speed (`lodTransitionSeconds`) |
| Near cell trees <-> far impostors | ~48 | cross-fade, overlapped (`CellHandover`), none abrupt |

The terrain rows are the ones "all around": a hill's shape changing reads more than a tree's.

## Techniques

1. **Geomorphing (CDLOD-style) for terrain.** Each vertex of a finer level also stores where the
   coarser level's surface is under it, and the vertex shader moves it toward that position as the
   distance approaches the switch, so at the switch the fine mesh already has the coarse shape and the
   swap changes nothing on screen. No dither, no overlap, no holes. CDLOD morphs vertices by camera
   distance in the vertex shader ([terrain-cdlod](https://github.com/tschie/terrain-cdlod),
   [Terrain Geomorphing in the Vertex Shader](https://gamedev.net/tutorials/programming/graphics/terrain-geomorphing-in-the-vertex-shader-r1936));
   a per-patch morph factor from 0 (finest) to 1 (coarsest) ([Vertex morphing action](https://www.gamedev.net/blogs/entry/1007337-vertex-morphing-action)).
   Our chunks are surface nets, not grids, so the "coarse position" is found at build time by sampling
   the coarser level's surface under each vertex (the volume at the coarse stride, or a ray against
   the coarse mesh) and stored as an extra vertex attribute. Applies to volume chunk levels and to far
   nodes (split/merge), whose surfaces come from the same volume.
2. **Per-vertex next-LOD positions for vegetation (SpeedTree).** SpeedTree stores each vertex's position
   in the next LOD and interpolates on the GPU; leaves shrink away while the rest scale up; the last
   step to a billboard uses alpha-to-coverage ([Unity: LOD transitions](https://docs.unity3d.com/Manual/lod/lod-transitions-lod-group.html),
   [SpeedTree runtime LOD](https://docs.unity3d.com/speedtree-runtime-sdk/manual/level-of-detail.html)).
   Needs authored LOD chains with correspondence; our tree LODs are separate meshes, so the cheaper part
   is leaf thinning: past a distance, leaf cards shrink away by a per-card rank (as grass thinning does)
   so the LOD1 -> LOD2 -> impostor steps remove little at each switch.
3. **Time-based cross-fade (Unity "Animate cross-fading").** The fade starts at the threshold and runs
   for a fixed time; Unity added it because distance-based fades left trees stuck half-dithered
   ([Unity LODGroup](https://docs.unity3d.com/2022.3/Documentation/Manual/class-LODGroup.html)). Ours is
   speed-scaled distance bands (close to time-based while moving; a tree hovering at a switch stays
   dithered, TAA resolves it). A true time-based fade needs per-placement state in the cull pass.
4. **Impostor frame blending and depth.** Blend the three nearest baked views by barycentric weight;
   bake depth and use it for parallax and to write depth, so the impostor sits in the same place as the
   mesh and the mesh-to-impostor fade does not jump ([Imposter baking and rendering](https://arycama.github.io/2024/10/05/Imposters.html),
   [Octahedral imposters](https://smailikhappy.github.io/posts/Octahedral_imposters/),
   [Octahedral impostors README](https://unpkg.com/@woosh/meep-engine@2.124.2/src/engine/graphics/impostors/octahedral/README.md)).
   We blend three views up to 600 m and snap to one past it (a GPU cost trade); popping also comes from
   too few views ([A forest of octahedral impostors](https://discourse.threejs.org/t/a-forest-of-octahedral-impostors/85735)).
5. **Layered forest representations.** Serious Sam 4 stacks terrain, forest impostors, near trees,
   plants, grass, and splats the far forest into the terrain past the impostors
   ([GDC 2019, 4 million acres](https://gpuopen.com/gdc-presentations/2019/gdc-2019-agtd2-4-million-acres-serious-sam-4.pdf));
   we have the same stack (canopy colour on far terrain, clusters, impostors, meshes); each boundary
   needs a fade, not just the near one.
6. **Alpha-test / dither transitions without transparency** are the standard for discrete LODs
   ([Candussi et al. 2005](https://sites.cs.ucsb.edu/~holl/pubs/Candussi-2005-EG.pdf)), with two rules
   we learnt the hard way: both sides must read the same screen threshold (a world-hashed threshold left
   holes: terrain went see-through), and for shapes that differ an overlapped fade (new in, then old out)
   avoids the thinned middle of a split.

## Plan (in order)

1. **Far terrain set swap -> cross-fade** (cheap, the largest one-frame change): keep the old node
   set drawing while the new one fades in, with the runtime-LOD fade the chunks use (screen threshold),
   then retire it. 0.3-0.5 s.
2. **Geomorph volume chunk levels** (removes the 650/km shape changes): store the coarser level's
   surface position per vertex at build time; morph in the vertex shader over the last part of each
   level's range; the swap then happens on matching shapes (keep the short dither as a safety net).
   Then the same for far node split/merge.
3. **Cluster <-> single tree flips -> cross-fade**, with the handover mechanism (a second, distance
   ring at farClusterDistance) or by keeping the old block while the new one fades in.
4. **Hysteresis everywhere** a level is chosen by distance: far node split/merge and volume chunk
   levels have none (cells have the unload margin since 2026-11-27).
5. Later: leaf thinning toward the impostor; impostor depth; per-placement time-based fades.
