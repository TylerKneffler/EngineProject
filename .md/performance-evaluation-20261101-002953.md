# Current landscape bottlenecks

Capture: `.local/performance-logs/performance-20261101-002953-778.jsonl`.
1,039 Play frames over 17.83 seconds, one 1848 x 1069 Game view.

Measured throughput is **58.27 FPS**: 17.16 ms mean frame interval,
29.00 ms P95 and 45.26 ms P99. CPU frame work averages 16.41 ms.
Grass is present: approximately 3,354 active placements on average.

## Bottlenecks in priority order

1. **Rendering submission stalls drive stutter.** Main-pass submission totals
   5.79 ms average / 11.35 ms P95. Its geometry submission subphase alone averages
   2.45 ms, reaching 44.39 ms. Shadow submission totals 3.16 ms average / 9.20 ms
   P95. The shadow submission loop reaches 46.26 ms, with one batch taking
   45.65 ms. These are nested measurements, not additional costs to sum.
   The worst shadow submission used 3.74 million thread cycles, versus a typical
   submission density of 2.66 million cycles/ms. The evaluator flags 236 long,
   low-cycle-density submissions. Waiting or descheduling is a stronger lead than
   increased CPU calculation for these spikes; a specific driver/GPU wait is not
   proven. Add native-call timing around compute-constant Map, dispatches, resource
   binding, indirect-argument preparation and indirect draws before choosing the
   exact fix. Consider a ring of compute constants and indirect arguments if
   resource reuse is confirmed as the blocking point.

2. **GPU shadows dominate measured scene GPU work.** Four cascades and a
   2048-pixel atlas cost 6.53 ms average / 11.48 ms P95, 66.1% of measured GPU
   stages. Opaque rendering costs 2.87 ms and terrain 0.48 ms. Total measured
   scene GPU time averages 9.88 ms / 14.98 ms P95. Compare the same camera route
   with two cascades and tighter grass caster distance, then examine redundant
   per-cascade LOD dispatches and grass shadow overdraw. Retain a controlled
   quality comparison rather than silently changing visual quality.

3. **Recurring main-thread scene work remains substantial.** Object updates
   average 2.12 ms / 4.00 ms P95, including 0.48 ms of grass scheduling. Render
   preparation averages 2.20 ms: item preparation 1.31 ms, spatial indexing
   0.37 ms and capacity-related work 0.28 ms. Reuse static render-item/bounds
   preparation and profile the individual updating component types before moving
   further work onto workers. Worker execution averages 0.41 ms summed across
   workers, and waits average only 0.007 ms. The main thread averages 82.3% of one
   logical core; the process averages 0.95 core equivalents across hardware
   samples. Work is still concentrated on the main thread, with periods of waiting.

4. **Editor UI adds a steady 1.70 ms.** This is meaningful at a 16.67 ms frame
   budget, but it does not explain the 40+ ms submission spikes. Retained scene
   UI is only 0.010 ms; the larger editor UI cost is separate. Present averages
   0.094 ms and pacing 0.003 ms, so neither dominates the recorded frame work.

## Improvements confirmed

- No placement rescans, edited placement uploads, fallback uploads or pool-growth
  uploads. The instance pool remains at 524,288 bytes.
- New placements upload approximately 348 bytes per frame on average; tile
  metadata averages approximately 9.4 KB across camera and shadow passes.
- 198 new tiles were prepared. Frames with no new tiles still average 16.99 ms,
  versus 18.12 ms for frames building tiles; streaming is not the main steady cost.
- Grass updates average 0.48 ms / 1.05 ms P95.
- Physics averages 0.63 ms: setup 0.046 ms, simulation 0.061 ms, post-processing
  0.522 ms. Only 1–2 of 441 registered terrain colliders are active. Physics is
  currently a secondary cost, and post-processing is its largest remaining part.

## Measurement limits

CPU wall-time phases and asynchronous GPU measurements overlap; do not add them.
The `dt_ms` interval ends at the start of the current update and can reflect the
preceding frame's spike; same-row CPU work and GPU results need not correspond to
that interval. GPU stage results also resolve asynchronously. This capture has no
movement markers, so stationary-versus-moving causality cannot be established.
Hardware GPU-engine percentages cover all adapters/processes, not only the editor.
Evaluator output is saved under `.local/performance-logs/evaluation-20261101-002953.*`.
