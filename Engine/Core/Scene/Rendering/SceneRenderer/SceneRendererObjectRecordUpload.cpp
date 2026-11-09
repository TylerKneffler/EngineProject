#include "Core/Scene/Rendering/SceneRenderer/SceneRendererObjectRecordUpload.h"

#include "Core/Rendering/RenderJobs.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <vector>

namespace Engine::Scene::Renderer
{
namespace
{
struct DirtyRange
{
    uint32_t first = UINT32_MAX;
    uint32_t last = 0;
    double elapsedMs = 0.0;
};
}

struct SceneRendererObjectRecordUpload::BufferedRecords
{
    std::vector<std::future<DirtyRange>> jobs;

    ~BufferedRecords()
    {
        for (auto& job : jobs)
            if (job.valid()) job.wait();
    }
};

SceneRendererObjectRecordUpload::SceneRendererObjectRecordUpload(
    Scene& scene,
    std::vector<PreparedDraw>& draws,
    bool shadowOnly)
    : m_scene(scene), m_uploaded(shadowOnly),
      m_recordSnapshot([]() -> BufferedRecords& {
          static thread_local std::array<BufferedRecords, 2> buffers;
          static thread_local unsigned index = 0;
          return buffers[index++ % buffers.size()];
      }())
{
    m_recordSnapshot.jobs.clear();
    if (shadowOnly)
        return;

    const size_t count = draws.size();
    scene.m_objectUploadCache.Prepare(
        size_t(scene.m_objectDataBuffer->GetSize() / sizeof(ObjectGPUData)),
        sizeof(ObjectGPUData));
    auto* objectCache = &scene.m_objectUploadCache;
    auto* objectBytes = static_cast<uint8_t*>(scene.m_objectDataMapped);
    auto* constantBytes = static_cast<uint8_t*>(scene.m_objectCBMapped);
    const size_t taskCount = std::min<size_t>(3, (count + 63) / 64);
    for (size_t task = 0; task < taskCount; ++task)
    {
        const size_t first = count * task / taskCount;
        const size_t last = count * (task + 1) / taskCount;
        m_recordSnapshot.jobs.push_back(Engine::Rendering::RenderJobs::Get().Submit(
            [&draws, objectBytes, objectCache, constantBytes, first, last,
                stride = scene.kCBStride]()
            {
                const auto started = std::chrono::steady_clock::now();
                DirtyRange dirty;
                for (size_t i = first; i < last; ++i)
                {
                    const auto& draw = draws[i];
                    const uint32_t slot = draw.drawData.objectIndex;
                    std::memcpy(constantBytes + size_t(slot) * stride,
                        &draw.drawData, sizeof(draw.drawData));
                    if (objectCache->Write(slot, &draw.objectData, objectBytes))
                    {
                        dirty.first = std::min(dirty.first, slot);
                        dirty.last = std::max(dirty.last, slot);
                    }
                }
                dirty.elapsedMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                return dirty;
            }));
    }
}

SceneRendererObjectRecordUpload::~SceneRendererObjectRecordUpload()
{
    for (auto& job : m_recordSnapshot.jobs)
        if (job.valid()) job.wait();
}

void SceneRendererObjectRecordUpload::EnsureUploaded()
{
    if (m_uploaded)
        return;

    using Clock = std::chrono::steady_clock;
    auto& timings = m_scene.m_lastRenderCpuTimings;
    const auto waitStart = Clock::now();
    for (auto& job : m_recordSnapshot.jobs)
    {
        const auto dirty = job.get();
        m_firstDirtyObject = std::min(m_firstDirtyObject, dirty.first);
        m_lastDirtyObject = std::max(m_lastDirtyObject, dirty.last);
        timings.recordPackingMs += dirty.elapsedMs;
    }
    m_recordSnapshot.jobs.clear();
    timings.recordWaitMs += std::chrono::duration<double, std::milli>(
        Clock::now() - waitStart).count();

    const auto uploadStart = Clock::now();
    if (m_firstDirtyObject != UINT32_MAX)
    {
        const uint64_t bytes = uint64_t(m_lastDirtyObject - m_firstDirtyObject + 1) *
            sizeof(ObjectGPUData);
        m_scene.m_objectDataBuffer->FlushMappedWrites(
            uint64_t(m_firstDirtyObject) * sizeof(ObjectGPUData), bytes);
        m_scene.m_lastObjectDataUploadBytes += bytes;
        timings.changedUploadBytes += bytes;
    }
    timings.uploadMs += std::chrono::duration<double, std::milli>(
        Clock::now() - uploadStart).count();
    m_uploaded = true;
}
}
