# Camera-sorted shadow upload correction

The supplied `performance-20261103-184046-7.jsonl` contains 852 Play frame records.
Every frame reports 12 shadow draws, 79 active grass tiles, zero grass LOD
switches and zero LOD-transition tiles. It uses one Game view at 1814 x 1069.
Those counters do not indicate whole shadow draws switching on and off, but
they do not measure per-instance GPU visibility or establish pixel stability.
This older capture lacks explicit cascade/atlas enable and allocation state.

Found and fixed a concrete stale-data defect in shadow submission. Prepared
opaque draws are sorted by distance to the camera, independently of their
persistent GPU record slots. Shadow dirty-range accumulation used the last
visited slot as the upper bound instead of the highest changed slot. DX11
structured upload buffers use range-based UpdateSubresource, so changed records
beyond that bound retained previous camera/cascade matrices and material data.
Moving or turning the camera can alter sorting and expose those stale records
even when caster and draw counts stay constant.

The fix takes the maximum changed record index in each cascade before merging
the upload ranges. It preserves partial uploads and does not force all shadow
data to upload every frame. DX12/Vulkan coherent mapped resources were not
dependent on this DX11 upload copy, but their pixel regressions were rerun.

## Reproduction and verification

Added `--shadow-record-upload-order` to GrassPerformanceTests and the optional
`ShadowRecordUploadOrderDX11` CTest. The fixture creates three casters whose
camera-distance order differs from persistent slot order. It moves and rotates
the camera, exercises one and four cascades and ordinary/native packet paths,
and reads actual GPU shadow buffers back. It compares every submitted record
with its current CPU source bytes, rather than relying on draw counts alone.

Before the fix, the regression failed on the first one-cascade frame with
`drawn GPU shadow record differs from its current CPU data after out-of-order upload`.
After the fix, it passes all 16 camera/cascade/packet combinations.

The Editor and GrassPerformanceTests Debug builds passed. All five selected
CTest suites passed in 31.76 seconds: the new upload-order test, existing
DX11/DX12/Vulkan shadow pixel regressions and PerformanceEvaluation. The new
readback fixture checks upload correctness, not the complete rendered image of
the user's moving grass scene; further visual confirmation remains open.

Recordings now include shadow enabled/selected-light flags, cascade count and
splits, atlas resolution/allocation generation and caster count. These supplement
existing draw and timing fields for distinguishing state toggles from stale
records in a follow-up interactive capture.

Artifacts: `build/Debug/shadow-upload-before.log` preserves the failing readback;
`build/Debug/shadow-upload-fix-build.log` records the build;
`build/Debug/shadow-upload-verification-20261103/ctest.log` contains passing tests.
The same directory contains a 1920 x 1080 full-editor smoke capture and evaluation.
User layout and asset-location files were preserved in memory and restored.
The 180 measured smoke frames used exactly 1920 x 1080 and averaged 164.77 FPS
(6.069 ms interval, P95 6.817 ms). Shadow enable and selected-light flags stayed
on, with four cascades, a 2048 atlas and allocation generation 1 throughout.
This is a smoke result rather than a controlled performance comparison.
