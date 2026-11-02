# Parallel rendering stage 8: matched GPU capture comparison

The 441-cell DX11 grass route spends about 63.4 ms per measured GPU sample in the grass stage at 1848 × 1069. Quarter-pixel resolution reduced that to about 30 ms in stage 7, which points to pixel coverage and shading rather than CPU draw submission as the larger cost.

Two fragment shader experiments were measured and reverted. Skipping the world-space dither hash for fully visible geometry produced 65.5 ms average grass GPU time. Skipping shadow and BRDF work for non-scattering back faces produced 63.5 ms, versus 63.4 ms in the stage 7 baseline. Neither was a meaningful improvement. Both experiments used 240 moving measured frames with the same route, renderer, resolution, and shadow settings. Frame averages varied enough that GPU stage timings are the more useful comparison.

`Scripts/evaluate_performance.py --compare` now checks renderer, benchmark scope, route/version, view dimensions, cascade count, and atlas resolution. It reports per-stage GPU changes only when all are present and match. It explicitly notes that motion and competing hardware load still need review. The evaluator's 16 unit tests pass, including matched and mismatched capture cases.

The broad grass GPU reduction task remains open. A dedicated simple-material grass shader or a geometry/coverage change needs controlled visual and timing checks across DX11, DX12, and Vulkan before it can be considered complete.
