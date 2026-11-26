#pragma once

#include "Core/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"
#include "Core/Rendering/TextureFootprintCache.h"

namespace Engine::Rendering
{
// The fields of a PreparedDraw the footprint request reads, copied so the
// request can run on another thread while recording goes on (the draws are
// still being edited: fades, levels, groups). Same member names.
struct FootprintSource
{
    const Engine::Components::Mesh* mesh = nullptr;
    const Engine::Components::Material* material = nullptr;
    struct { glm::mat4 world{1.f}; glm::vec4 textureUvSets1{0.f}; glm::vec4 skinParams{0.f}; } objectData;
    bool terrain = false, terrainCell = false, castsShadow = false;
    std::array<const Engine::Graphics::IGraphicsTexture*, 30> textures{};
};

class SceneTextureFootprints
{
public:
    static std::vector<FootprintSource> Snapshot(const std::vector<PreparedDraw>& draws)
    {
        std::vector<FootprintSource> sources(draws.size());
        for (size_t i = 0; i < draws.size(); ++i)
        {
            const auto& draw = draws[i];
            auto& source = sources[i];
            source.mesh = draw.mesh;
            source.material = draw.material;
            source.objectData.world = draw.objectData.world;
            source.objectData.textureUvSets1 = draw.objectData.textureUvSets1;
            source.objectData.skinParams = draw.objectData.skinParams;
            source.terrain = draw.terrain;
            source.terrainCell = draw.terrainCell;
            source.castsShadow = draw.castsShadow;
            source.textures = draw.textures;
        }
        return sources;
    }
    // `view` identifies the camera/viewport so cached instance results never
    // mix between views. A null cache re-projects every instance each call.
    static void Request(const std::vector<FootprintSource>& draws,
        const glm::mat4& viewProjection, uint32_t viewportWidth, uint32_t viewportHeight,
        const std::array<glm::mat4, 4>& shadowViewProjections,
        uint32_t shadowCascadeCount, uint32_t shadowResolution,
        Engine::Rendering::TextureFootprintCache* cache = nullptr, uintptr_t view = 0);
};
}
