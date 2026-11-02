#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace Engine::Threading
{
// CPU jobs over owned/immutable inputs. Live scene and graphics operations keep
// their owning threads. Dependencies never consume workers while waiting.
class JobScheduler
{
    using Clock=std::chrono::steady_clock;
    struct State
    {
        std::function<void()> function;
        std::promise<void> promise;
        std::shared_future<void> completion=promise.get_future().share();
        std::vector<std::shared_ptr<State>> dependents;
        size_t pending=0;
        bool complete=false;
        bool allowCaller=true;
        std::thread::id ownerThread{};
        std::exception_ptr error,dependencyError;
        Clock::time_point submitted=Clock::now(),ready=submitted;
        JobScheduler* owner=nullptr;
    };
public:
    class Handle
    {
        friend class JobScheduler;
        std::shared_ptr<State> state;
        explicit Handle(std::shared_ptr<State> value):state(std::move(value)) {}
    public:
        Handle()=default;
        bool Valid() const { return bool(state); }
    };
    struct Statistics
    {
        uint64_t workerNs=0,callerNs=0,waitNs=0,jobs=0,workerTasks=0;
        uint64_t queueNs=0,dependencyNs=0,pending=0,peakPending=0;
        unsigned workers=0;
    };
private:
    std::mutex m_mutex;
    std::condition_variable m_ready,m_done;
    std::deque<std::shared_ptr<State>> m_queue;
    std::vector<std::thread> m_workers;
    bool m_stop=false;
    std::atomic<bool> m_parallelFor{true};
    inline static thread_local JobScheduler* s_worker=nullptr;
    inline static thread_local JobScheduler* s_executing=nullptr;
    uint64_t m_pending=0;
    std::atomic<uint64_t> m_workerNs{0},m_callerNs{0},m_waitNs{0},m_jobs{0},m_workerTasks{0};
    std::atomic<uint64_t> m_queueNs{0},m_dependencyNs{0},m_peakPending{0};
    static uint64_t Ns(Clock::duration value)
    { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(value).count()); }
    void Run(const std::shared_ptr<State>& task,bool worker)
    {
        const auto started=Clock::now(); m_queueNs+=Ns(started-task->ready);
        auto* previous=s_executing; s_executing=this;
        std::exception_ptr failure;
        try { if(task->dependencyError) std::rethrow_exception(task->dependencyError); task->function(); }
        catch(...) { failure=std::current_exception(); }
        s_executing=previous;
        if(worker) ++m_workerTasks;
        if(previous!=this)
        { if(worker) m_workerNs+=Ns(Clock::now()-started); else m_callerNs+=Ns(Clock::now()-started); }
        size_t readied=0;
        {
            std::lock_guard lock(m_mutex);
            task->error=failure; task->complete=true; task->function={};
            for(auto& next:task->dependents)
            {
                if(failure && !next->dependencyError) next->dependencyError=failure;
                if(--next->pending==0)
                { next->ready=Clock::now(); m_dependencyNs+=Ns(next->ready-next->submitted); m_queue.push_back(next); ++readied; }
            }
            task->dependents.clear();
            if(failure) task->promise.set_exception(failure); else task->promise.set_value();
            --m_pending;
        }
        while(readied--) m_ready.notify_one();
        m_done.notify_all();
    }
    bool CanHelp() const
    {
        return std::any_of(m_queue.begin(),m_queue.end(),[&](const auto& task) {
            return (task->ownerThread==std::thread::id{} ||
                task->ownerThread==std::this_thread::get_id()) &&
                (task->allowCaller || s_worker==this || m_workers.empty());
        });
    }
    bool Help()
    {
        std::shared_ptr<State> task;
        { std::lock_guard lock(m_mutex);
          auto eligible=std::find_if(m_queue.begin(),m_queue.end(),[&](const auto& entry) {
              return (entry->ownerThread==std::thread::id{} ||
                  entry->ownerThread==std::this_thread::get_id()) &&
                  (entry->allowCaller || s_worker==this || m_workers.empty());
          });
          if(eligible==m_queue.end()) return false;
          task=std::move(*eligible); m_queue.erase(eligible); }
        Run(task,s_worker==this); return true;
    }
public:
    static unsigned DefaultWorkerCount()
    {
        if(const char* text=std::getenv("ENGINE_WORKER_COUNT"))
        {
            char* end=nullptr; const auto count=std::strtoul(text,&end,10);
            if(end!=text && !*end && count<=128) return unsigned(count);
        }
        const unsigned hardware=std::thread::hardware_concurrency();
        return hardware>2 ? hardware-2:1;
    }
    explicit JobScheduler(unsigned workers=DefaultWorkerCount())
    {
        try { for(unsigned index=0;index<workers;++index) m_workers.emplace_back([this] {
            s_worker=this;
            for(;;)
            {
                std::shared_ptr<State> task;
                {
                    std::unique_lock lock(m_mutex);
                    m_ready.wait(lock,[&] { return m_stop || std::any_of(
                        m_queue.begin(),m_queue.end(),[](const auto& queued) {
                            return queued->ownerThread==std::thread::id{};
                        }); });
                    auto eligible=std::find_if(m_queue.begin(),m_queue.end(),
                        [](const auto& queued) { return queued->ownerThread==std::thread::id{}; });
                    if(eligible==m_queue.end()) { if(m_stop) return; continue; }
                    task=std::move(*eligible); m_queue.erase(eligible);
                }
                Run(task,true);
            }
        }); }
        catch(...)
        {
            { std::lock_guard lock(m_mutex); m_stop=true; }
            m_ready.notify_all(); for(auto& worker:m_workers) worker.join();
            throw;
        }
    }
    JobScheduler(const JobScheduler&)=delete;
    JobScheduler& operator=(const JobScheduler&)=delete;
    ~JobScheduler()
    {
        // Stop producers before destruction; finish the submitted graph first.
        for(;;)
        {
            { std::lock_guard lock(m_mutex); if(!m_pending) { m_stop=true; break; } }
            if(!Help()) { std::unique_lock lock(m_mutex); m_done.wait(lock,[&] { return !m_pending || CanHelp(); }); }
        }
        m_ready.notify_all(); for(auto& worker:m_workers) worker.join();
    }
    unsigned WorkerCount() const { return unsigned(m_workers.size()); }
    bool IsComplete(const Handle& handle)
    {
        if(!handle.state) return true;
        if(handle.state->owner!=this)
            throw std::invalid_argument("Job handle belongs to another scheduler");
        std::lock_guard lock(m_mutex);
        return handle.state->complete;
    }
    // Change at frame boundaries for profiling. Async Submit jobs retain worker
    // affinity; disabling parallel loops never moves background IO onto the UI.
    void SetParallelForEnabled(bool enabled) { m_parallelFor=enabled; }
    Statistics ReadStatistics()
    {
        std::lock_guard lock(m_mutex);
        return {m_workerNs.load(),m_callerNs.load(),m_waitNs.load(),m_jobs.load(),m_workerTasks.load(),
            m_queueNs.load(),m_dependencyNs.load(),m_pending,m_peakPending.load(),WorkerCount()};
    }
    template<class F> Handle ScheduleImpl(F&& function,const std::vector<Handle>& dependencies,
        bool allowCaller,std::thread::id ownerThread)
    {
        auto task=std::make_shared<State>(); task->function=std::forward<F>(function); task->owner=this;
        task->allowCaller=allowCaller; task->ownerThread=ownerThread;
        {
            std::lock_guard lock(m_mutex);
            if(m_stop) throw std::runtime_error("Job scheduler is stopping");
            for(const auto& dependency:dependencies)
                if(dependency.state && dependency.state->owner!=this)
                    throw std::invalid_argument("Job dependencies must use the same scheduler");
            for(const auto& dependency:dependencies) if(dependency.state)
            {
                if(dependency.state->complete)
                { if(dependency.state->error) task->dependencyError=dependency.state->error; }
                else { ++task->pending; dependency.state->dependents.push_back(task); }
            }
            ++m_jobs; ++m_pending; m_peakPending=std::max(m_peakPending.load(),m_pending);
            if(!task->pending) { task->ready=Clock::now(); m_queue.push_back(task); }
        }
        m_ready.notify_one(); Handle result(task);
        if(m_workers.empty()) { try { Wait(result); } catch(...) {} }
        return result;
    }
    template<class F> Handle Schedule(F&& function,const std::vector<Handle>& dependencies={},bool allowCaller=true)
    { return ScheduleImpl(std::forward<F>(function),dependencies,allowCaller,{}); }
    // Jobs touching live scene or native physics are queued with their owner.
    // Workers may run their dependencies but never execute these callbacks.
    template<class F> Handle ScheduleOnOwner(F&& function,const std::vector<Handle>& dependencies={})
    { return ScheduleImpl(std::forward<F>(function),dependencies,true,std::this_thread::get_id()); }
    void Wait(const Handle& handle)
    {
        if(!handle.state) return;
        if(handle.state->owner!=this) throw std::invalid_argument("Job handle belongs to another scheduler");
        while(handle.state->completion.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
        {
            if(!Help())
            {
                const auto started=Clock::now();
                std::unique_lock lock(m_mutex); m_done.wait(lock,[&] { return handle.state->complete || CanHelp(); });
                m_waitNs+=Ns(Clock::now()-started);
            }
        }
        handle.state->completion.get();
    }
    template<class F> auto Submit(F&& function)
    {
        using Result=std::invoke_result_t<F>;
        auto task=std::make_shared<std::packaged_task<Result()>>(std::forward<F>(function));
        // Async producers (asset IO, for example) must not be stolen onto the
        // main thread by a cooperative render wait. Serial mode is explicit.
        auto result=task->get_future(); Schedule([task] { (*task)(); },{},false); return result;
    }
    template<class F> void For(size_t count,size_t minimumBatch,F&& function)
    {
        const size_t grain=std::max<size_t>(1,std::min<size_t>(minimumBatch,64));
        // SIZE_MAX is an explicit opt-out used by thread-affine render paths.
        if(!m_parallelFor.load() || minimumBatch==SIZE_MAX || count<grain*2 || m_workers.empty())
        {
            const auto started=Clock::now(); for(size_t i=0;i<count;++i) function(i);
            if(s_executing!=this)
            { if(s_worker==this) m_workerNs+=Ns(Clock::now()-started); else m_callerNs+=Ns(Clock::now()-started); }
            return;
        }
        const size_t batches=std::min<size_t>(m_workers.size()+1,(count+grain-1)/grain);
        std::vector<Handle> handles; handles.reserve(batches);
        std::exception_ptr failure;
        try { for(size_t batch=0;batch<batches;++batch)
            handles.push_back(Schedule([&,first=count*batch/batches,last=count*(batch+1)/batches] {
                for(size_t i=first;i<last;++i) function(i);
            })); }
        catch(...) { failure=std::current_exception(); }
        // Join every capture even if one batch fails. Cooperative waits allow
        // nested parallel loops to progress without exhausting the worker pool.
        for(const auto& handle:handles) try { Wait(handle); } catch(...) { if(!failure) failure=std::current_exception(); }
        if(failure) std::rethrow_exception(failure);
    }
    static JobScheduler& Get() { static JobScheduler scheduler; return scheduler; }
};
}
