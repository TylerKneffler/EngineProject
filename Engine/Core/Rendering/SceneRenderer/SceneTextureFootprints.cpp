#include "Core/Rendering/SceneRenderer/SceneTextureFootprints.h"
#include <cstdlib>
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Materials/Texture.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Rendering/TextureFootprint.h"
#include "Core/Rendering/RenderBoundsCache.h"
#include "Core/Rendering/RenderJobs.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Engine::Rendering
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
    // Whose placements: the mesh's instance data, or one block of a shared part.
    const Engine::Components::Mesh::InstanceData* data = nullptr;
    float pixels = 0.f;
    float coverage = 0.f;
    Engine::Rendering::TextureFootprintCache::ChunkResult* cached = nullptr;
    bool project = true;
    // A far-object block projected as one box (its world bounds, the largest
    // placement scale): no per-chunk enumeration or cache lookups.
    bool wholeBlock = false;
    // From a far-object block (Mesh::InstanceData::traceSource 3).
    bool farBlock = false;
    // Collects this draw's far-block chunks into `cached` after projection
    // (the per-part far aggregate, see the gather loop).
    bool farAggregate = false;
    glm::vec3 blockMinimum{0.f}, blockMaximum{0.f};
    float blockScale = 1.f;
};

template<class T> void HashBytes(uint64_t& hash, const T& value)
{
    unsigned char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    for (unsigned char byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
}
}

void SceneTextureFootprints::Request(const std::vector<FootprintSource>& preparedDraws,
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
            uint64_t hash=1469598103934665603ull;
            HashBytes(hash,mesh);
            HashBytes(hash,draw.objectData.world);
            HashBytes(hash,draw.objectData.textureUvSets1);
            HashBytes(hash,uvScale);
            HashBytes(hash,uint8_t((draw.castsShadow ? 1:0) | (draw.terrain ? 2:0) | (draw.terrainCell ? 4:0)));
            footprintDraw.stateHash=hash;
            std::vector<const Engine::Components::Mesh::InstanceData*> sources;
            if(draw.mesh->UsesInstanceBlocks())
            {
                // A shared part spans every loaded cell: only blocks the camera
                // or a shadow cascade can see ask for texture detail (a
                // per-cell object out of view was not prepared at all).
                const auto outside=[](const glm::mat4& m,const glm::vec3& low,const glm::vec3& high)
                {
                    const auto row=[&](int r) { return glm::vec4(m[0][r],m[1][r],m[2][r],m[3][r]); };
                    for(int plane=0;plane<4;++plane)
                    {
                        const glm::vec4 p=row(3)+(plane&1 ? -1.f:1.f)*row(plane>>1);
                        const glm::vec3 corner(p.x>=0.f ? high.x:low.x,p.y>=0.f ? high.y:low.y,p.z>=0.f ? high.z:low.z);
                        if(glm::dot(glm::vec3(p),corner)+p.w<0.f) return true;
                    }
                    return false;
                };
                // Far-object blocks (distant forest) ask once per block from
                // its box instead of per 512-placement chunk: the chunk walk
                // and its cache lookups were ~1.3 ms of camera recording with
                // the 8 km forest, for no change in resident textures
                // (todo.md Step 0). ENGINE_FOOTPRINT_WHOLE_FAR_BLOCKS=0: per chunk.
                static const bool wholeFarBlocks=[]{ const char* v=std::getenv("ENGINE_FOOTPRINT_WHOLE_FAR_BLOCKS"); return !(v && v[0]=='0'); }();
                bool identityWorld=true;
                for(int c=0;c<4 && identityWorld;++c) for(int r=0;r<4;++r)
                    if(std::abs(draw.objectData.world[c][r]-(c==r ? 1.f:0.f))>1e-5f) { identityWorld=false; break; }
                // The far forest's demand changes slowly: per part, its far
                // blocks are walked every RefreshFrames frames (staggered by
                // part) and their combined demand reused in between. The walk
                // (~287 blocks x every impostor part, frustum and cascade tests
                // and a cache lookup each) was ~1.3 ms of camera recording.
                bool farCached=false;
                Cache::ChunkResult* farAggregate=nullptr;
                if(view && wholeFarBlocks && identityWorld && draw.mesh->GetInstanceData())
                {
                    const auto* stub=draw.mesh->GetInstanceData().get();
                    const Cache::ChunkKey farKey{stub,stub->revision,hash,SIZE_MAX-1};
                    auto [found,inserted]=view->chunks.try_emplace(farKey);
                    found->second.used=view->frame;
                    if(inserted) found->second.due=view->frame+1+(index%Cache::RefreshFrames);
                    else if(view->frame<found->second.due) farCached=true;
                    FootprintChunk chunk{index,0,0};
                    chunk.project=false;
                    if(farCached)
                    {
                        chunk.pixels=found->second.pixels;
                        chunk.coverage=found->second.coverage;
                    }
                    else
                    {
                        chunk.farAggregate=true;
                        chunk.cached=&found->second;
                        farAggregate=&found->second;
                    }
                    chunks.push_back(chunk);
                }
                for(const auto& block:draw.mesh->GetInstanceBlocks())
                {
                    if(farCached && block.data && block.data->traceSource==3) continue;
                    bool seen=!outside(viewProjection,block.minimum,block.maximum);
                    if(draw.castsShadow)
                        for(uint32_t cascade=0;cascade<shadowCascadeCount && !seen;++cascade)
                            seen=!outside(shadowViewProjections[cascade],block.minimum,block.maximum);
                    if(!seen) continue;
                    // Whole-block only when the box is wholly in front of the
                    // camera (its corners well past the near plane).
                    bool inFront=false;
                    if(wholeFarBlocks && identityWorld && block.data && block.data->traceSource==3 &&
                        block.data->hasCullStats && !block.data->transforms.empty())
                    {
                        inFront=true;
                        for(int corner=0;corner<8 && inFront;++corner)
                            inFront=(viewProjection*glm::vec4(corner&1?block.maximum.x:block.minimum.x,
                                corner&2?block.maximum.y:block.minimum.y,corner&4?block.maximum.z:block.minimum.z,1.f)).w>1.f;
                    }
                    if(inFront)
                    {
                        FootprintChunk chunk{index,0,0};
                        chunk.data=block.data.get();
                        chunk.wholeBlock=true;
                        chunk.farBlock=true;
                        chunk.blockMinimum=block.minimum; chunk.blockMaximum=block.maximum;
                        // Row lengths bound each axis scale of a rotated, scaled placement.
                        chunk.blockScale=std::max(block.data->coverageMaximum,block.data->maximumMatrixNorm*0.57735f);
                        if(view)
                        {
                            // Cached like a chunk (begin = SIZE_MAX marks a whole block).
                            const Cache::ChunkKey key{block.data.get(),block.data->revision,hash,SIZE_MAX};
                            auto [found,inserted]=view->chunks.try_emplace(key);
                            chunk.cached=&found->second;
                            chunk.cached->used=view->frame;
                            if(inserted) chunk.cached->due=view->frame+1+(chunks.size()%Cache::RefreshFrames);
                            else if(view->frame<chunk.cached->due)
                            {
                                chunk.project=false;
                                chunk.pixels=chunk.cached->pixels;
                                chunk.coverage=chunk.cached->coverage;
                            }
                        }
                        chunks.push_back(chunk);
                        continue;
                    }
                    sources.push_back(block.data.get());
                }
            }
            else sources.push_back(draw.mesh->GetInstanceData().get());
            for(const auto* source:sources)
            {
            const auto& instanceData=*source;
            const size_t count=instanceData.transforms.size();
            for(size_t begin=0;begin<count;begin+=kInstancesPerChunk)
            {
                FootprintChunk chunk{index,begin,std::min(count,begin+kInstancesPerChunk)};
                chunk.data=source;
                chunk.farBlock=source->traceSource==3;
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
        const auto footprint=[&](const glm::mat4& world,float& covered,bool shadows) {
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
            if(shadows && draw.castsShadow) for(uint32_t cascade=0;cascade<shadowCascadeCount;++cascade)
                demand=std::max(demand,Engine::Rendering::ProjectedTextureFootprint(low,high,
                    shadowViewProjections[cascade]*projectionWorld,glm::vec2(float(shadowResolution)/2.f),units));
            return demand;
        };
        if(chunk.wholeBlock)
        {
            const float units=footprintDraw.uvScale*chunk.blockScale;
            float covered=0.f;
            float demand=Engine::Rendering::ProjectedTextureFootprint(chunk.blockMinimum,chunk.blockMaximum,
                viewProjection,{float(viewportWidth),float(viewportHeight)},units,&covered);
            if(std::isfinite(demand))
            {
                if(draw.castsShadow) for(uint32_t cascade=0;cascade<shadowCascadeCount;++cascade)
                {
                    const float shadow=Engine::Rendering::ProjectedTextureFootprint(chunk.blockMinimum,chunk.blockMaximum,
                        shadowViewProjections[cascade],glm::vec2(float(shadowResolution)/2.f),units);
                    if(std::isfinite(shadow)) demand=std::max(demand,shadow);
                }
                chunk.pixels=demand; chunk.coverage=covered;
                return;
            }
            // The camera is inside the block's box: every placement exactly.
            chunk.pixels=0.f; chunk.coverage=0.f;
            for(const auto& affine:chunk.data->transforms)
            {
                glm::mat4 matrix(1.f);
                for(int row=0;row<3;++row) for(int column=0;column<4;++column) matrix[column][row]=affine.rows[row][column];
                float instanceCovered=0.f;
                chunk.pixels=std::max(chunk.pixels,footprint(draw.objectData.world*matrix,instanceCovered,true));
                chunk.coverage+=instanceCovered;
            }
            return;
        }
        if(footprintDraw.instanced)
        {
            const auto& transforms=chunk.data->transforms;
            const auto instanceWorld=[&](size_t instance) {
                const auto& affine=transforms[instance];
                glm::mat4 matrix(1.f);
                for(int row=0;row<3;++row) for(int column=0;column<4;++column) matrix[column][row]=affine.rows[row][column];
                return draw.objectData.world*matrix;
            };
            bool shadowsPerInstance=draw.castsShadow;
            float shadowDemand=0.f;
            if(!draw.terrain && !draw.terrainCell && chunk.end>chunk.begin)
            {
                // World box of the chunk and the largest instance scale; fixed
                // for a cache key (instance revision and draw state).
                Engine::Rendering::TextureFootprintCache::ChunkResult local;
                auto* bounds=chunk.cached ? chunk.cached:&local;
                if(!bounds->boundsValid)
                {
                    glm::vec3 low(std::numeric_limits<float>::max()),high(-std::numeric_limits<float>::max());
                    float maxScale=0.f;
                    for(size_t instance=chunk.begin;instance<chunk.end;++instance)
                    {
                        const auto world=instanceWorld(instance);
                        const auto box=Engine::Rendering::TransformBounds(mesh->GetBoundsMin(),mesh->GetBoundsMax(),world);
                        low=glm::min(low,box.minimum);high=glm::max(high,box.maximum);
                        maxScale=std::max({maxScale,glm::length(glm::vec3(world[0])),
                            glm::length(glm::vec3(world[1])),glm::length(glm::vec3(world[2]))});
                    }
                    bounds->boundsMin=low;bounds->boundsMax=high;bounds->maxScale=maxScale;
                    bounds->boundsValid=true;
                }
                const glm::vec3 low=bounds->boundsMin,high=bounds->boundsMax;
                const float units=footprintDraw.uvScale*bounds->maxScale;
                // Shadow cascades are orthographic: texel density does not
                // depend on depth, so the box with the largest scale bounds
                // every instance's shadow demand in one projection. A box
                // reaching past the light's near plane reports infinity; those
                // chunks keep the per-instance shadow projection.
                if(draw.castsShadow)
                {
                    for(uint32_t cascade=0;cascade<shadowCascadeCount;++cascade)
                        shadowDemand=std::max(shadowDemand,Engine::Rendering::ProjectedTextureFootprint(low,high,
                            shadowViewProjections[cascade],glm::vec2(float(shadowResolution)/2.f),units));
                    shadowsPerInstance=!std::isfinite(shadowDemand);
                    if(shadowsPerInstance) shadowDemand=0.f;
                }
                // A chunk far away relative to its own size (distant forest)
                // is bounded in the camera by one projection of its box too:
                // at 4x its diagonal away the over-estimate stays under ~25%.
                // Nearer chunks keep the exact per-instance projection.
                float nearestDepth=std::numeric_limits<float>::max();
                for(int corner=0;corner<8;++corner)
                    nearestDepth=std::min(nearestDepth,(viewProjection*glm::vec4(corner&1?high.x:low.x,
                        corner&2?high.y:low.y,corner&4?high.z:low.z,1.f)).w);
                if(!shadowsPerInstance && nearestDepth>4.f*glm::length(high-low))
                {
                    float covered=0.f;
                    chunk.pixels=std::max(shadowDemand,Engine::Rendering::ProjectedTextureFootprint(low,high,
                        viewProjection,{float(viewportWidth),float(viewportHeight)},units,&covered));
                    chunk.coverage=covered;
                    return;
                }
            }
            chunk.pixels=shadowDemand;
            for(size_t instance=chunk.begin;instance<chunk.end;++instance)
            {
                float covered=0.f;
                chunk.pixels=std::max(chunk.pixels,footprint(instanceWorld(instance),covered,shadowsPerInstance));
                chunk.coverage+=covered;
            }
        }
        else chunk.pixels=footprint(draw.objectData.world,chunk.coverage,true);
    });

    if(view)
    {
        // Far aggregates: the max demand and summed coverage of their draw's
        // far-block chunks this frame (which also count this frame directly).
        for(size_t index=0;index<chunks.size();++index)
        {
            auto& aggregate=chunks[index];
            if(!aggregate.farAggregate || !aggregate.cached) continue;
            float farPixels=0.f,farCoverage=0.f;
            for(size_t other=index+1;other<chunks.size() && chunks[other].draw==aggregate.draw;++other)
                if(chunks[other].farBlock)
                {
                    farPixels=std::max(farPixels,chunks[other].pixels);
                    farCoverage+=chunks[other].coverage;
                }
            aggregate.cached->pixels=farPixels;
            aggregate.cached->coverage=farCoverage;
            aggregate.cached->due=view->frame+Cache::RefreshFrames;
        }
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
