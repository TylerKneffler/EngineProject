# Engine TODO

- [ ] Test oak leaf materials with the existing cheaper foliage shader. Compare GPU timings and near/mid/far appearance against the general material shader before switching defaults.
- [ ] Reduce oak canopy overdraw with fewer overlapping leaf cards and tighter card silhouettes. Measure alpha coverage and GPU cost while preserving canopy density and silhouette.
- [ ] Evaluate cheaper distant canopy geometry and tighter impostor silhouettes. Compare pixel coverage as well as triangle counts; distant impostors still pay for alpha masking.
- [ ] Tune oak alpha LOD material overrides: compare preserved coverage, increasing alpha mip bias, and disabling alpha testing at far distances. Check exposed card rectangles, transition popping, and matching shadows before enabling defaults.
- [ ] Measure the extra base-color/alpha sampling cost when their LOD biases differ. Compare matched biases, which already share a sample, against independent coverage quality before changing the sampling policy.
- [ ] Isolate oak bark and leaf GPU costs and measure leaf overdraw. Repeat full-scene, terrain-only, trees-only, and resolution comparisons to distinguish shading, pixel coverage, geometry, and submission costs.
- [ ] Compare oak canopy changes across DX11, DX12, and Vulkan using matched near/mid/far views, resolution, warm-up, and measured frame counts. Record the GPU adapter, per-pass GPU timings, alpha coverage, and appearance before choosing defaults.
