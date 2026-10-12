# Engine roadmap

Last audited against the codebase: 2026-10-12.

Status convention:

- `[x]` Complete, implemented and present in the repository.
- `[ ]` Incomplete, not implemented or not complete enough to meet its acceptance criteria.

## Next priorities

- [ ] Add localized stretch and compression for skinned mesh surfaces, driven by bone motion and mapped collision impulses. Preserve skin weights, cap deformation, and restore the authored shape after impacts.
- [ ] Build reusable brush assets and richer surface/weight painting controls in the editor.
  - [x] Add a separate Paint section to the main editor toolbar with Cursor, Brush, Eraser, and Bucket selection; Cursor can return to viewport selection.
  - [x] Add project `.brush` assets that can be created in Assets, edited in the asset inspector, and loaded, saved, or duplicated from the weight paint toolbar. Keep preset settings separate from bone, paint weight, and mirror target.
  - [x] Apply weight brush size, hardness/falloff, strength, opacity, flow, spacing, square rotation, and scatter. Add eraser and connected-face bucket fill with tolerance and opacity, preserving locked-bone and normalization rules.
  - [x] Test preset save/reload and invalid settings, connected bucket regions, and locked-bone erasure. The Debug Editor build and focused tests pass.
  - [ ] Add a paint mode for both mesh surfaces and image assets (starting with `.png`): paint vertex colors or texture targets on meshes, and open image files as editable 2D canvases for drawing. Provide visible stroke preview, undo/redo, and save/reload for both targets.
    - [x] Open PNG assets as editable canvases with Cursor, Brush, Eraser, and Bucket in the editor Paint header; show a brush outline, save strokes to the PNG, and reload pixels during undo/redo.
    - [x] Load, create, save, and duplicate shared `.brush` assets from the PNG paint toolbar.
    - [x] Test deterministic PNG stamping, erasure, connected fill, and encoded PNG reload.
    - [x] Paint vertex colors directly on a mesh with Brush, Eraser, and connected Bucket tools, a viewport brush outline, shared mesh undo/redo, and Save Mesh persistence.
    - [x] Verify vertex color snapshot undo/redo and native mesh save/reload with a focused test.
    - [ ] Paint a mesh texture target directly on the surface, using its selected UV set and respecting texture wrap at UV boundaries.
    - [ ] Add mesh UV unwrap with seams and island packing; keep the existing manual UV editor for adjustments.
  - [ ] Share the preset library across weight paint, mesh paint, and image asset paint, with graphical brush previews in Assets and the paint toolbar.
    - [x] Share `.brush` presets across weight, vertex color, and PNG paint; show graphical analytic tip previews in Assets and their paint toolbars.
    - [ ] Preview imported image tips and use the same preset in mesh texture painting.
  - [ ] Apply image tips and pen pressure, and add smear and average behaviors plus further symmetry/masking controls. Bucket mirroring is still unsupported.
    - [x] Add Blur, Smear, and Average to PNG image painting.
    - [x] Apply image tip alpha or opaque luminance to PNG Brush, Eraser, Smear, and Average strokes; preview tip images in the brush inspector and PNG toolbar.
    - [x] Add PNG X/Y mirror placement and Alpha Lock for non-bucket strokes; report unsupported bucket mirroring.
    - [x] Map Windows pen pressure to size and strength responses in PNG, vertex color, and weight paint.
    - [ ] Apply image tips to weight and vertex painting; add mesh paint symmetry/masking.
  - [ ] Import image brush tips and Photoshop `.abr` brush packs as project brush assets. Investigate Blender `.blend` brush assets and provide a direct importer or documented Blender export/conversion path. Preview imported brushes, map supported settings, and report unsupported settings without silently changing the stroke. Keep imported source files and reusable presets in the project Assets workflow.
    - [x] Import an image tip from the PNG paint toolbar into `Assets/Brushes/Tips`, preview it, and apply its alpha to PNG paint strokes.
    - [ ] Import `.abr` packs and map their supported settings with explicit unsupported-setting reports.
  - [ ] Validate deterministic strokes, painted-result save/reload, undo/redo, and imported preset behavior with focused tests. Use [Blender weight paint brushes](https://docs.blender.org/manual/en/latest/sculpt_paint/weight_paint/brushes.html), [Blender brush asset management](https://docs.blender.org/manual/en/5.0/sculpt_paint/brush/brush_management.html), and [Photoshop brush imports](https://helpx.adobe.com/photoshop/desktop/apply-painting-techniques/brushes-presets/import-brushes-brush-packs.html) as feature and format references.

## Deferred feature gates

- [ ]: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites are complete.
- [ ]: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
