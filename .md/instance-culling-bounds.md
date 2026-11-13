# Per-instance culling bounds — 2026-11-12

The GPU instance-culling input mixed two coordinate spaces: a shared source mesh
box and the aggregate box of an entire placement tile. The compute shader then
applied each placement transform to that enlarged box. A tree could consequently
inherit the extent of a 256 m group, retaining off-screen geometry and multiple
distance LODs long after their visible fragments would be rejected.

`Mesh::GetInstanceGeometryBounds` now returns one source's local bounds, including
source deformation and the batch's explicit/wind padding. Wind height comes from
the source, not translated placement heights. Runtime LOD variants inherit the
owning draw's deformation policy. `SceneInstanceBatching` unions resident source
representations in this coordinate space and passes them to compute. Aggregate
batch bounds remain in use for CPU spatial traversal.

This change preserves the oak meshes, density, distance thresholds, materials,
resolution and far impostors. It fixes visibility selection rather than reducing
authored detail. The benchmark now exposes asynchronous GPU selected/transition
counts by LOD; those counts describe mesh-part instances, not unique trees.

Initial validation uses the expanded oak meadow, Release, RTX 4090, 1920x1080,
native scale, uncapped, 1,200 warm-up and 240 measured frames. Against the previous
indexed-instance milestone, DX12 median frame time is 16.74 → 14.30 ms, opaque GPU
time 13.37 → 8.11 ms, and GPU shadows 1.56 → 1.07 ms. CPU candidate triangles remain
79,400,310 because the change happens after submission. All 240 frames pass.

The top 520 rows of the DX12 capture (the canopy) match the previous capture
exactly, pixel for pixel. Grass wind phase varies between captures. Initial DX11
and Vulkan captures also pass 240/240 frames; median frame times are 14.99 ms and
14.62 ms respectively. These are observations of separate runs, not isolated
per-pass causal measurements. CPU/GPU timings overlap.

The regression test covers widely separated and vertically translated placements,
ensuring their aggregate extent never enters source-local culling bounds. It also
checks wind, explicit padding, inherited runtime-LOD settings and rejection of
non-instanced sources. Evidence is under `.local/instance-bounds-*`.

The 120 FPS / 8.33 ms target remains open. CPU command recording is now a major
remaining cost (about 8 ms in the initial DX12 run), alongside roughly 3.6 ms of
preparation. The opaque GPU pass still consumes nearly the whole target budget.

Final repeat after the inherited-LOD padding check: DX12 median/p95 is
14.60/18.46 ms, with opaque/shadow GPU times of 8.21/1.07 ms. The canopy comparison
is still exactly equal. Final DX11 is 15.61/17.31 ms and Vulkan is 13.80/15.23 ms;
all three runs pass 240/240 frames. Run-to-run variation remains, particularly on
the CPU. The final DX12 command-recording median is 8.38 ms. Release regression
tests `IndexedInstanceGeometry`, `DrawBatchPreparation` and
`InstanceTilePreparation` all pass.

The Debug Editor builds and the indexed geometry/bounds regression passes in
Debug as well. A final DX12 movement check passes 300/300 frames, moving 31.28 m
from z=170 to z=201.18 across the z=180 terrain seam while retaining eight streamed
neighbors. Its timing is excluded from performance comparisons because it
overlapped the Editor build. The final scene capture shows continuous terrain,
grass and canopy. Summarized evidence: `.local/instance-bounds-validation.json`.
