#pragma once
#include <cstdint>
#include <algorithm>
#include <ostream>
namespace Engine::Rendering
{
struct InstanceStorageTimings
{
    uint64_t newBytes=0,editedBytes=0,growthBytes=0,metadataBytes=0,visibleIdBytes=0,fallbackBytes=0;
    uint64_t allocations=0,retirements=0,cacheCold=0,cacheMembership=0,cacheMetadata=0,cacheMode=0;
    uint64_t residentBytes=0;
    template<class Stream> void WriteJson(Stream& output,const char* prefix="instance_") const
    {
        const auto field=[&](const char* name,uint64_t value) { output<<",\""<<prefix<<name<<"\":"<<value; };
        field("new_bytes",newBytes); field("edited_bytes",editedBytes); field("growth_bytes",growthBytes);
        field("metadata_bytes",metadataBytes); field("visible_id_bytes",visibleIdBytes);
        field("fallback_bytes",fallbackBytes);
        field("allocations",allocations); field("retirements",retirements); field("resident_bytes",residentBytes);
        field("cache_cold",cacheCold); field("cache_membership",cacheMembership); field("cache_metadata",cacheMetadata); field("cache_mode",cacheMode);
    }
    InstanceStorageTimings& operator+=(const InstanceStorageTimings& value)
    {
#define ADD(field) field+=value.field
        ADD(newBytes); ADD(editedBytes); ADD(growthBytes); ADD(metadataBytes); ADD(visibleIdBytes); ADD(fallbackBytes);
        ADD(allocations); ADD(retirements); ADD(cacheCold); ADD(cacheMembership); ADD(cacheMetadata); ADD(cacheMode);
#undef ADD
        if(value.residentBytes>residentBytes) residentBytes=value.residentBytes;
        return *this;
    }
};
struct RenderCpuTimings
{
    InstanceStorageTimings instances;
    double cullingMs=0, sortingMs=0, materialMs=0, uploadMs=0, submissionMs=0, batchingMs=0;
    double totalMs=0, startupMs=0, terrainPreparationMs=0, occlusionMs=0, otherMs=0;
    double recordPackingMs=0, recordWaitMs=0;
    double workerActiveMs=0, mainJobMs=0, workerWaitMs=0, retainedUiMs=0, portalMs=0;
    double workerQueueMs=0,workerDependencyMs=0;
    uint64_t configuredWorkers=0;
    uint64_t workerJobs=0, workerTasks=0;
    uint64_t gpuGrassBatches=0, grassBatchCacheHits=0, grassPlacementUploadBytes=0;
    uint64_t automaticBatches=0, automaticInstances=0, gpuBatches=0, changedUploadBytes=0;
    uint64_t candidateDraws=0, physicalDraws=0, candidateTriangles=0, views=0;
    RenderCpuTimings& operator+=(const RenderCpuTimings& value)
    {
        instances+=value.instances;
#define ADD(field) field+=value.field
        ADD(cullingMs); ADD(sortingMs); ADD(materialMs); ADD(uploadMs); ADD(submissionMs); ADD(batchingMs);
        ADD(recordPackingMs); ADD(recordWaitMs);
        ADD(workerActiveMs); ADD(mainJobMs); ADD(workerWaitMs); ADD(retainedUiMs); ADD(portalMs);
        ADD(workerQueueMs); ADD(workerDependencyMs);
        configuredWorkers=std::max(configuredWorkers,value.configuredWorkers);
        ADD(workerJobs); ADD(workerTasks);
        ADD(gpuGrassBatches); ADD(grassBatchCacheHits); ADD(grassPlacementUploadBytes);
        ADD(totalMs); ADD(startupMs); ADD(terrainPreparationMs); ADD(occlusionMs); ADD(otherMs);
        ADD(automaticBatches); ADD(automaticInstances); ADD(gpuBatches); ADD(changedUploadBytes);
        ADD(candidateDraws); ADD(physicalDraws); ADD(candidateTriangles); ADD(views);
#undef ADD
        return *this;
    }
};
}
