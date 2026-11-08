# Texture mip selection and filtering audit

Completed 2026-11-07, including continuous-motion and rendered-shadow checks.
Runtime sampling and alpha defaults are unchanged. The audit
found complete generated material mip chains and shared automatic LOD selection,
but anisotropic filtering is inconsistent across backends. No alpha-test
replacement is approved by these results.

| Material path | DX11 | DX12 | Vulkan |
|---|---|---|---|
| Uploaded mip chain | All supplied subresources | All supplied subresources | All supplied image levels |
| Shader-visible levels | Default SRV exposes chain | SRV exposes supplied count | Image view exposes supplied count |
| Minification / mip interpolation | Anisotropic, 8x | Linear / linear | Linear / linear |
| Sampler LOD range | 0 to float maximum | 0 to float maximum | 0 to unclamped maximum |
| Anisotropy | Enabled | Disabled | Disabled; device feature also not enabled |

These are material samplers, not postprocess, UI, shadow-map comparison, or
single-level render-target samplers. Relevant definitions are
`DX11GraphicsContext.cpp`, `DX12GraphicsProvider.cpp`,
`VulkanGraphicsTexture.cpp`, and `VulkanRenderCore::CreateDevice`.

## Mip generation and selection

`Texture.cpp::FullMipCount` and `GenerateMipChain` halve each dimension with a
minimum of one, continuing through 1x1, including rectangular textures. Ordinary
loaded textures generate mips by default. RGB filtering for sRGB textures occurs
in linear space; alpha is averaged linearly. HDR uses float RGBA. Backend texture
factories upload the supplied levels; they do not generate missing levels.
Detached CPU decodes explicitly disable mip generation. Preview/render targets
and 1x1 fallback textures may intentionally have only one level.

KTX2 currently reads level zero and regenerates the chain; authored lower mips
and authored alpha-coverage corrections are not retained. The CPU 2x2 reducer
also drops the trailing row/column when an odd dimension greater than one is
halved. Completeness therefore does not establish optimal filtering quality.

The shared `Object.hlsl` material and terrain paths use `SampleBias`, preserving
implicit screen-space derivative selection and adding an optional bias. A zero
bias still selects coarser mips automatically as projected texel size shrinks.
Environment roughness sampling and parallax height traversal deliberately use
`SampleLevel`; those are separate algorithms, not evidence that ordinary color
textures are pinned to an explicit mip. Derivative instructions inside divergent
shader branches remain a reason to inspect real motion, beyond this atlas test.

`TextureLodSettings` adds `smoothstep(near, far, camera distance) * mipBias`.
Default authored values are disabled, 20/80 distance, bias 4; normalization bounds
bias to 0..8. Scene settings and per-material overrides are resolved through
`GraphicsQualitySettings::ResolveTexture`. Texture and shading simplification
remain independently selectable. The bundled oak meadow enables 12/48, bias 4.

## Alpha coverage and temporal risks

`SampleObjectBaseColor`, `PSFoliageMain`, and `PSShadowMain` retain alpha cutoff
testing. Alpha has its own distance bias, default zero, independent of RGB bias.
Zero alpha bias means automatic derivative-selected alpha mips, not mip zero.
Ordinary color and shadow shaders use the same alpha-bias formula, cutoff, and
material alpha multipliers, but different camera/light derivatives can still
select different mip footprints. Fixed prefab shadow proxies explicitly zero
both texture and alpha LOD parameters, preserving their unbiased cutouts
independently of camera distance. Color and shadow silhouettes are not
guaranteed identical.

`disableAlphaAtFar` is an existing explicit option, default false. It removes
the mask flag at the far threshold in ordinary color and shadow paths and can
abruptly expose whole leaf cards. Fixed prefab shadow proxies retain their
cutouts. No enabling of this option was found in bundled Woodland materials,
prefabs, or terrain scenes. Keep it false for foliage pending coverage evidence.

Mip generation does not preserve coverage at a material's alpha cutoff. Thin
leaves and twigs can shrink or disappear at coarse levels; a zero distance alpha
bias does not eliminate that risk. Oblique captures visibly resolve fewer thin
features. Anisotropy can improve oblique detail, but enabling it on DX12/Vulkan
would also change mask sampling and needs coverage comparisons before adoption.
No default filtering change or alpha replacement is made in this audit.

## Reproducible backend checks

Build `TextureSamplingBackendTests` in Debug, then run from the repository root:

```powershell
cmake --build build --config Debug --target TextureSamplingBackendTests --parallel 4
python Scripts/Assets/validate_texture_sampling.py build/Debug/TextureSamplingBackendTests.exe
```

The optional CMake target is guarded by source existence and uses
`.local/tests/TextureSamplingBackendTests.cpp`. The runner saves JSON, PGM masks,
and RGB PPM captures under `.local/texture-sampling`. `--reuse` reanalyzes existing
captures. The harness renders the real oak leaf atlas at cutoff .32, both the
built-in material shader and `Assets/Shaders/Foliage.hlsl`, at 0, 60, and 80
degrees. It samples 2.9/3/3.1, 5.4/5.5/5.6, and 7.9/8/8.1 camera distances around
a test-only 3..8 bias transition. Each position compares disabled bias with bias
4, with shading simplification off, alpha bias zero, and cutouts retained.

Debug build and all three backend runs passed: **54 identical mask pairs per
backend, 162 total**, comparing individual pixels as well as coverage counts.
Both shader paths produced nonempty captures and no reported shader errors.
DX12 and Vulkan coverage counts matched across all 108 samples. For the foliage
shader at distance 8, coverage at 0/60/80 degrees was:

| Backend | 0 degrees | 60 degrees | 80 degrees |
|---|---:|---:|---:|
| DX11 | 3525 | 1823 | 664 |
| DX12 | 3572 | 1887 | 604 |
| Vulkan | 3572 | 1887 | 604 |

The sampler mismatch is consistent with cross-backend coverage differences;
this does not isolate all rasterizer differences. The initial static atlas
comparison has shadow casting off. The following extended tests cover motion
and rendered shadows separately.

## Continuous motion and rendered shadows

```powershell
python Scripts/Assets/validate_texture_sampling.py build/Debug/TextureSamplingBackendTests.exe --extended --output .local/texture-sampling-final
```

For each material shader, extended mode advances 180 simulation frames at 60 Hz:
a two-second continuous camera sweep out and back over distances 2.8..8.2 and
angles 60..80 degrees, followed by one second at a fixed 80-degree view, distance
5.5. Diagnostic wind strength is .5. Every simulation frame captures color bias
off/on at identical time and camera pose and asserts pixel-identical alpha
masks. The discontinuity between camera and fixed-camera phases is excluded
from temporal metrics. Normal scene assets and their wind settings are unchanged.

The motion report separates mask turnover from RGB change on pixels covered in
both consecutive frames. These diagnostics include legitimate wind and camera
motion; they are **not a calibrated shimmer score**. Identical paired masks
directly test whether color bias adds cutout flicker through the mip transition.
RGB differences measure temporal texture variation independently of entering
and leaving silhouette pixels. Representative RGB frames are retained every
six simulation frames and at each phase endpoint.
Foreground masks use any unlit RGB channel above 5/255, matching the static
atlas audit. Shadow masks use summed RGB receiver darkening above 24/255 against
the matched shadow-off capture; opaque controls must expose over ten additional
hole pixels. These thresholds are capture diagnostics, not material cutoffs.

Rendered-shadow checks use the same real leaf atlas, a lit parallel receiver,
both shaders, distances 3/5.5/8 and camera angles 0/60/80 degrees. A 2048-square
single-cascade map with filter radius zero isolates the cutout footprint. The
caster uses authored independent foliage-card sidedness and 28/90 geometry
thresholds, outside this test's range. The receiver color pass hides the card
after the complete shadow pass, so card color cannot contaminate shadow masks.
Matched shadow-off, masked-caster and opaque-caster captures prove that actual
receiver darkening and cutout holes are visible. The harness asserts identical
shadow masks with color bias off/on. An additional capture after advancing wind
one second asserts identical receiver RGB under the existing static-caster
shadow policy. This policy intentionally does not animate shadow silhouettes.

The direct-context harness waits for GPU completion before mutating shared
buffers and, critically, after submission before mapping capture memory.
DX12/Vulkan readback mapping alone does not synchronize the pending GPU copy.
Earlier exploratory captures lacked this second wait and produced inconsistent
masks and apparently missing shadows; those runs are rejected. Adding extra
warm frames did not establish correctness and is not a substitute for the
completion wait. The final static and extended suites are rerun with synchronized
readback. These waits are test infrastructure, not production performance
measurements. Alpha tests, sampler defaults and production rendering code are
unchanged.

The synchronized static suite passes all 162 paired masks. The synchronized
extended suite passes **360 continuous-motion pairs and 18 rendered-shadow pairs
per backend**: 1,080 motion pairs and 54 shadow pairs total. Every shadow view
also passes the one-second wind comparison, with zero receiver RGB changes
(108 comparisons total). Opaque controls expose at least 1,263 extra hole pixels
on DX11, 1,235 on DX12 and 1,231 on Vulkan. The rendered-shadow controls were also
visually inspected.

For the foliage shader, mean absolute RGB change per common covered channel,
in captured 8-bit values, is:

| Backend | Camera + wind, bias off | Camera + wind, bias on | Fixed-camera wind, bias off | Fixed-camera wind, bias on |
|---|---:|---:|---:|---:|
| DX11 | 6.107 | 5.223 | 1.191 | 0.373 |
| DX12 | 3.193 | 2.745 | 0.403 | 0.114 |
| Vulkan | 3.193 | 2.743 | 0.403 | 0.114 |

Compare off/on within each backend; the capture paths and rasterization differ
between APIs. The existing bias reduces measured temporal texture variation in
these sequences. Alpha-mask turnover remains identical frame by frame, including
the near/far mip-bias transitions and fixed oblique wind view. This confirms no
added cutout flicker from color bias in the audited sequences. It does not prove
the absence of all shimmer in every scene, nor authorize replacing cutouts,
changing anisotropy, or changing authored alpha bias. Tiny foliage features can
still alias or thin through their ordinary averaged alpha mips.

Final evidence is `.local/texture-sampling/summary.json`,
`.local/texture-sampling-final/summary.json`, backend logs and raw captures.
Build output is `.local/texture-sampling-final-build.log`. Normalized shadow
controls are summarized in `.local/texture-sampling-final/dx12-shadow-controls.png`
(shadow off, masked, opaque from left to right).

## Diagonal-light coverage finding

The additional synchronized 45-degree sun stress case fails on **all three
backends** at camera angle 80 degrees, receiver distance 8, with color bias
disabled. Both masked and opaque controls produce zero shadow pixels, despite
one submitted caster, a selected light and a 20-unit shadow band. Nearer and
less oblique views show shadows and cutout holes. This is a measured directional
shadow-coverage failure independent of the color-bias comparison. Its underlying
cause is not isolated by the texture audit. A blanket shadow-coverage signoff
across light angles is therefore unsupported; cutouts remain required.

Reproduce from the repository root (substitute `dx12` or `vulkan` as needed):

```powershell
$env:ENGINE_TEXTURE_DIAGONAL_SHADOW = '1'
build/Debug/TextureSamplingBackendTests.exe dx11 .local/texture-sampling-diagonal/dx11 --shadows
Remove-Item Env:ENGINE_TEXTURE_DIAGONAL_SHADOW
```

The shadow-presence assertion exits with code 1 and saves shadow-off, masked and
opaque diagnostic RGB captures at the failing view. Evidence is in
`.local/texture-sampling-diagonal/` and
`.local/texture-sampling-diagonal-{dx11,dx12,vulkan}.log`. These synchronized
stress failures are distinct from the discarded asynchronous-readback results.
The audit is complete with this finding recorded; no replacement or filtering
change has been accepted on the strength of the narrower passing controls.

## Sampling quality versus resident memory

Distance mip bias, anisotropy, and derivative selection change texture reads,
cache behavior, detail, and possibly bandwidth. They do not remove allocated
top levels, reduce resident texture bytes, reduce material slots/draws, or prove
lower shader cost. A complete 2D RGBA chain adds roughly one third to base-level
payload for large square power-of-two textures.

`ENGINE_TEXTURE_MAX_DIMENSION` is a separate startup policy in
`Texture::PrepareImmediate`: it omits oversized top levels before creating a
smaller GPU resource, so it can reduce allocation. HDR retains its existing
2048 ceiling. Source metadata remains full resolution. This is a fixed resident
cap, not view-dependent streaming; it has no mip-demand feedback, eviction,
prefetch, or per-frame memory budget. Resource allocation, process usage, upload
staging, and CPU decode memory must be reported separately. See
`audit_texture_residency.py` and `validate_texture_residency.py` for that distinct
audit; a zero DX11 per-resource allocation field means unavailable, not free.
