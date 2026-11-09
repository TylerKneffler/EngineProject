#include "Core/Scene/Rendering/SceneRenderer/SceneTextureFootprints.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Rendering/TextureFootprint.h"
#include "Core/Rendering/RenderBoundsCache.h"
#include <algorithm>
#include <limits>

namespace Engine::Scene::Renderer
{
void SceneTextureFootprints::Request(const std::vector<PreparedDraw>& preparedDraws,
    const glm::mat4& viewProjection, uint32_t viewportWidth, uint32_t viewportHeight,
    const std::array<glm::mat4, 4>& shadowViewProjections,
    uint32_t shadowCascadeCount, uint32_t shadowResolution)
{
    if(Engine::Components::Texture::GetStreamingSnapshot().budgetBytes)
    {
        // Gather once per view after resolving cached material records. Include
        // every placement and directional cascade, including offscreen casters.
        for(const auto& draw:preparedDraws)
        {
            float pixels=std::numeric_limits<float>::infinity();
            const auto* mesh=draw.mesh && draw.mesh->IsInstanced() ? draw.mesh->GetInstanceSource():draw.mesh;
            if(mesh && mesh->HasBounds() && draw.objectData.skinParams.y<=0.f &&
                !mesh->HasMorphTargets() && !mesh->HasSurfaceOffsets() &&
                (!draw.material || draw.material->shader.empty()))
            {
                float uvScale=mesh->GetTextureUvWorldScale();
                if(draw.material && (draw.material->baseColorUvSet || draw.material->normalUvSet ||
                    draw.material->metallicRoughnessUvSet || draw.material->occlusionUvSet ||
                    draw.material->emissiveUvSet || draw.material->heightUvSet))
                    uvScale=std::max(uvScale,mesh->GetTextureUvWorldScale(1));
                if(draw.terrain || draw.terrainCell) uvScale=uvScale*1.5f/std::max(1.f,draw.objectData.textureUvSets1.z);
                const auto footprint=[&](const glm::mat4& world) {
                    auto low=mesh->GetBoundsMin(),high=mesh->GetBoundsMax();
                    auto projectionWorld=world;
                    float units=uvScale;
                    if((draw.terrain || draw.terrainCell) && draw.objectData.textureUvSets1.x>0.f)
                    {
                        const auto bounds=Engine::Rendering::TransformBounds(low,high,world);
                        low=bounds.minimum;high=bounds.maximum;projectionWorld=glm::mat4(1.f);
                        units=1.5f*draw.objectData.textureUvSets1.x/std::max(1.f,draw.objectData.textureUvSets1.z);
                    }
                    float demand=Engine::Rendering::ProjectedTextureFootprint(low,high,
                        viewProjection*projectionWorld,{float(viewportWidth),float(viewportHeight)},units);
                    if(draw.castsShadow) for(uint32_t cascade=0;cascade<shadowCascadeCount;++cascade)
                        demand=std::max(demand,Engine::Rendering::ProjectedTextureFootprint(low,high,
                            shadowViewProjections[cascade]*projectionWorld,glm::vec2(float(shadowResolution)/2.f),units));
                    return demand;
                };
                pixels=0.f;
                if(draw.mesh->IsInstanced() && draw.mesh->GetInstanceData())
                    for(const auto& affine:draw.mesh->GetInstanceData()->transforms)
                    {
                        glm::mat4 matrix(1.f);
                        for(int row=0;row<3;++row) for(int column=0;column<4;++column) matrix[column][row]=affine.rows[row][column];
                        pixels=std::max(pixels,footprint(draw.objectData.world*matrix));
                    }
                else pixels=footprint(draw.objectData.world);
            }
            for(size_t slot=0;slot<draw.textures.size();++slot)
                Engine::Components::Texture::RequestGraphicsFootprint(draw.textures[slot],
                    slot==10 || slot==11 ? std::numeric_limits<float>::infinity():pixels);
        }
    }
}
}
