#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>
#include <glm/glm.hpp>

namespace Engine::Graphics
{
class IGraphicsBuffer;
class IGraphicsContext;
class IGraphicsProvider;
class IGraphicsTexture;
class IPipelineState;
}

namespace Engine::Rendering
{
struct PreparedDraw;

// Terrain runtime virtual texture (.md/todo.md Step 5, "Terrain runtime
// virtual texture"): the blended terrain layer material on a clipmap around
// the camera, so the terrain pixel shader reads it instead of blending up to
// six layers per pixel. Level k covers resolution x resolution texels of
// texelSize * 2^k metres, its window centred on the camera and snapped to
// 1/16 of its extent; each frame only the strips a window gained are drawn,
// by the terrain draws themselves seen from above with the
// PSTerrainVirtualTexture entry (Object.hlsl). Three RGBA8 arrays, slice =
// level: albedo (sRGB) + ground-cover share; normal XZ + roughness +
// occlusion; layer mean colour (sRGB) + filled flag. DX12 only.
class TerrainVirtualTexture
{
public:
    struct Settings
    {
        bool enabled = false;
        uint32_t levels = 6;
        uint32_t resolution = 2048;
        float texelSize = 0.004f;
        bool operator==(const Settings& other) const
        {
            return enabled == other.enabled && levels == other.levels &&
                resolution == other.resolution && texelSize == other.texelSize;
        }
    };
    // Creates (or drops) the textures for these settings. False when off or
    // the backend cannot render into texture arrays.
    bool Configure(Graphics::IGraphicsProvider* provider, const Settings& settings);
    bool Active() const { return m_active; }
    // Object.hlsl ObjectData::terrainTrailReserved: level-0 texel (m),
    // levels, texels per side, on.
    glm::vec4 ShaderConstants() const;
    std::array<const Graphics::IGraphicsTexture*, 3> Textures() const;
    // Everything is drawn again (layer arrays became resident, paint or
    // trails changed, the terrain set changed).
    void Invalidate();
    // Records this frame's fill into `context`: the strips each level's
    // window gained moving with the camera, drawn by the terrain draws among
    // `draws` (camera view, records uploaded, object buffers bound) that
    // overlap them. submit binds a draw's geometry and draws it.
    void Record(Graphics::IGraphicsContext* context, Graphics::IGraphicsProvider* provider,
        const std::vector<PreparedDraw>& draws, const glm::vec3& camera,
        Graphics::IPipelineState* pipeline, uint64_t frameSlot,
        const std::function<void(const PreparedDraw&)>& submit);
    // A terrain draw this texture is filled from (and that may sample it).
    static bool Fills(const PreparedDraw& draw);
    struct Statistics { uint32_t rectangles = 0, draws = 0, fullLevels = 0, newSources = 0; uint64_t texels = 0; };
    const Statistics& LastStatistics() const { return m_statistics; }

private:
    struct Level { glm::ivec2 center{0}; bool valid = false; };
    Settings m_settings;
    bool m_active = false;
    std::shared_ptr<Graphics::IGraphicsTexture> m_albedo, m_normal, m_mean, m_depth;
    std::vector<Level> m_levels;
    // Terrain draws (vertex buffers) drawn last frame; see Record.
    std::unordered_set<const void*> m_previousSources;
    // Per frame slot: the fill draws' constants (256-byte records).
    struct FillConstants { std::shared_ptr<Graphics::IGraphicsBuffer> buffer; void* mapped = nullptr; size_t capacity = 0; };
    std::vector<FillConstants> m_constants;
    Statistics m_statistics;
};
}
