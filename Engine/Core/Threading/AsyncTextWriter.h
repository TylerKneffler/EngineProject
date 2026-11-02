#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace Engine::Threading
{
// Formatting and file IO never run on producers. Jobs must capture owned values,
// not live scene pointers. Overflow is reported rather than blocking a frame.
class AsyncTextWriter
{
    struct Record { std::function<std::string()> format; size_t bytes; };
public:
    struct Statistics
    {
        uint64_t accepted=0,written=0,dropped=0,failed=0,formatNs=0,writeNs=0;
        size_t queuedBytes=0,peakQueuedBytes=0,queuedRecords=0;
    };
private:
    mutable std::mutex m_mutex;
    std::mutex m_lifecycle;
    std::condition_variable m_ready;
    std::deque<Record> m_queue;
    std::thread m_worker;
    std::ofstream m_file;
    Statistics m_statistics;
    const size_t m_capacity,m_recordLimit;
    bool m_accepting=false,m_stop=false;
    void Run()
    {
        using Clock=std::chrono::steady_clock;
        auto flushed=Clock::now(); unsigned sinceFlush=0;
        for(;;)
        {
            Record record;
            {
                std::unique_lock lock(m_mutex);
                m_ready.wait_for(lock,std::chrono::milliseconds(250),[&] { return m_stop || !m_queue.empty(); });
                if(m_queue.empty())
                {
                    if(m_stop) break;
                    lock.unlock(); m_file.flush(); flushed=Clock::now(); continue;
                }
                record=std::move(m_queue.front()); m_queue.pop_front();
                m_statistics.queuedBytes-=record.bytes; m_statistics.queuedRecords=m_queue.size();
            }
            uint64_t formatNs=0,writeNs=0; bool success=false;
            try
            {
                auto started=Clock::now(); auto line=record.format();
                formatNs=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-started).count());
                started=Clock::now(); m_file<<line<<'\n';
                if(++sinceFlush>=30 || Clock::now()-flushed>=std::chrono::milliseconds(250))
                { m_file.flush(); sinceFlush=0; flushed=Clock::now(); }
                writeNs=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-started).count());
                success=bool(m_file);
            }
            catch(...) {}
            {
                std::lock_guard lock(m_mutex);
                m_statistics.formatNs+=formatNs; m_statistics.writeNs+=writeNs;
                if(success) ++m_statistics.written; else ++m_statistics.failed;
                if(!m_file)
                {
                    m_statistics.dropped+=m_queue.size(); m_queue.clear();
                    m_statistics.queuedBytes=0; m_statistics.queuedRecords=0;
                    m_accepting=false; m_stop=true;
                }
            }
        }
        const auto stats=ReadStatistics();
        m_file<<"{\"type\":\"writer_summary\",\"accepted_records\":"<<stats.accepted
            <<",\"written_records\":"<<stats.written<<",\"dropped_records\":"<<stats.dropped
            <<",\"failed_records\":"<<stats.failed<<",\"peak_queue_bytes\":"<<stats.peakQueuedBytes
            <<",\"format_ms\":"<<stats.formatNs/1.e6<<",\"write_ms\":"<<stats.writeNs/1.e6<<"}\n";
        m_file.flush(); m_file.close();
    }
public:
    explicit AsyncTextWriter(size_t capacity=8*1024*1024,size_t recordLimit=4096)
        :m_capacity(capacity),m_recordLimit(recordLimit) {}
    ~AsyncTextWriter() { Stop(); }
    bool Start(const std::filesystem::path& path)
    {
        std::lock_guard lifecycle(m_lifecycle);
        std::lock_guard lock(m_mutex);
        if(m_worker.joinable()) return false;
        m_file.clear(); m_file.open(path,std::ios::binary|std::ios::trunc);
        if(!m_file) return false;
        m_statistics={}; m_queue.clear(); m_accepting=true; m_stop=false;
        try { m_worker=std::thread([this] { Run(); }); }
        catch(...) { m_accepting=false; m_file.close(); throw; }
        return true;
    }
    bool IsOpen() const { std::lock_guard lock(m_mutex); return m_accepting; }
    Statistics ReadStatistics() const { std::lock_guard lock(m_mutex); return m_statistics; }
    bool Enqueue(std::function<std::string()> format,size_t ownedBytes)
    {
        std::lock_guard lock(m_mutex);
        if(!m_accepting) return false;
        ownedBytes+=sizeof(Record);
        if(ownedBytes>m_capacity || m_statistics.queuedBytes>m_capacity-ownedBytes || m_queue.size()>=m_recordLimit)
        { ++m_statistics.dropped; return false; }
        m_queue.push_back({std::move(format),ownedBytes}); ++m_statistics.accepted;
        m_statistics.queuedBytes+=ownedBytes; m_statistics.queuedRecords=m_queue.size();
        if(m_statistics.queuedBytes>m_statistics.peakQueuedBytes) m_statistics.peakQueuedBytes=m_statistics.queuedBytes;
        m_ready.notify_one(); return true;
    }
    void Stop()
    {
        std::lock_guard lifecycle(m_lifecycle);
        { std::lock_guard lock(m_mutex); m_accepting=false; m_stop=true; }
        m_ready.notify_one(); if(m_worker.joinable()) m_worker.join();
    }
};
}
