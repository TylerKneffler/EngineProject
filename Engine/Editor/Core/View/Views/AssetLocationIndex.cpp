#include "AssetLocationIndex.h"
#include "Core/Serialization/Json.h"
#include "Core/AssetRecord.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;
namespace Engine::Editor
{
namespace
{
constexpr const char* IndexName = ".assetlocations.json";

std::string FileIdentity(const fs::path& path, bool directory)
{
    HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ |
        FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(handle, &info) != 0;
    CloseHandle(handle);
    if (!ok) return {};
    std::ostringstream id;
    id << std::hex << info.dwVolumeSerialNumber << ':'
        << info.nFileIndexHigh << ':' << info.nFileIndexLow;
    return "file:" + id.str();
}

bool Under(const std::string& path, const std::string& prefix)
{
    return path.size() > prefix.size() &&
        path.compare(0, prefix.size(), prefix) == 0 &&
        path[prefix.size()] == '/';
}
}

void AssetLocationIndex::Open(const fs::path& assetsRoot)
{
    std::error_code error;
    m_root = fs::weakly_canonical(assetsRoot, error);
    m_entries.clear();
    m_scanned.clear();
    if (error || !fs::is_directory(m_root)) return;
    try
    {
        const auto file = m_root / IndexName;
        if (fs::exists(file))
        {
            const auto json = Engine::Serialization::JsonParseFile(file.string());
            if (json["version"].IsNumber() &&
                json["version"].AsInt() == 1 && json["entries"].IsArray())
                for (size_t i = 0; i < json["entries"].ArraySize(); ++i)
                {
                    const auto& item = json["entries"].ArrayAt(i);
                    if (item["id"].IsString() && item["path"].IsString() &&
                        item["directory"].IsBool())
                        m_entries[item["id"].AsString()] = {
                            item["path"].AsString(), item["directory"].AsBool() };
                }
        }
    }
    catch (...) { m_entries.clear(); }
    if (m_entries.empty() && Scan(m_entries)) Save();
}

bool AssetLocationIndex::Scan(std::unordered_map<std::string, Entry>& entries) const
{
    entries.clear();
    if (m_root.empty()) return false;
    std::error_code error;
    fs::recursive_directory_iterator it(m_root,
        fs::directory_options::skip_permission_denied, error), end;
    std::unordered_set<std::string> ambiguousIds;
    if (error) return false;
    for (; it != end; it.increment(error))
    {
        if (error) return false;
        const auto& item = *it;
        if (item.is_symlink(error)) { it.disable_recursion_pending(); continue; }
        if (error) return false;
        const bool directory = item.is_directory(error);
        if (error) return false;
        if (!directory && !item.is_regular_file(error)) continue;
        if (error) return false;
        const auto name = item.path().filename().string();
        if (!name.empty() && name.front() == '.')
        {
            if (directory) it.disable_recursion_pending();
            continue;
        }
        if (!directory && item.path().extension() == ".meta") continue;
        const std::string id = FileIdentity(item.path(), directory);
        if (id.empty()) return false;
        const auto relative = item.path().lexically_relative(m_root).generic_string();
        const auto addIdentity = [&](const std::string& identity)
        {
            // Duplicate sidecars and hard links cannot identify a unique move.
            if (ambiguousIds.find(identity) == ambiguousIds.end() &&
                !entries.emplace(identity, Entry{ relative, directory }).second)
            {
                entries.erase(identity);
                ambiguousIds.insert(identity);
            }
        };
        addIdentity(id);
        if (!directory)
        {
            try
            {
                if (const auto record = Engine::Core::AssetRecord::Load(item.path()))
                    addIdentity("asset:" + record->id);
            }
            catch (...) { /* An invalid sidecar cannot hide a valid file. */ }
        }
    }
    return !error;
}

std::vector<AssetLocationIndex::Move> AssetLocationIndex::Detect()
{
    std::unordered_map<std::string, Entry> current;
    if (!Scan(current)) return {};
    std::vector<Move> moves;
    for (const auto& [id, now] : current)
    {
        const auto prior = m_entries.find(id);
        if (prior == m_entries.end() || prior->second.path == now.path ||
            prior->second.directory != now.directory)
            continue;
        moves.push_back({ prior->second.path, now.path, now.directory });
    }
    std::sort(moves.begin(), moves.end(), [](const Move& a, const Move& b)
    {
        if (a.directory != b.directory) return a.directory;
        if (a.oldPath.size() != b.oldPath.size())
            return a.oldPath.size() < b.oldPath.size();
        if (a.oldPath != b.oldPath) return a.oldPath < b.oldPath;
        return a.newPath < b.newPath;
    });
    moves.erase(std::unique(moves.begin(), moves.end(),
        [](const Move& a, const Move& b)
        {
            return a.oldPath == b.oldPath && a.newPath == b.newPath &&
                a.directory == b.directory;
        }), moves.end());
    std::vector<Move> distinct;
    for (const auto& move : moves)
    {
        bool covered = false;
        for (const auto& parent : distinct)
            if (parent.directory && Under(move.oldPath, parent.oldPath) &&
                move.newPath == parent.newPath +
                    move.oldPath.substr(parent.oldPath.size()))
            { covered = true; break; }
        if (!covered) distinct.push_back(move);
    }
    m_scanned = std::move(current);
    if (distinct.empty()) Accept();
    return distinct;
}

void AssetLocationIndex::Accept()
{
    if (m_scanned.empty()) return;
    bool changed = m_entries.size() != m_scanned.size();
    if (!changed)
        for (const auto& [id, entry] : m_scanned)
        {
            const auto previous = m_entries.find(id);
            if (previous == m_entries.end() ||
                previous->second.path != entry.path ||
                previous->second.directory != entry.directory)
            { changed = true; break; }
        }
    if (!changed) { m_scanned.clear(); return; }
    m_entries = std::move(m_scanned);
    Save();
}

void AssetLocationIndex::RecordKnownMove(const fs::path& oldPath,
    const fs::path& newPath)
{
    std::error_code error;
    const fs::path oldAbsolute = fs::absolute(oldPath, error).lexically_normal();
    if (error) return;
    const fs::path newAbsolute = fs::absolute(newPath, error).lexically_normal();
    if (error) return;
    const std::string oldRelative =
        oldAbsolute.lexically_relative(m_root).generic_string();
    const std::string newRelative =
        newAbsolute.lexically_relative(m_root).generic_string();
    if (oldRelative.empty() || newRelative.empty() ||
        oldRelative[0] == '.' || newRelative[0] == '.') return;
    bool changed = false;
    for (auto& [id, entry] : m_entries)
        if (entry.path == oldRelative || Under(entry.path, oldRelative))
        {
            entry.path = newRelative + entry.path.substr(oldRelative.size());
            changed = true;
        }
    if (changed) Save();
}

bool AssetLocationIndex::Save() const
{
    if (m_root.empty()) return false;
    auto root = Engine::Serialization::JsonValue::MakeObject();
    root.Set("version", Engine::Serialization::JsonValue(1));
    auto entries = Engine::Serialization::JsonValue::MakeArray();
    for (const auto& [id, entry] : m_entries)
    {
        auto item = Engine::Serialization::JsonValue::MakeObject();
        item.Set("id", Engine::Serialization::JsonValue(id));
        item.Set("path", Engine::Serialization::JsonValue(entry.path));
        item.Set("directory", Engine::Serialization::JsonValue(entry.directory));
        entries.Push(std::move(item));
    }
    root.Set("entries", std::move(entries));
    const fs::path destination = m_root / IndexName;
    const fs::path temporary = m_root / ".assetlocations.json.tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << Engine::Serialization::JsonWrite(root) << '\n';
        if (!output.good()) return false;
    }
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    std::error_code error;
    fs::remove(temporary, error);
    return false;
}
}
