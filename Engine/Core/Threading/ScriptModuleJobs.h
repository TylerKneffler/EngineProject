#pragma once
#include "Core/Threading/JobScheduler.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace Engine::Threading
{
// The editor owns this interface and its scheduler. Reloadable code may
// provide callbacks, but it cannot own the queue that keeps them alive.
class IScriptModuleJobs
{
public:
    virtual ~IScriptModuleJobs() = default;
    virtual bool Submit(std::function<void()> callback) = 0;
};

class ScriptModuleJobs final : public IScriptModuleJobs
{
public:
    explicit ScriptModuleJobs(JobScheduler& scheduler = JobScheduler::Get())
        : m_scheduler(scheduler), m_cancelled(std::make_shared<std::atomic_bool>(false)) {}

    bool Submit(std::function<void()> callback) override
    {
        std::lock_guard lock(m_mutex);
        if (m_closed) return false;
        auto cancelled = m_cancelled;
        m_jobs.push_back(m_scheduler.Schedule(
            [cancelled, callback = std::move(callback)]
            { if (!cancelled->load(std::memory_order_acquire)) callback(); },
            {}, false));
        return true;
    }

    void CloseAndWait()
    {
        std::vector<JobScheduler::Handle> jobs;
        {
            std::lock_guard lock(m_mutex);
            m_closed = true;
            m_cancelled->store(true, std::memory_order_release);
            jobs.swap(m_jobs);
        }
        // Wait every callback, including failures, before its DLL can unload.
        for (const auto& job : jobs)
            try { m_scheduler.Wait(job); } catch (...) {}
    }

    ~ScriptModuleJobs() override { CloseAndWait(); }
private:
    JobScheduler& m_scheduler;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    std::mutex m_mutex;
    std::vector<JobScheduler::Handle> m_jobs;
    bool m_closed = false;
};

inline IScriptModuleJobs*& ScriptModuleJobsSlot()
{
    static IScriptModuleJobs* host = nullptr;
    return host;
}
}
