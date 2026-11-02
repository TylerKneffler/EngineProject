#pragma once
#include "Core/Threading/JobScheduler.h"
#include <cctype>
#include <array>
#include <filesystem>
#include <map>
#include <string>
#if defined(_WIN32)
#include <Windows.h>
#endif

namespace Engine::IO
{
struct DirectoryEntry
{
    std::filesystem::path value;
    bool directory=false,symlink=false,meshPacked=false;
    const std::filesystem::path& path() const { return value; }
    bool is_directory() const { return directory; }
    bool is_symlink() const { return symlink; }
};
struct DirectorySnapshot
{
    std::vector<DirectoryEntry> entries;
    std::string error;
};
// The UI polls completed immutable results; filesystem enumeration, filtering,
// metadata queries and sorting run on CPU workers. Jobs never capture this cache.
class DirectorySnapshotCache
{
    using Clock=std::chrono::steady_clock;
    struct Entry
    {
        uint64_t generation=0,requested=0,lastUse=0;
        std::shared_ptr<const DirectorySnapshot> snapshot;
        std::future<std::shared_ptr<const DirectorySnapshot>> pending;
    };
    using Key=std::pair<std::filesystem::path,std::string>;
    std::map<Key,Entry> m_entries;
    std::shared_ptr<std::atomic<unsigned>> m_inFlight=std::make_shared<std::atomic<unsigned>>(0);
    uint64_t m_generation=1,m_use=0;
    Clock::time_point m_pollAfter{};
#if defined(_WIN32)
    HANDLE m_directory=INVALID_HANDLE_VALUE,m_event=nullptr;
    OVERLAPPED m_notification{};
    alignas(DWORD) std::array<uint8_t,65536> m_buffer{};
    bool m_reading=false;
    bool Arm()
    {
        ResetEvent(m_event);
        m_reading=ReadDirectoryChangesW(m_directory,m_buffer.data(),DWORD(m_buffer.size()),TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE,
            nullptr,&m_notification,nullptr)!=FALSE;
        return m_reading;
    }
    void CloseWatch()
    {
        if(m_directory!=INVALID_HANDLE_VALUE)
        {
            if(m_reading) { CancelIoEx(m_directory,&m_notification); DWORD bytes=0; GetOverlappedResult(m_directory,&m_notification,&bytes,TRUE); }
            CloseHandle(m_directory); m_directory=INVALID_HANDLE_VALUE;
        }
        if(m_event) { CloseHandle(m_event); m_event=nullptr; }
        m_reading=false;
    }
#endif
    static std::shared_ptr<const DirectorySnapshot> Read(const Key& key)
    {
        auto snapshot=std::make_shared<DirectorySnapshot>();
        try
        {
            for(const auto& source:std::filesystem::directory_iterator(key.first))
            {
                const auto name=source.path().filename().string();
                if((!name.empty() && name.front()=='.') || source.path().extension()==".meta") continue;
                std::string lower=name;
                std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c) { return char(std::tolower(c)); });
                if(!key.second.empty() && lower.find(key.second)==std::string::npos) continue;
                DirectoryEntry entry; entry.value=source.path();
                entry.directory=source.is_directory(); entry.symlink=source.is_symlink();
                if(entry.value.extension()==".rig") entry.meshPacked=std::filesystem::exists(
                    entry.value.parent_path()/(entry.value.stem().string()+".meshpack"));
                snapshot->entries.push_back(std::move(entry));
            }
            std::sort(snapshot->entries.begin(),snapshot->entries.end(),[](const auto& first,const auto& second) {
                if(first.directory!=second.directory) return first.directory;
                return first.value.filename().native()<second.value.filename().native();
            });
        }
        catch(const std::exception& error) { snapshot->error=error.what(); snapshot->entries.clear(); }
        return snapshot;
    }
public:
    DirectorySnapshotCache()=default;
    DirectorySnapshotCache(const DirectorySnapshotCache&)=delete;
    DirectorySnapshotCache& operator=(const DirectorySnapshotCache&)=delete;
    ~DirectorySnapshotCache()
    {
#if defined(_WIN32)
        CloseWatch();
#endif
    }
    void Watch(const std::filesystem::path& root)
    {
        m_entries.clear(); ++m_generation;
#if defined(_WIN32)
        CloseWatch();
        m_directory=CreateFileW(root.c_str(),FILE_LIST_DIRECTORY,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,nullptr);
        if(m_directory!=INVALID_HANDLE_VALUE)
        {
            m_event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if(m_event) { m_notification={}; m_notification.hEvent=m_event; if(!Arm()) CloseWatch(); }
            else CloseWatch();
        }
#else
        (void)root;
#endif
    }
    // Returns true when completed work or filesystem notifications changed the UI.
    bool Poll()
    {
        bool changed=false,watched=false;
#if defined(_WIN32)
        watched=m_reading;
        if(m_reading && WaitForSingleObject(m_event,0)==WAIT_OBJECT_0)
        {
            DWORD bytes=0; GetOverlappedResult(m_directory,&m_notification,&bytes,FALSE);
            m_reading=false; ++m_generation; changed=true;
            if(!Arm()) CloseWatch();
        }
#endif
        if(!watched && Clock::now()>=m_pollAfter)
        { m_pollAfter=Clock::now()+std::chrono::seconds(1); ++m_generation; changed=true; }
        for(auto& pair:m_entries)
        {
            auto& entry=pair.second;
            if(entry.pending.valid() && entry.pending.wait_for(std::chrono::seconds(0))==std::future_status::ready)
            {
                auto snapshot=entry.pending.get();
                entry.snapshot=std::move(snapshot); entry.generation=entry.requested; changed=true;
            }
        }
        return changed;
    }
    std::shared_ptr<const DirectorySnapshot> Get(const std::filesystem::path& directory,std::string query)
    {
        Key key{directory.lexically_normal(),std::move(query)};
        auto& entry=m_entries[key]; entry.lastUse=++m_use;
        if(entry.generation!=m_generation && !entry.pending.valid() && m_inFlight->load()<4)
        {
            entry.requested=m_generation; auto inFlight=m_inFlight; ++*inFlight;
            try { entry.pending=Threading::JobScheduler::Get().Submit([key,inFlight] {
                struct Finish { std::shared_ptr<std::atomic<unsigned>> counter; ~Finish() { --*counter; } } finish{inFlight};
                return Read(key);
            }); } catch(...) { --*inFlight; throw; }
        }
        // Keep snapshots bounded during navigation/search. Pending jobs retain
        // their entries until completion so no UI-thread future destructor waits.
        if(m_entries.size()>128)
        {
            auto oldest=m_entries.end();
            for(auto it=m_entries.begin();it!=m_entries.end();++it)
                if(it->first!=key && !it->second.pending.valid() && (oldest==m_entries.end() || it->second.lastUse<oldest->second.lastUse)) oldest=it;
            if(oldest!=m_entries.end()) m_entries.erase(oldest);
        }
        return entry.snapshot;
    }
};
}
