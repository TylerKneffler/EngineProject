#include "ScriptFileWatcher.h"
#include "pch.h"
#include <algorithm>
#include <array>
#include <cwctype>

namespace Engine::Editor
{
namespace
{
constexpr auto kFallbackPollInterval = std::chrono::seconds(1);
constexpr auto kChangeDebounce = std::chrono::milliseconds(150);

bool IsScriptPath(const std::wstring& path)
{
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return extension == L".cpp" || extension == L".c" ||
        extension == L".cc" || extension == L".cxx" ||
        extension == L".h" || extension == L".hpp" ||
        extension == L".inl";
}
}

struct ScriptFileWatcher::NotificationState
{
    static constexpr DWORD BufferSize = 64 * 1024;

    HANDLE directory = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    alignas(DWORD) std::array<std::byte, BufferSize> buffer{};
    bool readPending = false;

    ~NotificationState()
    {
        if (directory != INVALID_HANDLE_VALUE)
        {
            if (readPending)
                CancelIoEx(directory, &overlapped);
            CloseHandle(directory);
        }
        if (event)
            CloseHandle(event);
    }

    bool Start(const std::filesystem::path& path)
    {
        directory = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory == INVALID_HANDLE_VALUE)
            return false;
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event)
            return false;
        return Arm();
    }

    bool Arm()
    {
        if (directory == INVALID_HANDLE_VALUE || !event)
            return false;
        ResetEvent(event);
        overlapped = {};
        overlapped.hEvent = event;
        readPending = ReadDirectoryChangesW(directory, buffer.data(),
            static_cast<DWORD>(buffer.size()), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
            nullptr, &overlapped, nullptr) != FALSE;
        return readPending;
    }
};

ScriptFileWatcher::ScriptFileWatcher() = default;
ScriptFileWatcher::~ScriptFileWatcher() = default;

void ScriptFileWatcher::Initialize(std::string directory)
{
    m_directory = std::filesystem::path(std::move(directory));
    m_files = Scan();
    m_notifications = std::make_unique<NotificationState>();
    if (!m_notifications->Start(m_directory))
        m_notifications.reset();
    m_nextFallbackScan = std::chrono::steady_clock::now() +
        kFallbackPollInterval;
    m_debouncedChangePending = false;
}

std::unordered_map<std::string, ScriptFileWatcher::Stamp>
ScriptFileWatcher::Scan() const
{
    std::unordered_map<std::string, Stamp> files;
    std::error_code error;
    if (m_directory.empty() || !std::filesystem::exists(m_directory, error))
        return files;
    for (std::filesystem::recursive_directory_iterator iterator(m_directory, error), end;
         iterator != end && !error; iterator.increment(error))
    {
        if (!iterator->is_regular_file(error)) continue;
        std::string extension = iterator->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension != ".cpp" && extension != ".c" && extension != ".cc" &&
            extension != ".cxx" && extension != ".h" && extension != ".hpp" &&
            extension != ".inl")
            continue;
        files.emplace(iterator->path().lexically_normal().string(),
            iterator->last_write_time(error));
    }
    return files;
}

bool ScriptFileWatcher::Poll()
{
    const auto now = std::chrono::steady_clock::now();
    if (!m_notifications)
        return PollFallback(now);

    bool relevantChange = false;
    if (WaitForSingleObject(m_notifications->event, 0) == WAIT_OBJECT_0)
    {
        DWORD bytes = 0;
        const BOOL completed = GetOverlappedResult(m_notifications->directory,
            &m_notifications->overlapped, &bytes, FALSE);
        m_notifications->readPending = false;

        if (!completed)
        {
            relevantChange = GetLastError() != ERROR_OPERATION_ABORTED;
        }
        else if (bytes == 0)
        {
            // A zero-byte completion means the kernel notification buffer
            // overflowed. Treat it as a change rather than risking a missed
            // script edit; the build system will determine what is stale.
            relevantChange = true;
        }
        else
        {
            DWORD offset = 0;
            while (offset < bytes)
            {
                const auto* information = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                    m_notifications->buffer.data() + offset);
                const std::wstring path(information->FileName,
                    information->FileNameLength / sizeof(wchar_t));
                relevantChange = relevantChange || IsScriptPath(path);
                if (information->NextEntryOffset == 0)
                    break;
                offset += information->NextEntryOffset;
            }
        }

        if (!m_notifications->Arm())
        {
            m_notifications.reset();
            m_nextFallbackScan = now;
        }
    }

    if (relevantChange)
    {
        m_lastRelevantChange = now;
        m_debouncedChangePending = true;
    }
    if (m_debouncedChangePending &&
        now - m_lastRelevantChange >= kChangeDebounce)
    {
        m_debouncedChangePending = false;
        // Keep the fallback baseline current without scanning during idle
        // frames. This scan runs only after a real source notification.
        m_files = Scan();
        return true;
    }
    return false;
}

bool ScriptFileWatcher::PollFallback(
    std::chrono::steady_clock::time_point now)
{
    if (now < m_nextFallbackScan)
        return false;
    m_nextFallbackScan = now + kFallbackPollInterval;
    auto current = Scan();
    if (current == m_files)
        return false;
    m_files = std::move(current);
    return true;
}
}
