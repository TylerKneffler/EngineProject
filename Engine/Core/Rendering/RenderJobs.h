#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace Engine::Rendering
{
// Synchronous jobs over immutable render snapshots. Scene mutation and graphics
// submission stay on the caller. Small workloads avoid waking worker threads.
class RenderJobs
{
    std::mutex m_runMutex, m_mutex;
    std::condition_variable m_ready, m_done;
    std::vector<std::thread> m_workers;
    std::function<void(size_t)> m_job;
    std::deque<std::function<void()>> m_queue;
    std::atomic<size_t> m_next{0};
    size_t m_count=0, m_generation=0, m_remaining=0;
    bool m_stop=false;
    using Clock=std::chrono::steady_clock;
    std::atomic<uint64_t> m_workerNs{0},m_callerNs{0},m_waitNs{0},m_jobs{0},m_workerTasks{0};
    void Drain(bool worker=false)
    {
        const auto start=Clock::now(); uint64_t tasks=0;
        for(size_t index=m_next.fetch_add(1);index<m_count;index=m_next.fetch_add(1))
        { m_job(index); ++tasks; }
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
        if(worker) { m_workerNs+=ns; m_workerTasks+=tasks; } else m_callerNs+=ns;
    }
public:
    struct Statistics { uint64_t workerNs,callerNs,waitNs,jobs,workerTasks; };
    Statistics ReadStatistics() const { return {m_workerNs.load(),m_callerNs.load(),m_waitNs.load(),m_jobs.load(),m_workerTasks.load()}; }
    RenderJobs()
    {
        const unsigned count=std::min(3u,std::max(1u,std::thread::hardware_concurrency())-1);
        for(unsigned i=0;i<count;++i) m_workers.emplace_back([this] {
            size_t generation=0;
            for(;;)
            {
                std::unique_lock lock(m_mutex);
                m_ready.wait(lock,[&] { return m_stop || generation!=m_generation || !m_queue.empty(); });
                if(m_stop) return;
                if(!m_queue.empty())
                {
                    auto job=std::move(m_queue.front()); m_queue.pop_front();
                    lock.unlock(); const auto started=Clock::now(); job();
                    m_workerNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-started).count();
                    ++m_workerTasks; continue;
                }
                generation=m_generation;
                lock.unlock(); Drain(true); lock.lock();
                if(--m_remaining==0) m_done.notify_one();
            }
        });
    }
    ~RenderJobs()
    {
        { std::lock_guard lock(m_mutex); m_stop=true; }
        m_ready.notify_all();
        for(auto& worker:m_workers) worker.join();
    }
    template<class F> auto Submit(F&& function)
    {
        using Result=std::invoke_result_t<F>;
        auto task=std::make_shared<std::packaged_task<Result()>>(std::forward<F>(function));
        auto result=task->get_future();
        if(m_workers.empty()) { (*task)(); return result; }
        { std::lock_guard lock(m_mutex); m_queue.emplace_back([task] { (*task)(); }); ++m_jobs; }
        m_ready.notify_one(); return result;
    }
    template<class F> void For(size_t count,size_t threshold,F&& function)
    {
        if(count<threshold || m_workers.empty())
        { for(size_t i=0;i<count;++i) function(i); return; }
        ++m_jobs;
        std::lock_guard runLock(m_runMutex);
        {
            std::lock_guard lock(m_mutex);
            m_job=std::forward<F>(function); m_count=count; m_next=0;
            m_remaining=m_workers.size(); ++m_generation;
        }
        m_ready.notify_all(); Drain();
        const auto waitStart=Clock::now();
        std::unique_lock lock(m_mutex);
        m_done.wait(lock,[&] { return m_remaining==0; });
        m_waitNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-waitStart).count();
        m_job={};
    }
    static RenderJobs& Get() { static RenderJobs jobs; return jobs; }
};
}
