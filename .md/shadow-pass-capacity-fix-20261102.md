# Shadow upload crash: sparse persistent object slots

The reported Editor access violation occurred while a cascade worker copied an
object record into the mapped shadow upload buffer. Pass buffers were resized
using the compact active draw count while object and shadow records were indexed
by persistent object slots. Visibility/LOD changes can leave a high slot active
with only a handful of draws. Selecting an older, smaller frame/view allocation
then allowed out-of-range camera or shadow writes.

## Fix

- Compute the required object-slot range from the highest slot in the actual
  frame item list (including an owned native render packet), plus one.
- Grow each selected frame/view upload allocation to that range, including the
  initial selected pass. Do not use active draw count as its capacity requirement.
- Validate mapped camera/shadow allocations and their declared byte sizes on the
  owner before record-packing or cascade jobs start. Invalid slot ranges fail
  with a diagnostic instead of an unchecked worker copy.
- Keep cascade parallelism and persistent slot identity. The correction applies
  to all scene meshes and all backends.

## Reproduction and regression

The regression first warms small frame/view allocations, assigns more than 1,024
persistent mesh slots, hides all but the last new mesh, and renders that high-ID
caster through four shadow cascades and camera views. It repeats reuse across
frames, direct/native-packet rendering and serial/parallel cascade modes, checking
that the remaining mesh is visible and submits a shadow caster. DX12 uses a
fresh frame to bind color attachments after the depth-only pass; DX11/Vulkan
exercise the shadow and camera passes together. This avoids testing attachment
restoration or flip-buffer capture behavior as part of a capacity regression.

With the pre-fix count-based sizing and the new owner-side guard, DX11 reproduces
`Render pass upload buffers do not cover persistent object slots` before any
unsafe copy. Log: `.local/shadow-capacity-repro.log`.

Registered CTests: `ShadowSparseSlotsDX11`, `ShadowSparseSlotsDX12`, and
`ShadowSparseSlotsVulkan` when Vulkan is enabled. These extend the existing grass
instancing/fade/shadow image comparisons with the sparse-capacity regression.

Build and verification logs use `.local/shadow-capacity-*`. Editor, Game and the
focused helpers were rebuilt. All six focused CTests pass, including sparse-slot
DX11/DX12/Vulkan rendering, GrassPerformance, RenderSpatialIndex and
DrawBatchPreparation. Eighteen evaluator tests pass. User settings
and the preexisting dirty asset-location cache were preserved byte-for-byte.
