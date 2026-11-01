#pragma once
#include <cstdint>
namespace Engine::Rendering
{
struct RenderCpuTimings
{
    double cullingMs=0, sortingMs=0, materialMs=0, uploadMs=0, submissionMs=0, batchingMs=0;
    double totalMs=0, startupMs=0, terrainPreparationMs=0, occlusionMs=0, otherMs=0;
    double recordPackingMs=0, recordWaitMs=0;
    double workerActiveMs=0, mainJobMs=0, workerWaitMs=0, retainedUiMs=0, portalMs=0;
    uint64_t workerJobs=0, workerTasks=0;
    uint64_t automaticBatches=0, automaticInstances=0, gpuBatches=0, changedUploadBytes=0;
    uint64_t candidateDraws=0, physicalDraws=0, candidateTriangles=0, views=0;
    RenderCpuTimings& operator+=(const RenderCpuTimings& value)
    {
#define ADD(field) field+=value.field
        ADD(cullingMs); ADD(sortingMs); ADD(materialMs); ADD(uploadMs); ADD(submissionMs); ADD(batchingMs);
        ADD(recordPackingMs); ADD(recordWaitMs);
        ADD(workerActiveMs); ADD(mainJobMs); ADD(workerWaitMs); ADD(retainedUiMs); ADD(portalMs);
        ADD(workerJobs); ADD(workerTasks);
        ADD(totalMs); ADD(startupMs); ADD(terrainPreparationMs); ADD(occlusionMs); ADD(otherMs);
        ADD(automaticBatches); ADD(automaticInstances); ADD(gpuBatches); ADD(changedUploadBytes);
        ADD(candidateDraws); ADD(physicalDraws); ADD(candidateTriangles); ADD(views);
#undef ADD
        return *this;
    }
};
}
