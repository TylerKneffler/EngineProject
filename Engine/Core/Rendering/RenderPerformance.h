#pragma once
#include <cstdint>
#include <algorithm>
#include <array>
#include <cstddef>
#include <ostream>
#include <vector>
namespace Engine::Rendering
{
struct RenderViewIdentity
{
    uint64_t id=0;
    bool portal=false,shadow=false;
    bool operator==(const RenderViewIdentity& other) const
    { return id==other.id && portal==other.portal && shadow==other.shadow; }
};
struct InstanceStorageTimings
{
    uint64_t newBytes=0,editedBytes=0,growthBytes=0,metadataBytes=0,visibleIdBytes=0,fallbackBytes=0;
    uint64_t allocations=0,retirements=0,cacheCold=0,cacheMembership=0,cacheMetadata=0,cacheMode=0;
    uint64_t residentBytes=0;
    uint64_t preparedRecords=0,preparationWorkerPasses=0;
    template<class Stream> void WriteJson(Stream& output,const char* prefix="instance_") const
    {
        const auto field=[&](const char* name,uint64_t value) { output<<",\""<<prefix<<name<<"\":"<<value; };
        field("new_bytes",newBytes); field("edited_bytes",editedBytes); field("growth_bytes",growthBytes);
        field("metadata_bytes",metadataBytes); field("visible_id_bytes",visibleIdBytes);
        field("fallback_bytes",fallbackBytes);
        field("allocations",allocations); field("retirements",retirements); field("resident_bytes",residentBytes);
        field("prepared_records",preparedRecords); field("preparation_worker_passes",preparationWorkerPasses);
        field("cache_cold",cacheCold); field("cache_membership",cacheMembership); field("cache_metadata",cacheMetadata); field("cache_mode",cacheMode);
    }
    InstanceStorageTimings& operator+=(const InstanceStorageTimings& value)
    {
#define ADD(field) field+=value.field
        ADD(newBytes); ADD(editedBytes); ADD(growthBytes); ADD(metadataBytes); ADD(visibleIdBytes); ADD(fallbackBytes);
        ADD(allocations); ADD(retirements); ADD(cacheCold); ADD(cacheMembership); ADD(cacheMetadata); ADD(cacheMode);
        ADD(preparedRecords); ADD(preparationWorkerPasses);
#undef ADD
        if(value.residentBytes>residentBytes) residentBytes=value.residentBytes;
        return *this;
    }
};
struct RenderVisibilityMetrics
{
    uint64_t views=0,frameItems=0,candidateItems=0,acceptedItems=0;
    uint64_t viewIdentity=0;
    std::vector<RenderViewIdentity> viewIdentities;
    uint64_t bvhNodesVisited=0,bvhBoundsTested=0,visibleSlots=0;
    uint64_t instanceTilesSubmitted=0,nonTerrainInstanceTilesSubmitted=0,placementCandidates=0;
    std::array<uint64_t,3> placementCandidatesByLod{},placementTrianglesByLod{};
    std::array<uint64_t,3> screenSelectedInstancesByLod{},screenSelectedTrianglesByLod{};
    std::array<uint64_t,3> gpuSelectedInstancesByLod{},gpuTransitionBandInstancesByLod{};
    uint64_t gpuReadbackSamples=0,gpuReadbackViewIdentity=0;
    uint64_t drawGroups=0,materialGroups=0;
    RenderVisibilityMetrics& operator+=(const RenderVisibilityMetrics& value)
    {
        views+=value.views; frameItems+=value.frameItems;
        if (value.viewIdentity) viewIdentity=value.viewIdentity;
        for (const auto& identity : value.viewIdentities)
            if (std::find(viewIdentities.begin(),viewIdentities.end(),identity)==viewIdentities.end())
                viewIdentities.push_back(identity);
        candidateItems+=value.candidateItems; acceptedItems+=value.acceptedItems;
        bvhNodesVisited+=value.bvhNodesVisited;
        bvhBoundsTested+=value.bvhBoundsTested; visibleSlots+=value.visibleSlots;
        instanceTilesSubmitted+=value.instanceTilesSubmitted;
        nonTerrainInstanceTilesSubmitted+=value.nonTerrainInstanceTilesSubmitted;
        placementCandidates+=value.placementCandidates;
        drawGroups+=value.drawGroups; materialGroups+=value.materialGroups;
        for (std::size_t lod=0; lod<placementCandidatesByLod.size(); ++lod)
        {
            placementCandidatesByLod[lod]+=value.placementCandidatesByLod[lod];
            placementTrianglesByLod[lod]+=value.placementTrianglesByLod[lod];
            screenSelectedInstancesByLod[lod]+=value.screenSelectedInstancesByLod[lod];
            screenSelectedTrianglesByLod[lod]+=value.screenSelectedTrianglesByLod[lod];
            gpuSelectedInstancesByLod[lod]+=value.gpuSelectedInstancesByLod[lod];
            gpuTransitionBandInstancesByLod[lod]+=value.gpuTransitionBandInstancesByLod[lod];
        }
        gpuReadbackSamples+=value.gpuReadbackSamples;
        if (value.gpuReadbackViewIdentity) gpuReadbackViewIdentity=value.gpuReadbackViewIdentity;
        return *this;
    }
};
struct RenderCpuTimings
{
    InstanceStorageTimings instances;
    RenderVisibilityMetrics cameraVisibility,shadowVisibility;
    double cullingMs=0, sortingMs=0, materialMs=0, uploadMs=0, submissionMs=0, batchingMs=0;
    double totalMs=0, startupMs=0, terrainPreparationMs=0, occlusionMs=0, otherMs=0;
    double recordPackingMs=0, recordWaitMs=0;
    double batchSnapshotMs=0,batchBuildMs=0,batchWaitMs=0;
    double instanceTilePrepareMs=0;
    double clusterPreparationMs=0;
    uint64_t clusterCacheHits=0,clusterUploadBytes=0;
    uint64_t instanceTileReused=0,instanceTileBuilt=0,instanceTileWorkerPasses=0;
    uint64_t batchEligibleDraws=0,batchCacheHits=0,batchWorkerJobs=0,batchReusedKeys=0;
    double workerActiveMs=0, mainJobMs=0, workerWaitMs=0, retainedUiMs=0, portalMs=0;
    double workerQueueMs=0,workerDependencyMs=0;
    uint64_t configuredWorkers=0;
    uint64_t workerJobs=0, workerTasks=0;
    uint64_t gpuGrassBatches=0, grassBatchCacheHits=0, grassPlacementUploadBytes=0;
    uint64_t automaticBatches=0, automaticInstances=0, gpuBatches=0, changedUploadBytes=0;
    uint64_t candidateDraws=0, physicalDraws=0, candidateTriangles=0, views=0;
    uint64_t visibilityFrameItems=0,visibilityCandidates=0,visibilityAccepted=0;
    std::array<uint64_t,3> colorSelectedLodDraws{},shadowSelectedLodDraws{};
    uint64_t submittedInstances=0, submittedTriangles=0;
    uint64_t terrainOcclusionRejected=0, solidOcclusionRejected=0;
    RenderCpuTimings& operator+=(const RenderCpuTimings& value)
    {
        instances+=value.instances;
        cameraVisibility+=value.cameraVisibility;
        shadowVisibility+=value.shadowVisibility;
#define ADD(field) field+=value.field
        ADD(cullingMs); ADD(sortingMs); ADD(materialMs); ADD(uploadMs); ADD(submissionMs); ADD(batchingMs);
        ADD(recordPackingMs); ADD(recordWaitMs);
        ADD(batchSnapshotMs); ADD(batchBuildMs); ADD(batchWaitMs);
        ADD(instanceTilePrepareMs); ADD(instanceTileReused); ADD(instanceTileBuilt); ADD(instanceTileWorkerPasses);
        ADD(clusterPreparationMs); ADD(clusterCacheHits); ADD(clusterUploadBytes);
        ADD(batchEligibleDraws); ADD(batchCacheHits); ADD(batchWorkerJobs); ADD(batchReusedKeys);
        ADD(workerActiveMs); ADD(mainJobMs); ADD(workerWaitMs); ADD(retainedUiMs); ADD(portalMs);
        ADD(workerQueueMs); ADD(workerDependencyMs);
        configuredWorkers=std::max(configuredWorkers,value.configuredWorkers);
        ADD(workerJobs); ADD(workerTasks);
        ADD(gpuGrassBatches); ADD(grassBatchCacheHits); ADD(grassPlacementUploadBytes);
        ADD(totalMs); ADD(startupMs); ADD(terrainPreparationMs); ADD(occlusionMs); ADD(otherMs);
        ADD(automaticBatches); ADD(automaticInstances); ADD(gpuBatches); ADD(changedUploadBytes);
        ADD(candidateDraws); ADD(physicalDraws); ADD(candidateTriangles); ADD(views);
        ADD(visibilityFrameItems); ADD(visibilityCandidates); ADD(visibilityAccepted);
        for (std::size_t lod = 0; lod < colorSelectedLodDraws.size(); ++lod)
        {
            colorSelectedLodDraws[lod] += value.colorSelectedLodDraws[lod];
            shadowSelectedLodDraws[lod] += value.shadowSelectedLodDraws[lod];
        }
        ADD(submittedInstances); ADD(submittedTriangles);
        ADD(terrainOcclusionRejected); ADD(solidOcclusionRejected);
#undef ADD
        return *this;
    }
};
}
