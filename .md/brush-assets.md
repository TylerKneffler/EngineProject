# Brush assets (initial implementation)

Brush presets are XML files with a `.brush` extension. Create one with **New brush**
in the Assets browser, edit it in the asset inspector, or create/save/duplicate one
from the Paint section of the main editor toolbar. Its Cursor/Brush control
switches between Cursor, Brush, Eraser, and Bucket, and **Paint...** opens
the detailed paint options. Eraser removes the selected bone's influence and
redistributes it among other unlocked influences. Bucket fills adjacent faces
whose starting average weight is within the chosen tolerance; it makes one
undoable edit per click. Bucket does not yet support mirrored bones. The toolbar stores new presets under
`Assets/Brushes`; the toolbar lists presets anywhere in Assets. A preset stores
brush geometry and dynamics. The selected bone, target weight, operation, and
mirror bone remain per stroke settings.

Weight paint currently applies analytic round and square tips and image tips, size, hardness,
falloff, strength, opacity, flow, spacing, rotation for square tips, scatter,
and Windows pen pressure for size and strength. The existing mesh history
provides weight stroke undo/redo and the mesh editor's Save weights action
persists painted weights. Image tips use alpha, or inverse luminance when
the image is fully opaque.

PNG assets opened in the Texture Editor also have a Paint section in the editor
header. Cursor leaves the image unchanged; Brush draws color, Eraser reduces
alpha, Bucket fills connected pixels within the color tolerance, Blur blends
local colors, Smear drags the sampled color, and Average paints the local mean.
Mirror X and Mirror Y repeat non-bucket strokes across the image. Alpha Lock
preserves existing transparency while painting; erasure is blocked while it is
enabled. Bucket reports that mirrored fill is unsupported.
The brush
outline shows the current size. Each completed stroke saves the PNG and enters
the document undo/redo history. PNG paint loads the same `.brush` assets as
weight paint and can create, save, or duplicate them in project Assets. Round
tips support hard, linear, and smooth falloff; a preset
can use an image tip in Brush, Eraser, Smear, and Average. Transparent images
use alpha as the mask; fully opaque images use inverse luminance (black paints,
white does not). Enter the
project asset path in the brush inspector or PNG paint toolbar, or use
**Import image tip...** in the PNG paint toolbar to copy a source image into
`Assets/Brushes/Tips`. Blur reports
that it cannot use image tips. A preset using pressure, square shape,
rotation or scatter reports those unsupported settings and blocks PNG brush
strokes.

Mesh mode has **Paint Mesh** in the editor Paint header. The Paint options
select either vertex colors or a PNG texture assigned to a material slot.
Texture strokes use the slot's UV 0 or UV 1 setting and repeat UVs outside the
unit square. Brush and Eraser draw on the visible mesh surface; Bucket fills a
connected region of the texture. Texture strokes save the PNG and have their
own undo/redo history. Vertex strokes enter mesh history and **Save Mesh**
persists them. The UV unwrap tool creates packed islands with automatic angle
seams or selected edge seams; the manual UV editor remains available for both
UV sets. Mesh paint uses the same `.brush` assets, including image tips.
Mirror local X repeats non-bucket strokes. Vertex selection masking and PNG
Alpha Lock are available for their respective targets; bucket reports these
unsupported controls instead of applying them partially. Windows pen pressure
changes size and strength according to the preset responses. Asset thumbnails
and paint toolbars preview analytic and imported image tips.

**Import .abr pack...** in the PNG Paint toolbar imports sampled 8-bit tips
from Photoshop ABR versions 1, 2, 6, and 10. It creates PNG tips in
`Assets/Brushes/Tips`, `.brush` presets in `Assets/Brushes`, and keeps the
original pack in `Assets/Brushes/Sources`. Version 1/2 spacing is mapped;
version 6/10 descriptor settings, Photoshop dynamics and blend modes are
reported as unsupported. Computed brushes and unsupported depths are skipped
with a count in the import report. Review imported presets before painting.

## Blender conversion path

Blender stores brush assets as Brush data blocks in `.blend` and `.asset.blend`
libraries. In Blender, open the library file, select the brush from the asset
shelf, and copy its Size, Strength, falloff shape, Spacing, Rotation, and
Scatter into a new `.brush` asset in this editor. Use a separate preset for
each Blender brush. Keep the original `.blend` file in project Assets beside
the converted presets so the source remains available. If the Blender brush
uses an image texture, export that image as a PNG into Assets and keep it with
the source. Blender-specific blend
modes, jitter curves, texture mapping, rake, and pressure curves currently have
no equivalent. Record those settings with the source file before conversion.

This path is manual. A `.blend` file is not parsed by the engine.

References: [Blender weight paint brushes](https://docs.blender.org/manual/en/latest/sculpt_paint/weight_paint/brushes.html),
[Blender brush asset management](https://docs.blender.org/manual/en/5.0/sculpt_paint/brush/brush_management.html),
and [Photoshop brush imports](https://helpx.adobe.com/photoshop/desktop/apply-painting-techniques/brushes-presets/import-brushes-brush-packs.html).
