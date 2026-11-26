#include "Core/Rendering/SceneRenderer.h"
#include <cstdlib>
#include "Core/Rendering/SceneRenderer/SceneInstanceBatching.h"
#include "Core/Rendering/RenderUtilities.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Physics/SpatialManipulator.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Rendering/RenderJobs.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <unordered_map>
#include <unordered_set>

namespace Engine::Rendering
{
using Scene = Engine::Scene::Scene;
glm::vec2 SceneInstanceBatching::ScaleShadowLodDistances(glm::vec2 distances, bool shadowOnly)
{
    if (!shadowOnly) return distances;
    const float lod1 = std::max(0.f, distances.x * .3f);
    const float lod2 = std::max(lod1, distances.y * .6f);
    return {lod1, lod2};
}

SceneInstanceBatching::Result SceneInstanceBatching::Prepare(SceneRenderer& renderer,
    std::vector<PreparedDraw>& preparedDraws, bool shadowOnly, bool automaticInstancing,
    bool hasPortalViews, bool includeEditorVisuals)
{
    Scene& scene = renderer.GetScene();
    using RenderClock = std::chrono::steady_clock;
    Result result;
    const auto batchingStart = RenderClock::now();
    using BatchPreparation=Engine::Rendering::DrawBatchPreparation;
    using AutomaticKey=Engine::Rendering::DrawBatchKey;
    std::vector<BatchPreparation::Input> batchInput;
    batchInput.reserve(preparedDraws.size());
    for (uint32_t index=0;index<preparedDraws.size();++index)
    {
        auto& draw=preparedDraws[index];
        const bool custom = draw.material &&
            !IsFastFoliageSurface(draw.material) &&
            !draw.material->shader.empty();
        const auto* instanceData = draw.mesh && draw.mesh->IsInstanced()
            ? draw.mesh->GetInstanceData().get() : nullptr;
        const auto* drawLayer = includeEditorVisuals && renderer.GetViewOverlays().selectedPlacementId != 0 &&
            renderer.GetViewOverlays().selectedPlacementLayer && instanceData && draw.object
            ? draw.object->GetComponentInParent<
                Engine::Components::TerrainObjectLayer>() : nullptr;
        const bool selectedPlacement = includeEditorVisuals &&
            renderer.GetViewOverlays().selectedPlacementId != 0 && renderer.GetViewOverlays().selectedPlacementLayer &&
            drawLayer && drawLayer->Owner == renderer.GetViewOverlays().selectedPlacementLayer &&
            instanceData &&
            std::find(instanceData->placementIds.begin(),
                instanceData->placementIds.end(), renderer.GetViewOverlays().selectedPlacementId) !=
                    instanceData->placementIds.end();
        const bool selected=includeEditorVisuals && (selectedPlacement ||
            draw.object==renderer.GetViewOverlays().selectedObject ||
            draw.mesh==renderer.GetViewOverlays().selectedMesh ||
            draw.mesh==renderer.GetViewOverlays().weightPaintMesh);
        if ((!automaticInstancing && !(draw.drawData.flags & (0x00100000u|0x00800000u))) || scene.IsMode2D() || hasPortalViews || !draw.mesh || draw.blended ||
            draw.preview || draw.pmxOutline || draw.spatialManipulator || draw.traversalChartPortal ||
            draw.objectData.skinParams.y>0 || draw.mesh->HasMorphTargets() || draw.mesh->HasSurfaceOffsets() ||
            draw.vertexBuffer!=draw.mesh->GetGraphicsBuffer() || custom || selected || !draw.mesh->HasBounds()) continue;
        AutomaticKey key{draw.vertexBuffer,draw.indexBuffer,draw.pipeline,
            draw.textures,draw.vertexStride,draw.vertexCount,draw.indexCount,draw.drawData.lightCount,
            draw.drawData.flags | (draw.terrain ? 0x08000000u : 0u) | (draw.doubleSided ? 0x04000000u : 0u),draw.castsShadow,draw.gpuLod,{},{},automaticInstancing ? nullptr:draw.object};
        if(draw.mesh->IsInstanced())
        {
            for(const auto& range:draw.mesh->GetInstanceRanges()) std::get<11>(key).emplace_back(range.firstVertex,range.vertexCount);
            if(draw.drawData.flags & 0x00100000u) for(int lod=0;lod<=draw.gpuMaximumLod;++lod)
                std::get<12>(key)[lod]=draw.mesh->GetRuntimeLodMesh(lod)->GetGraphicsBuffer();
        }
        batchInput.push_back({std::move(key),draw.drawData.objectIndex,index});
    }
    renderer.m_lastRenderCpuTimings.batchEligibleDraws=batchInput.size();
    renderer.m_lastRenderCpuTimings.batchSnapshotMs=std::chrono::duration<double,std::milli>(RenderClock::now()-batchingStart).count();
    auto& batchCache=renderer.m_drawBatchPreparation[shadowOnly ? 1:0];
    BatchPreparation::Result batchResult;
    std::future<BatchPreparation::Result> batchJob;
    struct BatchFence { std::future<BatchPreparation::Result>& job; ~BatchFence() { if(job.valid()) job.wait(); } } batchFence{batchJob};
    auto& batchScheduler=Engine::Rendering::RenderJobs::Get();
    // Small sets stay on the owner. Large copied snapshots can overlap the
    // draw statistics below; workers never inspect live meshes or materials.
    if(batchInput.size()>=256 && batchScheduler.WorkerCount() && batchScheduler.ParallelForEnabled())
    {
        renderer.m_lastRenderCpuTimings.batchWorkerJobs=1;
        batchJob=batchScheduler.Submit([&batchCache,input=std::move(batchInput)]() mutable {
            return batchCache.Prepare(std::move(input));
        });
    }
    else batchResult=batchCache.Prepare(std::move(batchInput));
    renderer.m_lastOrdinaryDrawCount = 0;
    renderer.m_lastOrdinaryTriangleCount = 0;
    for (const PreparedDraw& draw : preparedDraws)
    {
        if (draw.mesh && draw.mesh->IsInstanced())
        {
            for (const auto& range : draw.mesh->GetInstanceRanges())
                if (range.instanceCount && range.vertexCount)
                {
                    ++renderer.m_lastOrdinaryDrawCount;
                    renderer.m_lastOrdinaryTriangleCount += uint64_t(range.vertexCount / 3) * range.instanceCount;
                }
        }
        else
        {
            ++renderer.m_lastOrdinaryDrawCount;
            renderer.m_lastOrdinaryTriangleCount += (draw.indexBuffer ? draw.indexCount : draw.vertexCount) / 3;
        }
    }
    if(shadowOnly)
    {
        std::unordered_set<const Engine::Core::Object*> eligibleCasters;
        for(const auto& draw:preparedDraws)
            if(draw.castsShadow && draw.vertexBuffer && !(draw.spatialManipulator && draw.spatialManipulator->enabled))
                eligibleCasters.insert(draw.object);
        renderer.m_shadowDebugSnapshot.casterCount=static_cast<uint32_t>(eligibleCasters.size());
    }
    if (shadowOnly) renderer.m_shadowDebugSnapshot.submittedCasterDrawCount = 0;

    const auto batchWaitStart=RenderClock::now();
    if(batchJob.valid()) batchResult=batchJob.get();
    renderer.m_lastRenderCpuTimings.batchWaitMs=std::chrono::duration<double,std::milli>(RenderClock::now()-batchWaitStart).count();
    renderer.m_lastRenderCpuTimings.batchBuildMs=batchResult.buildMs;
    renderer.m_lastRenderCpuTimings.batchCacheHits=batchResult.reused ? 1:0;
    renderer.m_lastRenderCpuTimings.batchReusedKeys=batchResult.reusedKeys;
    auto& automaticGroups = result.automaticGroups;
    automaticGroups=std::move(batchResult.groups);
    for (uint32_t group=0;group<automaticGroups.size();++group)
        // Shared parts (instance blocks) always draw through the batched
        // path: their stub instance data holds no placements of its own.
        if (automaticGroups[group].members.size()>1 || preparedDraws[automaticGroups[group].members.front()].gpuLod>=0 || (preparedDraws[automaticGroups[group].members.front()].drawData.flags & 0x00100000u) ||
            preparedDraws[automaticGroups[group].members.front()].mesh->UsesInstanceBlocks() ||
            preparedDraws[automaticGroups[group].members.front()].mesh->gpuBladeField)
            for(auto index:automaticGroups[group].members) preparedDraws[index].automaticGroup=group;
    renderer.m_lastRenderCpuTimings.batchingMs=std::chrono::duration<double,std::milli>(RenderClock::now()-batchingStart).count();

    // Register resident placements once per source/view, not once per species,
    // LOD and cascade. Camera changes are compute constants, not placement edits.
    const auto tilePrepareStart=RenderClock::now();
    renderer.m_lastRenderCpuTimings.instanceTileRegistrations=0;
    renderer.m_lastRenderCpuTimings.instanceTileRanges=0;
    using TilePreparation=Engine::Rendering::InstanceTilePreparation;
    std::vector<TilePreparation::Input> tileInputs;
    std::vector<size_t> tileDrawIndexes;
    // Each placement source registers once per frame: a stamp on the data, as
    // a shared part's blocks are visited by every part drawing them, and the
    // shadow and camera views of a frame see the same placements (the stamp
    // was per view, so the camera repeated the shadow pass's registrations).
    // ENGINE_REGISTER_PER_VIEW=1 restores the per-view stamp (A/B runs).
    static std::atomic<uint64_t> registrationPasses{0};
    static const bool perView=[]{ const char* v=std::getenv("ENGINE_REGISTER_PER_VIEW"); return v && v[0]=='1'; }();
    const uint64_t pass=perView ? ++registrationPasses
        : (renderer.m_graphicsProvider->GetContextFactory()->GetPreparedFrameSerial() << 1) | 1u;
    for(size_t index=0;index<preparedDraws.size();++index)
    {
        const auto& draw=preparedDraws[index];
        if(draw.automaticGroup==UINT32_MAX || !draw.mesh->IsInstanced()) continue;
        const auto registerPlacements=[&](const std::shared_ptr<Engine::Components::Mesh::InstanceData>& data)
        {
            if(data->poolPass!=pass)
            {
                ++renderer.m_lastRenderCpuTimings.instanceTileRegistrations;
                data->poolBase=renderer.m_persistentInstances.Register(data,data->revision,data->transforms.data(),
                    uint32_t(data->transforms.size()),data->stableId,data->placementIds,&data->poolHandle);
                data->poolPass=pass;
            }
            return data->poolBase;
        };
        TilePreparation::Input input;
        input.slot=draw.drawData.objectIndex;
        if(draw.mesh->UsesInstanceBlocks())
        {
            // Shared part: one tile per geometry range and block, range-major
            // (SceneRendering reads tile rangeIndex*blocks+block). Records
            // address the pool directly, so the base stays 0.
            const auto& blocks=draw.mesh->GetInstanceBlocks();
            std::vector<uint32_t> bases; bases.reserve(blocks.size());
            for(const auto& block:blocks) bases.push_back(registerPlacements(block.data));
            for(size_t range=0;range<draw.mesh->GetInstanceRanges().size();++range)
                for(size_t block=0;block<blocks.size();++block)
                    input.ranges.emplace_back(bases[block],uint32_t(blocks[block].data->transforms.size()));
        }
        else input.base=registerPlacements(draw.mesh->GetInstanceData());
        input.maximumLod=uint32_t(draw.gpuMaximumLod);
        input.world=draw.objectData.world;
        input.thin=draw.objectData.placementThin;
        if (!draw.mesh->GetInstanceGeometryBounds(input.minimum, input.maximum))
            throw std::logic_error("Instanced geometry requires source bounds");
        if(draw.drawData.flags & 0x00100000u) for(int lod=1;lod<=draw.gpuMaximumLod;++lod)
        {
            glm::vec3 minimum, maximum;
            if (!draw.mesh->GetRuntimeLodMesh(lod)->GetInstanceGeometryBounds(minimum, maximum, draw.mesh))
                throw std::logic_error("Instanced LOD requires source bounds");
            input.minimum=glm::min(input.minimum,minimum);
            input.maximum=glm::max(input.maximum,maximum);
        }
        // Never union the aggregate batch bounds here: compute applies each
        // placement transform to this source-local box. Applying placements
        // again to an entire tile defeats frustum, range and LOD rejection.
        float shadowRange = draw.objectData.shadowCasterFade.x;
        if (shadowOnly && draw.gpuLod >= 0 && draw.mesh->instanceLodBand<0)
        {
            if (draw.gpuLod == 1) shadowRange *= .7f;
            else if (draw.gpuLod >= 2) shadowRange *= .4f;
        }
        const auto* lodPolicy = draw.object
            ? draw.object->GetComponent<Engine::Components::Mesh>() : draw.mesh;
        const float finalCullDistance = lodPolicy && lodPolicy->cullAfterLastLod
            ? std::max(0.f, lodPolicy->finalLodCullDistance) * draw.lodScale : 0.f;
        input.lod={ScaleShadowLodDistances(draw.gpuLodDistances, shadowOnly),
            shadowRange,finalCullDistance};
        if(!draw.mesh->UsesInstanceBlocks())
            for(const auto& range:draw.mesh->GetInstanceRanges())
                input.ranges.emplace_back(range.firstInstance,range.instanceCount);
        tileInputs.push_back(std::move(input));
        tileDrawIndexes.push_back(index);
    }
    renderer.m_lastRenderCpuTimings.instanceTileInputMs=std::chrono::duration<double,std::milli>(RenderClock::now()-tilePrepareStart).count();
    for(const auto& input:tileInputs) renderer.m_lastRenderCpuTimings.instanceTileRanges+=input.ranges.size();
    const auto tileResult=renderer.m_instanceTilePreparation.Prepare(tileInputs,
        renderer.m_graphicsProvider->GetContextFactory()->GetPreparedFrameSerial(),shadowOnly ? 1u:0u);
    auto& instanceTiles = result.instanceTiles;
    instanceTiles.resize(preparedDraws.size());
    for(size_t i=0;i<tileDrawIndexes.size();++i)
        instanceTiles[tileDrawIndexes[i]]=tileResult.records[i];
    renderer.m_lastRenderCpuTimings.instanceTileReused=tileResult.reused;
    renderer.m_lastRenderCpuTimings.instanceTileBuilt=tileResult.built;
    renderer.m_lastRenderCpuTimings.instanceTileWorkerPasses=tileResult.workerBatches;
    renderer.m_lastRenderCpuTimings.instanceTilePrepareMs=std::chrono::duration<double,std::milli>(RenderClock::now()-tilePrepareStart).count();
    auto& residentPlacementPool = result.residentPlacementPool;
    if(!tileInputs.empty())
    {
        const auto uploadStart=RenderClock::now();
        const auto allocations=renderer.m_persistentInstances.stats.allocations;
        residentPlacementPool=renderer.m_persistentInstances.Upload(renderer.m_graphicsProvider);
        renderer.m_lastRenderCpuTimings.instances.allocations+=renderer.m_persistentInstances.stats.allocations-allocations;
        renderer.m_lastRenderCpuTimings.uploadMs+=std::chrono::duration<double,std::milli>(RenderClock::now()-uploadStart).count();
    }

    return result;
}
}
