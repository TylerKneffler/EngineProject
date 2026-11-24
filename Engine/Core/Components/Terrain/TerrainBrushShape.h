#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Components
{
struct TerrainStamp;

// A shaped terrain brush made from a terrain stamp (.stamp, a scanned rock
// shape): the stamp's finest mesh seen from above (stamp-space Y up), as a
// square grid of heights and a coverage mask. Brushes use it in place of the
// round falloff: the mask is the brush's outline, and the heights are what
// the Stamp brush raises (or carves) the terrain by, so a stamp edits the
// terrain's own height data instead of being drawn on top of it.
//
// Shape space: u, v in [-1, 1] across the square footprint, u along the
// stamp's X and v along its Z. A brush of radius r maps that square to
// 2r x 2r metres around its centre, turned by its yaw; heights scale with it
// (r / halfExtent), so the shape keeps its proportions.
struct TerrainBrushShape
{
    int resolution = 0;            // samples per side
    float halfExtent = 1.f;        // stamp-space metres from the centre to the square's edge
    float maxHeight = 0.f;         // stamp-space metres, the tallest sample
    std::vector<float> heights;    // resolution^2, rows along v, metres above the shape's base
    std::vector<float> mask;       // resolution^2, 0..1, feathered at the outline
    // The way the stamp's face looks, in shape space (u, v), unit length: the
    // scan's outward direction seen from above. Its back (-facing) is where
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

    // Top-down raster of the stamp's finest level. Null when the stamp has
    // no triangles or no extent.
    static std::shared_ptr<const TerrainBrushShape> FromStamp(const TerrainStamp& stamp,
        int resolution = 128);
    // Loads a .stamp and converts it, once per path (thread-safe); null when
    // the file is missing or malformed.
    static std::shared_ptr<const TerrainBrushShape> Load(const std::string& stampPath);
};
}
