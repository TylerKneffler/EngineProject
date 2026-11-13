# CPU material recording follow-up

Target: 120 FPS at 1920x1080, retaining expanded oak meadow density and quality.

## Changes

- Material alpha-mode queries recognize canonical `Opaque`, `Mask`, and `Blend` directly. The existing case-insensitive fallback still handles public-field edits and older asset values.
- Camera and shadow recording resolve scene-wide texture LOD policy once per view. Explicit material overrides are still evaluated for each draw, with the existing alpha-coverage and fixed-shadow-band adjustments applied afterward.
- Standalone benchmark JSON now exposes camera startup, culling, sorting, occlusion, upload, submission, object-record packing/waiting, and instance-tile preparation. It also exposes shadow preparation, cascade culling, upload, submission, and total CPU time.

The renderer already retains geometry, placement data, instance tiles, and batch membership. These changes remove smaller repeated material-policy calculations; they do not introduce a new geometry hierarchy or change rendered detail.

## Measurements

Release DX12, RTX 4090, native 1920x1080, fixed expanded-meadow camera, uncapped, 1,200 warm-up frames followed by 240 measured frames:

| Run | Median frame | p95 frame | Command recording | Camera material | Shadow CPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| Instrumented baseline | 11.82 ms | 13.19 ms | 6.50 ms | 1.39 ms | 1.67 ms |
| Alpha-mode fast path | 11.67 ms | 13.14 ms | 6.41 ms | 1.36 ms | 1.66 ms |
| Both material changes | 12.79 ms | 15.27 ms | 7.15 ms | 1.43 ms | 1.81 ms |

The combined run also increased unchanged scene-preparation time from 3.15 to 3.35 ms and opaque GPU time from 7.40 to 8.02 ms. These samples do **not** establish an overall FPS improvement. The new baseline was already faster than the previous bounds-fix run before these material changes; do not attribute that difference to this work.

Longer 1,200-frame samples returned 13.82 ms median / 17.15 ms p95 for the combined changes and 12.80 / 15.84 ms for a subsequently rebuilt original-material baseline. An interactive Debug editor session was also recorded during this measurement window (`performance-20261112-194914-332.jsonl`), so these are not isolated timing comparisons. Performance acceptance remains open. The user's Scene-view visibility report took priority; see [the editor LOD preview fix](editor-foliage-lod-preview.md).

All short captures have 240 valid frames, nine terrain components, eight streamed cells, 177 submitted draws, and 79,400,310 CPU candidate triangles. The top 520 image rows (canopy) match the baseline byte-for-byte. Animated grass prevents treating full-image differences as a static reference.

Evidence: `.local/cpu-material-validation.json`, with source logs under `.local/cpu-record-baseline`, `.local/cpu-alpha-after`, and `.local/cpu-policy-after`.

## Validation and remaining work

Debug Editor and Release GameBackendCapture build successfully. Release IndexedInstanceGeometry, DrawBatchPreparation, and InstanceTilePreparation tests pass.

120 FPS remains unmet. Camera material assembly is the largest named camera CPU stage, but it is only part of recording time. Further work should isolate its costs before adding cache invalidation complexity. GPU opaque work plus shadows and grass also exceed the 8.33 ms frame budget in these captures; CPU improvements alone cannot establish the target.
