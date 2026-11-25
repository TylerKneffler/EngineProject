#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Brushes { struct BrushPreset; }

namespace Engine::Components
{
// A shaped terrain brush made from a brush asset (.brush, Core/Brushes): its
// tip seen from above as a square grid of a coverage mask and, for a brush
// with a relief, of heights. Brushes use it in place of the round falloff:
// the mask is the brush's outline, and the heights are what Raise, Lower and
// the Stamp tool build the terrain up by (scanned rock shapes, once terrain
// stamps: tools/stamp_brush.py turns those into brushes), so a brush edits
// the terrain's own height data instead of being drawn on top of it.
//
// Shape space: u, v in [-1, 1] across the square footprint (u along the
// tip's columns, v along its rows). A brush of radius r maps that square to
// 2r x 2r metres around its centre, turned by its yaw; heights scale with it
// (r / halfExtent), so the shape keeps its proportions.
struct TerrainBrushShape
{
    int resolution = 0;            // samples per side
    float halfExtent = 1.f;        // shape-space units from the centre to the square's edge
    float maxHeight = 0.f;         // shape-space units, the tallest sample
    std::vector<float> heights;    // resolution^2, rows along v, above the shape's base (may be all 0)
    std::vector<float> mask;       // resolution^2, 0..1, feathered at the outline
    // The way the shape's face looks, in shape space (u, v), unit length (a
    // relief's outward direction seen from above). Its back (-facing) is where
    // the terrain it is pressed against lies (BrushOptions::backed).
    glm::vec2 facing{0.f, 1.f};
    // The outline as a star polygon around the centre (shape space), for the
    // editor's brush preview.
    std::vector<glm::vec2> outline;

    // Bilinear samples; 0 outside the square.
    float SampleHeight(const glm::vec2& uv) const;
    float SampleMask(const glm::vec2& uv) const;

    // Shape-space position of an offset (dx, dz) from the brush centre, and
    // back, for a brush of `radius` turned by `yawRadians` (about +Y).
    static glm::vec2 ToShape(float dx, float dz, float radius, float yawRadians);
    static glm::vec2 FromShape(const glm::vec2& uv, float radius, float yawRadians);

    // The analytic falloff brushes share (BrushPreset falloff: 0 hard,
    // 1 linear, 2 smooth) at a distance 0..1 from the centre (1 = the edge),
    // full strength out to `hardness`.
    static float Falloff(float distance, float hardness, int falloff);
    // Fills `outline` from the mask (64 directions).
    void BuildOutline();

    // The shape of a brush: its image tip (alpha, or inverted luminance for
    // an opaque image), else its square tip with its falloff, else (a round
    // tip) the round falloff baked when it has a relief; plus the relief's
    // heights. Null for a plain round brush (the terrain's own falloff draws
    // it) or when an image cannot be read. Asset paths resolve through
    // Mesh::ResolveFilePath.
    static std::shared_ptr<const TerrainBrushShape> FromPreset(const Engine::Brushes::BrushPreset& preset,
        int resolution = 128);
    // Loads a .brush and its images, once per path while the files are
    // unchanged (thread-safe). Null as FromPreset, or when the file is
    // missing or malformed.
    static std::shared_ptr<const TerrainBrushShape> Load(const std::string& brushPath);
};
}
