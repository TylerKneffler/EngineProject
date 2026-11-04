# Shadow cascade stability during camera rotation

The supplied screenshots show nearby ground-shadow patterns changing between
small look-angle changes at the same position. The previous upload-order fix
addressed stale DX11 records, but did not establish stability of the shadow
projection itself.

An additional regression reproduced changing cascade extents under an unchanged
camera projection: radius 95.75 became 95.8125 after a small camera transform
change. The old fitter inverted the combined projection/view matrix in float,
unprojected the far plane, and measured a bounding radius in world space. Camera
roundoff crossed the radius quantization boundary, changing world units per
shadow texel. Texel-center snapping cannot stabilize a grid whose scale changes.
That mechanism can make a static shadow pattern crawl or flicker while looking.

Cascade fitting now unprojects camera-local rays in double precision, using an
interior clip depth instead of the numerically sensitive far-plane endpoint.
It extends those rays to exact cascade near/far view depths, including transition
overlap. The radius is measured and quantized in camera-local space before
transforming receiver corners and center into world space. Thus FOV, aspect and
split depths determine its size; ordinary camera rotation/translation does not.
Existing light-space center snapping, caster culling and cascade blending remain.
The helper is shared by main and separately rendered portal shadow cascades.

## Validation

The regression extends the actual DX11 shadow-rendering/readback fixture with
radius checks and a world shadow texel-grid phase check. It exercises fixed-position
yaw/pitch in both directions and a translation, with one/four cascades and both
ordinary/native packets (16 combinations). It checks current GPU record contents
and verifies a fixed world point's shadow texel coordinates shift only by whole
texels. The radius assertion fails before the fitting change and passes afterward.

Editor and GrassPerformanceTests Debug builds passed. All five selected CTest
suites passed in 46.89 seconds: PerformanceEvaluation, moving shadow upload/grid
stability, and existing DX11/DX12/Vulkan shadow pixel regressions. These tests
establish the corrected numerical behavior; visual confirmation in the user's
specific grass scene remains open. The original screenshots cannot establish
which of the two confirmed defects accounts for every observed frame difference.

Recordings now include `shadow_state_cascade_radii` alongside the prior cascade
splits, enable/selected-light flags and atlas allocation state. A 1920 x 1080
full-editor grass smoke capture is retained with the verification outputs.

Artifacts: `build/Debug/shadow-rotation-before.log` contains the failing radius
comparison. `build/Debug/shadow-rotation-verification-20261103/` contains `ctest.log`,
`editor-1080p.jsonl` and the editor evaluation JSON/Markdown. Build logs are
`build/Debug/shadow-rotation-fix-build.log`, `shadow-rotation-tests-build.log` and
`shadow-rotation-editor-build.log`. User layout and asset-location files were
saved in memory and restored after the editor smoke capture.
