#include "Core/Scene/Rendering/SceneRenderer/SceneInstanceBatching.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Rendering/RenderJobs.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <unordered_map>
#include <unordered_set>

namespace Engine::Scene::Renderer
{
glm::vec2 SceneInstanceBatching::ScaleShadowLodDistances(glm::vec2 distances, bool shadowOnly)
{
    if (!shadowOnly) return distances;
    const float lod1 = std::max(0.f, distances.x * .3f);
    const float lod2 = std::max(lod1, distances.y * .6f);
    return {lod1, lod2};
}

SceneInstanceBatching::Result SceneInstanceBatching::Prepare(Scene& scene,
    std::vector<PreparedDraw>& preparedDraws, bool shadowOnly, bool automaticInstancing,
    bool hasPortalViews, bool includeEditorVisuals)
{
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
        const auto* drawLayer = includeEditorVisuals && scene.m_selectedPlacementId != 0 &&
            scene.m_selectedPlacementLayer && instanceData && draw.object
            ? draw.object->GetComponentInParent<
                Engine::Components::TerrainObjectLayer>() : nullptr;
        const bool selectedPlacement = includeEditorVisuals &&
            scene.m_selectedPlacementId != 0 && scene.m_selectedPlacementLayer &&
            drawLayer && drawLayer->Owner == scene.m_selectedPlacementLayer &&
            instanceData &&
            std::find(instanceData->placementIds.begin(),
                instanceData->placementIds.end(), scene.m_selectedPlacementId) !=
                    instanceData->placementIds.end();
        const bool selected=includeEditorVisuals && (selectedPlacement ||
            draw.object==scene.m_selectedObject ||
            draw.mesh==scene.m_editorSelectedMesh ||
            draw.mesh==scene.m_editorWeightPaintMesh);
        if ((!automaticInstancing && !(draw.drawData.flags & (0x00100000u|0x00800000u))) || scene.m_editorMode2D || hasPortalViews || !draw.mesh || draw.blended ||
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
    scene.m_lastRenderCpuTimings.batchEligibleDraws=batchInput.size();
    scene.m_lastRenderCpuTimings.batchSnapshotMs=std::chrono::duration<double,std::milli>(RenderClock::now()-batchingStart).count();
    auto& batchCache=scene.m_drawBatchPreparation[shadowOnly ? 1:0];
    BatchPreparation::Result batchResult;
    std::future<BatchPreparation::Result> batchJob;
    struct BatchFence { std::future<BatchPreparation::Result>& job; ~BatchFence() { if(job.valid()) job.wait(); } } batchFence{batchJob};
    auto& batchScheduler=Engine::Rendering::RenderJobs::Get();
    // Small sets stay on the owner. Large copied snapshots can overlap the
    // draw statistics below; workers never inspect live meshes or materials.
    if(batchInput.size()>=256 && batchScheduler.WorkerCount() && batchScheduler.ParallelForEnabled())
    {
        scene.m_lastRenderCpuTimings.batchWorkerJobs=1;
        batchJob=batchScheduler.Submit([&batchCache,input=std::move(batchInput)]() mutable {
            return batchCache.Prepare(std::move(input));
        });
    }
    else batchResult=batchCache.Prepare(std::move(batchInput));
    scene.m_lastOrdinaryDrawCount = 0;
    scene.m_lastOrdinaryTriangleCount = 0;
    for (const PreparedDraw& draw : preparedDraws)
    {
        if (draw.mesh && draw.mesh->IsInstanced())
        {
            for (const auto& range : draw.mesh->GetInstanceRanges())
                if (range.instanceCount && range.vertexCount)
                {
                    ++scene.m_lastOrdinaryDrawCount;
                    scene.m_lastOrdinaryTriangleCount += uint64_t(range.vertexCount / 3) * range.instanceCount;
                }
        }
        else
        {
            ++scene.m_lastOrdinaryDrawCount;
            scene.m_lastOrdinaryTriangleCount += (draw.indexBuffer ? draw.indexCount : draw.vertexCount) / 3;
        }
    }
    if(shadowOnly)
    {
        std::unordered_set<const Engine::Core::Object*> eligibleCasters;
        for(const auto& draw:preparedDraws)
            if(draw.castsShadow && draw.vertexBuffer && !(draw.spatialManipulator && draw.spatialManipulator->enabled))
                eligibleCasters.insert(draw.object);
        scene.m_shadowDebugSnapshot.casterCount=static_cast<uint32_t>(eligibleCasters.size());
    }
    if (shadowOnly) scene.m_shadowDebugSnapshot.submittedCasterDrawCount = 0;

    const auto batchWaitStart=RenderClock::now();
    if(batchJob.valid()) batchResult=batchJob.get();
    scene.m_lastRenderCpuTimings.batchWaitMs=std::chrono::duration<double,std::milli>(RenderClock::now()-batchWaitStart).count();
    scene.m_lastRenderCpuTimings.batchBuildMs=batchResult.buildMs;
    scene.m_lastRenderCpuTimings.batchCacheHits=batchResult.reused ? 1:0;
    scene.m_lastRenderCpuTimings.batchReusedKeys=batchResult.reusedKeys;
    auto& automaticGroups = result.automaticGroups;
    automaticGroups=std::move(batchResult.groups);
    for (uint32_t group=0;group<automaticGroups.size();++group)
        if (automaticGroups[group].members.size()>1 || preparedDraws[automaticGroups[group].members.front()].gpuLod>=0 || (preparedDraws[automaticGroups[group].members.front()].drawData.flags & 0x00100000u))
            for(auto index:automaticGroups[group].members) preparedDraws[index].automaticGroup=group;
    scene.m_lastRenderCpuTimings.batchingMs=std::chrono::duration<double,std::milli>(RenderClock::now()-batchingStart).count();

    // Register resident placements once per source/view, not once per species,
    // LOD and cascade. Camera changes are compute constants, not placement edits.
    const auto tilePrepareStart=RenderClock::now();
    using TilePreparation=Engine::Rendering::InstanceTilePreparation;
    std::vector<TilePreparation::Input> tileInputs;
    std::vector<size_t> tileDrawIndexes;
    std::unordered_map<const void*,uint32_t> placementBases;
    for(size_t index=0;index<preparedDraws.size();++index)
    {
        const auto& draw=preparedDraws[index];
        if(draw.automaticGroup==UINT32_MAX || !draw.mesh->IsInstanced()) continue;
        const auto& data=draw.mesh->GetInstanceData();
        auto [base,inserted]=placementBases.try_emplace(data.get(),0);
        if(inserted)
            base->second=scene.m_persistentInstances.Register(data,data->revision,data->transforms.data(),
                uint32_t(data->transforms.size()),data->stableId,data->placementIds);
        TilePreparation::Input input;
        input.slot=draw.drawData.objectIndex;
        input.base=base->second;
        input.maximumLod=uint32_t(draw.gpuMaximumLod);
        input.world=draw.objectData.world;
        input.minimum=draw.mesh->GetInstanceSource()->GetBoundsMin();
        input.maximum=draw.mesh->GetInstanceSource()->GetBoundsMax();
        if(draw.drawData.flags & 0x00100000u) for(int lod=1;lod<=draw.gpuMaximumLod;++lod)
        {
            const auto* variant=draw.mesh->GetRuntimeLodMesh(lod)->GetInstanceSource();
            input.minimum=glm::min(input.minimum,variant->GetBoundsMin());
            input.maximum=glm::max(input.maximum,variant->GetBoundsMax());
        }
        // Mesh bounds include the source's authored deformation envelope, so
        // every instance tile (camera and cascade) uses the same conservative
        // bounds without needing to identify the deformation type.
        input.minimum = glm::min(input.minimum, draw.mesh->GetBoundsMin());
        input.maximum = glm::max(input.maximum, draw.mesh->GetBoundsMax());
        float shadowRange = draw.objectData.shadowCasterFade.x;
        if (shadowOnly && draw.gpuLod >= 0 && draw.mesh->instanceLodBand<0)
        {
            if (draw.gpuLod == 1) shadowRange *= .7f;
            else if (draw.gpuLod >= 2) shadowRange *= .4f;
        }
        const auto* lodPolicy = draw.object
            ? draw.object->GetComponent<Engine::Components::Mesh>() : draw.mesh;
        const float finalCullDistance = lodPolicy && lodPolicy->cullAfterLastLod
            ? std::max(0.f, lodPolicy->finalLodCullDistance) : 0.f;
        input.lod={ScaleShadowLodDistances(draw.gpuLodDistances, shadowOnly),
            shadowRange,finalCullDistance};
        for(const auto& range:draw.mesh->GetInstanceRanges())
            input.ranges.emplace_back(range.firstInstance,range.instanceCount);
        tileInputs.push_back(std::move(input));
        tileDrawIndexes.push_back(index);
    }
    const auto tileResult=scene.m_instanceTilePreparation.Prepare(tileInputs,
        scene.m_graphicsProvider->GetContextFactory()->GetPreparedFrameSerial(),shadowOnly ? 1u:0u);
    auto& instanceTiles = result.instanceTiles;
    instanceTiles.resize(preparedDraws.size());
    for(size_t i=0;i<tileDrawIndexes.size();++i)
        instanceTiles[tileDrawIndexes[i]]=tileResult.records[i];
    scene.m_lastRenderCpuTimings.instanceTileReused=tileResult.reused;
    scene.m_lastRenderCpuTimings.instanceTileBuilt=tileResult.built;
    scene.m_lastRenderCpuTimings.instanceTileWorkerPasses=tileResult.workerBatches;
    scene.m_lastRenderCpuTimings.instanceTilePrepareMs=std::chrono::duration<double,std::milli>(RenderClock::now()-tilePrepareStart).count();
    auto& residentPlacementPool = result.residentPlacementPool;
    if(!tileInputs.empty())
    {
        const auto uploadStart=RenderClock::now();
        const auto allocations=scene.m_persistentInstances.stats.allocations;
        residentPlacementPool=scene.m_persistentInstances.Upload(scene.m_graphicsProvider);
        scene.m_lastRenderCpuTimings.instances.allocations+=scene.m_persistentInstances.stats.allocations-allocations;
        scene.m_lastRenderCpuTimings.uploadMs+=std::chrono::duration<double,std::milli>(RenderClock::now()-uploadStart).count();
    }

    return result;
}
}
