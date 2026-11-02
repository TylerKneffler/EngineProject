# Parallel work: measured render bottleneck and grass GPU stage

The next rendering change was selected from a warmed DX11 grass capture. The
resident 441-cell route averaged 88.97 ms per frame over 240 measured frames.
Resolved GPU stages totaled 87.70 ms, while CPU rendering averaged 12.12 ms.
Native-call timing found no long map or indirect-argument call on that route:
the worst instrumented call was a 0.734 ms camera draw. This evidence does not
justify changing buffer reuse for this route.

The Grass GPU stage now separates grass mesh draws from ordinary opaque draws
on DX11, DX12, and Vulkan. The editor Performance view, frame recordings, grass
capture, and evaluator report the stage. The evaluator compares deduplicated GPU
stage work with CPU render wall time and identifies the largest GPU stage,
without treating asynchronous GPU timing as device utilization.

## Validation and findings

- Debug Editor and GrassPerformanceTests build. GrassPerformance CTest passes;
  15 evaluator tests pass. The Grass stage uses existing GPU timing regions and
  adds no shader or material change.
- A separate 240-frame DX11 capture at 1848 x 1069 averaged 88.40 ms frame
  time, 13.64 ms CPU render work, and 86.78 ms measured GPU stages. Grass took
  63.44 ms (73.1% of GPU stage time); terrain took 13.25 ms and shadows 10.08
  ms. Ordinary opaque draws registered 0 ms in this grass fixture.
- A matched-route half-resolution diagnostic at 924 x 535 averaged 29.95 ms
  grass GPU time, versus 63.44 ms at full resolution. Pixel count fell to one
  quarter while grass time fell by about half. This indicates a substantial
  pixel-dependent cost alongside geometry or fixed work; it does not isolate
  the pixel shader from rasterization and memory effects.
- Captures and evaluator reports are in
  `.local/performance-logs/parallel-{native,gpu}-stage7*` and
  `.local/performance-logs/parallel-gpu-halfres-stage7*`.
- A proposed worker batch for four shadow-cascade spatial queries was removed
  after an isolated Debug benchmark. At 4,096 / 16,384 / 65,536 slots, serial
  queries took 0.015 / 0.028 / 0.097 ms; worker batches took 0.053 / 0.073 /
  0.180 ms. The existing serial query remains the faster path at those sizes.

The captures are separate runs; their frame times are not a controlled
before/after comparison. The next measured performance task is reducing grass
GPU shading, geometry, or coverage cost while checking image quality. Scene
snapshot and simulation/render thread work remains important for scenes whose
CPU update work is on the critical path.
