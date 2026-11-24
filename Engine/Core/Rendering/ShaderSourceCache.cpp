#include "ShaderSourceCache.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <map>
#include <set>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Engine::Rendering
{
struct ShaderSourceCache::State
{
    using Clock=std::chrono::steady_clock;
    struct Entry
    {
        SearchPaths candidates;
        std::string requested;
        Source source;
        std::filesystem::file_time_type modified{};
        uintmax_t size=0;
        bool dirty=true, force=false;
    };
    std::map<std::pair<std::string,std::string>,Entry> entries;
    Statistics stats;
    int64_t generation=1;
    Clock::time_point fallbackAfter{};
    bool incompleteWatches=false;
#if defined(_WIN32)
    struct Watch
    {
        HANDLE directory=INVALID_HANDLE_VALUE,event=nullptr;
        OVERLAPPED overlapped{};
        alignas(DWORD) std::array<unsigned char,65536> buffer{};
        bool reading=false;
        std::set<std::wstring> candidates;
        void AddCandidate(const std::filesystem::path& path,const std::filesystem::path& root)
        {
            auto relative=path.lexically_relative(root);
            while(!relative.empty() && relative!=L".")
            {
                auto name=relative.generic_wstring();
                std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return wchar_t(std::towlower(c));});
                candidates.insert(std::move(name));
                relative=relative.parent_path();
            }
        }
        ~Watch()
        {
            if(directory!=INVALID_HANDLE_VALUE)
            {
                if(reading) { CancelIoEx(directory,&overlapped); DWORD bytes=0; GetOverlappedResult(directory,&overlapped,&bytes,TRUE); }
                CloseHandle(directory);
            }
            if(event) CloseHandle(event);
        }
        bool Arm()
        {
            ResetEvent(event); overlapped={}; overlapped.hEvent=event;
            reading=ReadDirectoryChangesW(directory,buffer.data(),DWORD(buffer.size()),TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|
                FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_SIZE,
                nullptr,&overlapped,nullptr)!=FALSE;
            return reading;
        }
        bool Start(const std::filesystem::path& path)
        {
            directory=CreateFileW(path.c_str(),FILE_LIST_DIRECTORY,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,nullptr);
            if(directory==INVALID_HANDLE_VALUE) return false;
            event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            return event && Arm();
        }
        // True when a watched source may have moved (recheck its file); sets
        // `content` when shader text itself changed (a source or an include,
        // which no entry's own time or size shows) or notifications were lost.
        // A watch can cover a large tree (the nearest existing ancestor of a
        // missing search path, up to the project root): files written there
        // (captures, caches, logs) and the directories holding them only
        // trigger the recheck, which keeps a source's revision unless its own
        // file changed. Bumping every revision on them recompiled every
        // material shader and dropped those draws (foliage) until it was done.
        bool Changed(bool& content)
        {
            if(!reading || WaitForSingleObject(event,0)!=WAIT_OBJECT_0) return false;
            DWORD bytes=0;
            const bool complete=GetOverlappedResult(directory,&overlapped,&bytes,FALSE)!=FALSE;
            reading=false;
            // Lost notifications conservatively invalidate every source.
            bool changed=!complete || !bytes;
            content |= changed;
            for(DWORD offset=0;complete && offset<bytes;)
            {
                const auto* info=reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data()+offset);
                const std::filesystem::path path(std::wstring(info->FileName,info->FileNameLength/sizeof(wchar_t)));
                auto name=path.generic_wstring();
                std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return wchar_t(std::towlower(c));});
                auto ext=path.extension().wstring();
                std::transform(ext.begin(),ext.end(),ext.begin(),[](wchar_t c){return wchar_t(std::towlower(c));});
                // Shader includes share the notification generation with their
                // entry-point files. No extension: a directory created or
                // renamed, which can make a missing candidate appear.
                const bool shaderText=ext==L".hlsl" || ext==L".hlsli" || ext==L".h" || ext==L".inc" || ext==L".fx" || ext==L".fxh";
                changed |= candidates.count(name)!=0 || ext.empty() || shaderText;
                content |= shaderText;
                if(!info->NextEntryOffset) break;
                offset+=info->NextEntryOffset;
            }
            if(!Arm()) { changed=true; content=true; }
            return changed;
        }
    };
    std::map<std::filesystem::path,std::unique_ptr<Watch>> watches;
#endif
    void WatchCandidates(const SearchPaths& candidates)
    {
#if defined(_WIN32)
        // A source that resolves watches only real directories: the missing
        // fallback locations of a found shader (a path relative to the
        // working directory, say) would otherwise watch the nearest existing
        // ancestor, often the project root, and every file written anywhere
        // in it (captures, caches, logs) would wake the watch; their floods
        // overflow it and read as lost notifications, which force every
        // material shader to recompile.
        bool anyExists=false;
        for(const auto& candidate:candidates)
        { std::error_code error; anyExists=anyExists || std::filesystem::is_regular_file(candidate,error); }
        for(const auto& candidate:candidates)
        {
            std::error_code error;
            const auto absolute=std::filesystem::absolute(candidate,error).lexically_normal();
            auto directory=absolute.parent_path();
            if(error) { incompleteWatches=true; continue; }
            if(anyExists && !std::filesystem::is_directory(directory,error)) continue;
            // No candidate exists yet: watch the nearest existing ancestor,
            // including when every shader subdirectory is missing. Creating
            // the directories then wakes us.
            while(!directory.empty() && !std::filesystem::is_directory(directory,error))
            { error.clear(); const auto parent=directory.parent_path(); if(parent==directory) break; directory=parent; }
            if(directory.empty()) { incompleteWatches=true; continue; }
            auto found=watches.find(directory);
            if(found!=watches.end() && found->second->reading)
            { found->second->AddCandidate(absolute,directory); continue; }
            auto watch=std::make_unique<Watch>();
            if(watch->Start(directory))
            { watch->AddCandidate(absolute,directory); watches[directory]=std::move(watch); }
            else incompleteWatches=true;
        }
#else
        (void)candidates;
#endif
    }
    void Refresh(Entry& entry)
    {
        ++stats.resolutions;
        Source source;
        source.path=std::filesystem::path(entry.requested).lexically_normal().generic_string();
        std::filesystem::file_time_type modified{};
        uintmax_t size=0;
        for(const auto& candidate:entry.candidates)
        {
            std::error_code error;
            ++stats.metadataChecks;
            if(!std::filesystem::is_regular_file(candidate,error)) continue;
            source.path=candidate.lexically_normal().generic_string(); source.exists=true;
            modified=std::filesystem::last_write_time(candidate,error);
            error.clear(); size=std::filesystem::file_size(candidate,error);
            break;
        }
        auto extension=std::filesystem::path(source.path).extension().string();
        std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return char(std::tolower(c));});
        source.hlsl=extension==".hlsl";
        const bool changed=entry.force || !entry.source.revision || source.path!=entry.source.path ||
            source.exists!=entry.source.exists || modified!=entry.modified || size!=entry.size;
        source.revision=changed ? generation : entry.source.revision;
        entry.source=std::move(source); entry.modified=modified; entry.size=size;
        entry.dirty=entry.force=false;
    }
};
ShaderSourceCache::ShaderSourceCache():m_state(std::make_unique<State>()) {}
ShaderSourceCache::~ShaderSourceCache()=default;
void ShaderSourceCache::Poll()
{
    bool changed=false, content=false, fallback=m_state->incompleteWatches;
#if defined(_WIN32)
    for(auto it=m_state->watches.begin();it!=m_state->watches.end();)
    {
        changed=it->second->Changed(content) || changed;
        if(!it->second->reading)
        { fallback=m_state->incompleteWatches=true; it=m_state->watches.erase(it); }
        else ++it;
    }
    // A failed initial watch must also be retried.
    fallback |= m_state->watches.empty() && !m_state->entries.empty();
#else
    fallback=true;
#endif
    if(changed) ++m_state->stats.notifications;
    const auto now=State::Clock::now();
    const bool retry=fallback && now>=m_state->fallbackAfter;
    if(retry) { m_state->fallbackAfter=now+std::chrono::seconds(1); m_state->incompleteWatches=false; }
    if(changed || retry) ++m_state->generation;
    if(changed || retry)
        for(auto& pair:m_state->entries)
        {
            // Only changed shader text forces a new revision; anything else is
            // a recheck of the entry's own file (time, size, existence).
            pair.second.dirty=true; pair.second.force |= content;
            if(retry) m_state->WatchCandidates(pair.second.candidates);
        }
}
const ShaderSourceCache::Source& ShaderSourceCache::Resolve(const std::string& location,
    const std::string& requested,const std::function<SearchPaths()>& searchPaths)
{
    auto [it,inserted]=m_state->entries.try_emplace({location,requested});
    auto& entry=it->second;
    if(inserted)
    {
        entry.candidates=searchPaths();
        entry.requested=requested;
        m_state->WatchCandidates(entry.candidates);
    }
    if(entry.dirty) m_state->Refresh(entry);
    return entry.source;
}
const ShaderSourceCache::Source& ShaderSourceCache::Resolve(const std::string& path)
{ return Resolve({},path,[&]{return SearchPaths{path};}); }
int64_t ShaderSourceCache::Generation() const { return m_state->generation; }
ShaderSourceCache::Statistics ShaderSourceCache::GetStatistics() const { return m_state->stats; }
}
