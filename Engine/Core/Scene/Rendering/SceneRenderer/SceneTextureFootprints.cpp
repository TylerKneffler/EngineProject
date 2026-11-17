#include "Core/Scene/Rendering/SceneRenderer/SceneTextureFootprints.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Rendering/TextureFootprint.h"
#include "Core/Rendering/RenderBoundsCache.h"
#include "Core/Rendering/RenderJobs.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace Engine::Scene::Renderer
{
namespace
{
// Instances per work item. Each instance projects its box into the camera
// and every shadow cascade, so large forests dominate this pass; chunking
// them spreads one draw across workers.
constexpr size_t kInstancesPerChunk = 512;

struct FootprintDraw
{
    const Engine::Components::Mesh* mesh = nullptr; // bounds source; null = no estimate
    float uvScale = 0.f;
    bool instanced = false;
    uint64_t stateHash = 0; // instanced only: inputs of the projection besides the view
};

struct FootprintChunk
{
    size_t draw = 0;
    size_t begin = 0, end = 0; // instance range; unused for single draws
    float pixels = 0.f;
    float coverage = 0.f;
    Engine::Rendering::TextureFootprintCache::ChunkResult* cached = nullptr;
    bool project = true;
};

template<class T> void HashBytes(uint64_t& hash, const T& value)
{
    unsigned char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    for (unsigned char byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
}
}

void SceneTextureFootprints::Request(const std::vector<PreparedDraw>& preparedDraws,
    const glm::mat4& viewProjection, uint32_t viewportWidth, uint32_t viewportHeight,
    const std::array<glm::mat4, 4>& shadowViewProjections,
    uint32_t shadowCascadeCount, uint32_t shadowResolution,
    Engine::Rendering::TextureFootprintCache* cache, uintptr_t viewKey)
{
    if(!Engine::Components::Texture::GetStreamingSnapshot().budgetBytes)
        return;
    using Cache = Engine::Rendering::TextureFootprintCache;
    Cache::View* view = nullptr;
    if(cache)
    {
        ++cache->requests;
        view = &cache->views[viewKey];
        ++view->frame;
        view->lastRequest = cache->requests;
    }

    // Gather once per view after resolving cached material records. Include
    // every placement and directional cascade, including offscreen casters.
    std::vector<FootprintDraw> draws(preparedDraws.size());
    std::vector<FootprintChunk> chunks;
    chunks.reserve(preparedDraws.size());
    for(size_t index=0;index<preparedDraws.size();++index)
    {
        const auto& draw=preparedDraws[index];
        const auto* mesh=draw.mesh && draw.mesh->IsInstanced() ? draw.mesh->GetInstanceSource():draw.mesh;
        if(!(mesh && mesh->HasBounds() && draw.objectData.skinParams.y<=0.f &&
            !mesh->HasMorphTargets() && !mesh->HasSurfaceOffsets() &&
            (!draw.material || draw.material->shader.empty())))
            continue;
        float uvScale=mesh->GetTextureUvWorldScale();
        if(draw.material && (draw.material->baseColorUvSet || draw.material->normalUvSet ||
            draw.material->metallicRoughnessUvSet || draw.material->occlusionUvSet ||
            draw.material->emissiveUvSet || draw.material->heightUvSet))
            uvScale=std::max(uvScale,mesh->GetTextureUvWorldScale(1));
        if(draw.terrain || draw.terrainCell) uvScale=uvScale*1.5f/std::max(1.f,draw.objectData.textureUvSets1.z);
        auto& footprintDraw=draws[index];
        footprintDraw.mesh=mesh;
        footprintDraw.uvScale=uvScale;
        footprintDraw.instanced=draw.mesh->IsInstanced() && draw.mesh->GetInstanceData();
        if(footprintDraw.instanced)
        {
            const auto& instanceData=*draw.mesh->GetInstanceData();
            uint64_t hash=1469598103934665603ull;
            HashBytes(hash,mesh);
            HashBytes(hash,draw.objectData.world);
            HashBytes(hash,draw.objectData.textureUvSets1);
            HashBytes(hash,uvScale);
            HashBytes(hash,uint8_t((draw.castsShadow ? 1:0) | (draw.terrain ? 2:0) | (draw.terrainCell ? 4:0)));
            footprintDraw.stateHash=hash;
            const size_t count=instanceData.transforms.size();
            for(size_t begin=0;begin<count;begin+=kInstancesPerChunk)
            {
                FootprintChunk chunk{index,begin,std::min(count,begin+kInstancesPerChunk)};
                if(view)
                {
                    const Cache::ChunkKey key{&instanceData,instanceData.revision,hash,begin};
                    auto [found,inserted]=view->chunks.try_emplace(key);
                    chunk.cached=&found->second;
                    chunk.cached->used=view->frame;
                    if(inserted)
                        // Stagger first refreshes so later re-projections
                        // spread evenly over the refresh window.
                        chunk.cached->due=view->frame+1+(chunks.size()%Cache::RefreshFrames);
                    else if(view->frame<chunk.cached->due)
                    {
                        chunk.project=false;
                        chunk.pixels=chunk.cached->pixels;
                        chunk.coverage=chunk.cached->coverage;
                    }
                }
                chunks.push_back(chunk);
            }
        }
        else chunks.push_back({index,0,0});
    }
    std::vector<size_t> projected;
    projected.reserve(chunks.size());
    for(size_t index=0;index<chunks.size();++index)
        if(chunks[index].project) projected.push_back(index);

    // Workers only read draws, meshes and instance transforms; each chunk
    // owns its result. Texture requests happen after the join.
    Engine::Rendering::RenderJobs::Get().For(projected.size(),1,[&](size_t projectedIndex)
    {
        auto& chunk=chunks[projected[projectedIndex]];
        const auto& draw=preparedDraws[chunk.draw];
        const auto& footprintDraw=draws[chunk.draw];
        const auto* mesh=footprintDraw.mesh;
        const auto footprint=[&](const glm::mat4& world,float& covered) {
            auto low=mesh->GetBoundsMin(),high=mesh->GetBoundsMax();
            auto projectionWorld=world;
            float units=footprintDraw.uvScale;
            if((draw.terrain || draw.terrainCell) && draw.objectData.textureUvSets1.x>0.f)
            {
                const auto bounds=Engine::Rendering::TransformBounds(low,high,world);
                low=bounds.minimum;high=bounds.maximum;projectionWorld=glm::mat4(1.f);
                units=1.5f*draw.objectData.textureUvSets1.x/std::max(1.f,draw.objectData.textureUvSets1.z);
            }
            float demand=Engine::Rendering::ProjectedTextureFootprint(low,high,
                viewProjection*projectionWorld,{float(viewportWidth),float(viewportHeight)},units,&covered);
            if(draw.castsShadow) for(uint32_t cascade=0;cascade<shadowCascadeCount;++cascade)
                demand=std::max(demand,Engine::Rendering::ProjectedTextureFootprint(low,high,
                    shadowViewProjections[cascade]*projectionWorld,glm::vec2(float(shadowResolution)/2.f),units));
            return demand;
        };
        if(footprintDraw.instanced)
        {
            const auto& transforms=draw.mesh->GetInstanceData()->transforms;
            for(size_t instance=chunk.begin;instance<chunk.end;++instance)
            {
                const auto& affine=transforms[instance];
                glm::mat4 matrix(1.f);
                for(int row=0;row<3;++row) for(int column=0;column<4;++column) matrix[column][row]=affine.rows[row][column];
                float covered=0.f;
                chunk.pixels=std::max(chunk.pixels,footprint(draw.objectData.world*matrix,covered));
                chunk.coverage+=covered;
            }
        }
        else chunk.pixels=footprint(draw.objectData.world,chunk.coverage);
    });

    if(view)
    {
        for(size_t index:projected)
            if(auto* cached=chunks[index].cached)
            {
                cached->pixels=chunks[index].pixels;
                cached->coverage=chunks[index].coverage;
                if(cached->due<=view->frame) cached->due=view->frame+Cache::RefreshFrames;
            }
        // Drop chunks of removed or changed instance sets, and views that
        // stopped rendering (closed editor viewports, replaced cameras).
        if(view->frame%64==0)
            for(auto it=view->chunks.begin();it!=view->chunks.end();)
                if(it->second.used+8<view->frame) it=view->chunks.erase(it); else ++it;
        if(cache->requests%256==0)
            for(auto it=cache->views.begin();it!=cache->views.end();)
                if(it->second.lastRequest+1024<cache->requests) it=cache->views.erase(it); else ++it;
    }

    // Chunks are ordered by draw, so one pass reduces them.
    std::vector<float> pixels(preparedDraws.size(),std::numeric_limits<float>::infinity());
    std::vector<float> coverage(preparedDraws.size(),1.f);
    for(size_t index=0;index<draws.size();++index)
        if(draws[index].mesh) pixels[index]=coverage[index]=0.f; // no instances: no demand
    for(size_t index=0;index<chunks.size();)
    {
        const size_t drawIndex=chunks[index].draw;
        float drawPixels=0.f,drawCoverage=0.f;
        for(;index<chunks.size() && chunks[index].draw==drawIndex;++index)
        {
            drawPixels=std::max(drawPixels,chunks[index].pixels);
            drawCoverage+=chunks[index].coverage;
        }
        pixels[drawIndex]=drawPixels;
        // One draw cannot cover more than the whole view.
        coverage[drawIndex]=draws[drawIndex].instanced ? std::min(drawCoverage,1.f):drawCoverage;
    }
    for(size_t index=0;index<preparedDraws.size();++index)
    {
        const auto& draw=preparedDraws[index];
        for(size_t slot=0;slot<draw.textures.size();++slot)
            Engine::Components::Texture::RequestGraphicsFootprint(draw.textures[slot],
                slot==10 || slot==11 ? std::numeric_limits<float>::infinity():pixels[index],
                slot==10 || slot==11 ? 1.f:coverage[index]);
    }
}
}
