# Intermediate oak canopy LOD

Completed 2026-11-08. Four approved spatial canopy meshes now replace the
large-oak middle band in `oak_meadow.scene`. The forest retains **28/90 m**,
the existing one-metre spatial dither transitions, independent fixed-band-1
shadows, source materials, zero tree wind and the original near/far geometry.
The 55 m impostor shortcut is not used. Original imported LOD1 assets remain
available for comparison or rollback.

## Geometry and quality

| Large oak | Original LOD1 triangles | Active canopy triangles | Reduction | Maximum missing coverage |
| --- | ---: | ---: | ---: | ---: |
| 001 | 9,853 | 8,395 | 14.80% | 0.474% |
| 002 | 9,716 | 8,470 | 12.82% | 0.432% |
| 003 | 15,139 | 12,756 | 15.74% | 0.672% |
| 004 | 7,790 | 6,676 | 14.30% | 0.380% |

Together these reduce **42,498 to 36,297 triangles (14.59%)**. Bark/branch
triangles fall from 8,598 to 2,889 (66.4%); leaf triangles fall from 33,900 to
33,408 (1.45%). This primarily removes hidden branch geometry, with modest
interior leaf-layer removal. It is not a large foliage-overdraw reduction.
The native middle-band payload is about **4.68 MiB smaller**.

The generator retains whole position-connected leaf components, tests bark
per triangle, and protects every original per-part axis extremum. It copies
all 66 native vertex attributes without changing UVs, normals, tangents, colors
or pivots. Candidate triangles are exact subsets of the originals. Source
hashes, native loader counts and bitwise attributes/bounds were verified for
all eight parts. Approved meshes are under
`Assets/Models/Woodland/Oaks/Meshes/Canopy/`; their manifest records source and
installed hashes. Four `Large_oak_tree_*_Canopy.prefab` assets reference them.
Only the four middle-prefab references change in the authored scene.

Visibility pruning samples 48 alpha-tested views at 192-square resolution.
A component can be removed only while every training view retains at least
99.9% of its alpha coverage. Separate ground/elevated/zoom screens use twelve
moving azimuths at 384-square resolution. Software screening uses direct native
UVs, nearest sampling, two-sided triangles and fixed diffuse lighting; it does
not approve PBR/shadow behavior by itself.

Actual forest validation covers ground and elevated views, 60/30-degree FOV
and twelve camera positions per view. There are **144 prototype comparison
pairs across DX11/DX12/Vulkan at 960x540**, plus **48 DX12 pairs at 1920x1080**.
An additional **144 pairs** load the actual published asset paths on all three
backends (336 pairs total). The harness requires both nonempty bark/leaf components and verifies
each candidate triangle count before rendering. Elevated shadow caster draws
remain 136. Reviewed paired stills, difference images, motion GIFs and
storyboards retain crown volume, coverage, trunk contact and convincing
shadows. No flat middle-distance replacement is introduced.

At 960x540, the largest full-frame change is 0.281% on DX11, 0.257% on DX12 and
0.276% on Vulkan. At 1080p it reaches 2.824%; an unchanged candidate-repeat
control reaches 0.757%, so those differences cannot all be assigned to geometry.
Separate alpha coverage losses are reported in the table. The three initial
software silhouettes per tree have zero-pixel 95th-percentile contour movement.
Filled-silhouette IoU ranges from 98.43% to 100%; the low ground-view value for
004 reflects hole-connectivity sensitivity and is not a 1.57% alpha-coverage
loss (its measured maximum coverage loss is 0.380%).

## Screen selection and transitions

`CanopyLodStudy.cpp` calls the actual shared C++ selector with the stable union
of original LOD0, LOD1 and impostor bounds, matching the renderer's conservative
policy. Candidate part bounds are unchanged, so the replacement cannot shrink
that envelope or bias selection toward a cheaper band.

The 96 cases cover all four varieties, ground/elevated views, 60/30-degree FOV,
0.5/1/2 render scales and 0/10% hysteresis. Experimental per-asset thresholds
are calibrated to center distances 28/90 m at 1080p, level view and 60-degree
FOV. They are not installed in the forest and are not the rejected illustrative
300/80-pixel policy. Center distances differ from placement-tile distances.

For 001 the reference thresholds are **1485.17 / 315.122 pixels**. Alternating
plus/minus 5 cm at the near boundary causes **119 switches without hysteresis,
zero at 10% hysteresis**. In a 0.25 m outward/return sweep, coarsening occurs
at **30.0 / 98.75 m**, refinement at **26.25 / 82.75 m**. Large-camera-cut,
zoom, conservative-wind and CPU/GPU selector tests also pass. Keep the authored
forest distance policy; accepting geometry does not approve a new pixel policy.

## Measured cost and limitations

Three independent paired processes per backend alternate case order. Each
process measures original and candidate geometry in ground/elevated/zoom views
and the same periodic camera motion. Every case warms for 180 frames and
records 180 frames: **12,960 measured frames** across nine processes. Camera
poses, FOV, resolution, distances and executable identity are compared. GPU
query IDs must be unique, completed workload samples are required, and the
non-tree opaque category must not absorb experimental tree geometry.

All runs use a frozen Release executable/DLL set, 1920x1080, original lighting
and source shading. Full shadow-cascade refresh is forced consistently, with
pipeline-statistics queries enabled. These are instrumented pass costs, not
ordinary gameplay FPS measurements. Asset/shader, candidate and runtime hashes
remain unchanged throughout the complete suite.

Visible tree primitives fall by **3.65-4.18%** across the measured full-forest
views; this includes the other thirteen varieties and near/far bands. GPU timing
is mixed. The median paired process changes in summed bark/leaf/shadow cost are:

| Backend | Ground | Elevated | Zoom | Motion |
| --- | ---: | ---: | ---: | ---: |
| DX11 | +1.934 ms | +2.015 ms | -0.076 ms | +0.329 ms |
| DX12 | -0.135 ms | -0.731 ms | -0.873 ms | -0.310 ms |
| Vulkan | -0.417 ms | -0.605 ms | -0.429 ms | -0.386 ms |

DX11 includes slower cases and a large outlier; do not claim a universal GPU
speedup or higher sustained FPS. The accepted result is lower geometry/memory
work with retained appearance, with lower instrumented cost in the DX12/Vulkan
paired medians. Overall FPS, foliage overdraw and dedicated shadow proxies remain
separate TODOs.

## Reproduction and evidence

```powershell
cmake --build build --config Release --target ForestPerformanceAudit OakBackendCapture CanopyLodStudy MeshLodSelectionTests ScreenLodBackendTests
python Scripts/Assets/build_canopy_lod.py --output .local/canopy-lod-full --coverage-constrained --training-size 192
python Scripts/run_canopy_study.py .local/canopy-lod-full
python Scripts/evaluate_canopy_lod.py .local/canopy-lod-full --capture-executable .local/canopy-lod-full/validation/runtime/Release/TerrainPrefabAndOakTests.exe
python Scripts/Assets/install_canopy_lod.py .local/canopy-lod-full
ctest --test-dir build -C Release -R '^(MeshLodSelection|ScreenMeshLod_.*)$' --output-on-failure
python .local/tests/test_canopy_lod.py
python .local/tests/test_canopy_study.py
```

The installer requires all four screened candidates, completed matched timing
provenance and three-backend quality evidence. It verifies source/candidate
hashes, publishes eight meshes/four prefabs and changes only middle references.
The forest builder selects approved canopies when regenerating placements and
filters species from LOD0/1/2 sources so canopy/impostor prefabs do not become
extra varieties. In-memory regeneration verifies 2,048 placements, seventeen
varieties, four approved middle bands and preserved 28/90 m settings.

- `.local/canopy-lod-full/report.json`: screening, source hashes and part counts.
- `.local/canopy-lod-full/evaluation.json`: 192 renderer pairs, 96 selector cases,
  silhouette metrics, image noise controls and motion residuals.
- `.local/canopy-lod-full/validation/timing-summary.json`: paired process results.
- `.local/canopy-lod-full/validation/timing-provenance.json`: complete unchanged
  asset/shader/candidate/runtime fingerprints for the matched suite.
- `.local/canopy-lod-installed/`: published-path backend comparisons and provenance.
- `Engine/Core/Assets/Models/Woodland/Oaks/Meshes/Canopy/manifest.json`: installed
  geometry identities and original source hashes.

Seven Python measurement/provenance tests, the numerical selector regression and
DX11/DX12/Vulkan selector device tests pass. Release capture/study targets build.
DX12 quality captures complete with validation enabled.

Earlier ranked candidates saved 46.7-48.1% but lost 8.9-13.8% coverage and were
rejected. Authored spatial LOD2 saved 34.6-36.3% but lost 12.4-14.1% coverage and
changed crown density/shading, so it was not selected. These were spatial
alternatives, not the rejected 55 m impostor shortcut. Early flipped-V screening,
XML namespace aliases and unclassified experimental timing runs were corrected
and superseded. Final exports use the literal native `components:` prefix;
final timing explicitly classifies forest-layer meshes as trees. The invalid
category timings are archived under `validation-unclassified` and excluded.
