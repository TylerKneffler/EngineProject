# Editor UI attribution and clipped drawing

Continued the editor frame overhead TODOs. Panel draw times now appear in the
Performance view's Timings tab and in both automated frame traces and normal
Performance recordings. Evaluation reports show per-panel mean, P95, P99 and
sample count, plus total panel time and UI time outside those panel scopes.
Document titles are copied and JSON escaped before asynchronous recording.
Duplicate titles are summed within each frame during evaluation.

Percentage grids and history graphs now reserve their normal layout and item
IDs, then skip geometry generation when clipped. Asset tiles skip thumbnail,
fallback icon and hover-decoration drawing when their button is clipped;
wrapped labels, group bounds, selection styling and interaction paths remain.
Hardware counter sampling already ran on a background worker.

## Verification

Debug DX11, grass_flat_test.scene, fixed 1920 x 1080 Game target, existing editor
layout, 60 gameplay warmup frames and 180 measured frames per capture. The editor
loads and plays the scene before measuring and exits after recording. User layout
and asset-location files were saved in memory and restored after each run.

| Measurement | Panel diagnostics baseline | Clipped drawing |
|---|---:|---:|
| FPS from frame intervals | 131.24 | 148.50 |
| Average interval ms | 7.620 | 6.734 |
| Interval P95 ms | 8.743 | 7.634 |
| Interval P99 ms | 12.723 | 8.066 |
| UI construction ms | 1.073 | 1.021 |
| Assets panel ms | 0.468 | 0.445 |
| Performance panel ms | 0.436 | 0.422 |
| Other UI work ms | 0.119 | 0.108 |
| Command recording ms | 3.860 | 3.407 |
| Measured GPU ms | 1.931 | 1.778 |

These are single sequential captures. UI improvement is small, and command
recording changed substantially without changes to its implementation. Overall
FPS differences therefore cannot be attributed to the clipping change. The
interactive capture's earlier 4.69 ms UI cost was not reproduced in this layout.
Assets and Performance remain the largest measured panels. Remaining command
recording, moving-route spikes, broader UI work and moving-shadow flicker stay
open in the TODO list.

The Editor build passed. Four Python regression cases passed through the optional
PerformanceEvaluation CTest: duplicate-document aggregation, measurement filtering
and invalid values, older trace compatibility, and report title escaping. Both
captures parsed successfully; 180 gameplay frames each used 1920 x 1080 and their
panel totals matched UI construction within serialization precision. No records
were dropped and no recurring grass placement uploads occurred. This run does
not establish visual correctness of every scrolled widget or fix shadow flicker.

Artifacts: `build/Debug/ui-overhead-verification-20261103/` contains
`panel-baseline.jsonl`, `panel-clipping.jsonl`, corresponding evaluation JSON and
Markdown files, and `ctest.log`. Build logs are `build/Debug/ui-final-build.log`.
