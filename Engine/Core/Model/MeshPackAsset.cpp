#include "MeshPackAsset.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace Engine::Model::MeshPackAsset
{
namespace fs = std::filesystem;
namespace
{
constexpr uint32_t Magic = 0x4b504d45; // EMPK
constexpr uint32_t Version = 1;
struct Entry { std::string name; uint64_t offset = 0, size = 0; fs::path source; };

template<class T> bool Read(std::istream& in, T& value)
{
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value)));
}
template<class T> void Write(std::ostream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
bool SafeName(const fs::path& name)
{
    if (name.empty() || name.is_absolute() || name.has_root_name()) return false;
    for (const auto& part : name)
        if (part == ".." || part == ".") return false;
    return true;
}
bool Index(std::ifstream& in, std::vector<Entry>& entries)
{
    uint32_t magic = 0, version = 0, count = 0;
    if (!Read(in, magic) || !Read(in, version) || !Read(in, count) ||
        magic != Magic || version != Version || count > 100000) return false;
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end < 0) return false;
    in.seekg(12);
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t length = 0;
        Entry entry;
        if (!Read(in, length) || !length || length > 4096) return false;
        entry.name.resize(length);
        if (!in.read(entry.name.data(), length) ||
            !Read(in, entry.offset) || !Read(in, entry.size) ||
            !SafeName(fs::path(entry.name)) ||
            entry.offset > static_cast<uint64_t>(end) ||
            entry.size > static_cast<uint64_t>(end) - entry.offset) return false;
        if (std::any_of(entries.begin(), entries.end(), [&](const Entry& other)
            { return other.name == entry.name; })) return false;
        entries.push_back(std::move(entry));
    }
    return true;
}
bool Copy(std::istream& source, std::ostream& dest, uint64_t bytes)
{
    std::array<char, 65536> buffer;
    while (bytes)
    {
        const auto chunk = static_cast<std::streamsize>(
            std::min<uint64_t>(bytes, buffer.size()));
        if (!source.read(buffer.data(), chunk)) return false;
        if (!dest.write(buffer.data(), chunk)) return false;
        bytes -= static_cast<uint64_t>(chunk);
    }
    return true;
}
fs::path PackPath(const fs::path& rig)
{
    auto result = rig;
    result.replace_extension(".meshpack");
    return result;
}
}

std::unique_ptr<std::ifstream> Open(const fs::path& asset)
{
    auto direct = std::make_unique<std::ifstream>(asset, std::ios::binary);
    if (direct->is_open()) return direct;
    const auto absolute = fs::absolute(asset).lexically_normal();
    for (auto folder = absolute.parent_path(); !folder.empty(); folder = folder.parent_path())
    {
        std::error_code ec;
        for (const auto& item : fs::directory_iterator(folder, ec))
        {
            if (ec) break;
            if (item.path().extension() != ".meshpack") continue;
            auto stream = std::make_unique<std::ifstream>(item.path(), std::ios::binary);
            std::vector<Entry> entries;
            if (!Index(*stream, entries)) continue;
            const auto name = absolute.lexically_relative(folder).generic_string();
            auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e)
                { return e.name == name; });
            if (found != entries.end())
            {
                stream->seekg(static_cast<std::streamoff>(found->offset));
                return stream;
            }
        }
        if (folder == folder.root_path()) break;
    }
    return {};
}

bool PackRig(const fs::path& rig, std::string& error)
{
    error.clear();
    if (rig.extension() != ".rig" || !fs::is_regular_file(rig))
    { error = "Select a rig file."; return false; }
    const auto root = rig.parent_path();
    const auto meshes = root / "Meshes";
    const auto pack = PackPath(rig);
    if (fs::exists(pack))
    { error = "Mesh pack already exists."; return false; }
    if (!fs::is_directory(meshes))
    { error = "No Meshes directory beside this rig."; return false; }
    std::vector<Entry> entries;
    for (const auto& item : fs::recursive_directory_iterator(meshes))
    {
        if (!item.is_regular_file()) continue;
        const auto ext = item.path().extension();
        if (ext != ".mesh" && ext != ".morph") continue;
        Entry entry;
        entry.source = item.path();
        entry.name = item.path().lexically_relative(root).generic_string();
        entry.size = item.file_size();
        if (!SafeName(fs::path(entry.name)))
        { error = "Unsafe mesh path."; return false; }
        entries.push_back(std::move(entry));
    }
    if (entries.empty())
    { error = "No mesh or morph files beside this rig."; return false; }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
        { return a.name < b.name; });
    uint64_t offset = 12;
    for (const auto& entry : entries) offset += 4 + entry.name.size() + 16;
    for (auto& entry : entries) { entry.offset = offset; offset += entry.size; }
    const auto temp = fs::path(pack.string() + ".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        Write(out, Magic); Write(out, Version);
        Write(out, static_cast<uint32_t>(entries.size()));
        for (const auto& entry : entries)
        {
            Write(out, static_cast<uint32_t>(entry.name.size()));
            out.write(entry.name.data(), entry.name.size());
            Write(out, entry.offset); Write(out, entry.size);
        }
        for (const auto& entry : entries)
        {
            std::ifstream in(entry.source, std::ios::binary);
            if (!in || !Copy(in, out, entry.size))
            { error = "Could not copy " + entry.source.string(); break; }
        }
        if (!out.good() && error.empty()) error = "Could not write mesh pack.";
    }
    if (!error.empty()) { fs::remove(temp); return false; }
    {
        std::ifstream check(temp, std::ios::binary);
        std::vector<Entry> indexed;
        if (!Index(check, indexed) || indexed.size() != entries.size())
        { fs::remove(temp); error = "Mesh pack verification failed."; return false; }
    }
    std::error_code ec;
    fs::rename(temp, pack, ec);
    if (ec) { error = ec.message(); fs::remove(temp); return false; }
    for (const auto& entry : entries)
    {
        fs::remove(entry.source, ec);
        if (ec) { error = "Pack written, but could not remove " + entry.source.string(); return false; }
        fs::remove(fs::path(entry.source.string() + ".meta"), ec);
        ec.clear();
    }
    return true;
}

bool Extract(const fs::path& pack, const fs::path& root, std::string& error)
{
    error.clear();
    std::ifstream in(pack, std::ios::binary);
    std::vector<Entry> entries;
    if (!in || !Index(in, entries))
    { error = "Not a valid mesh pack: " + pack.string(); return false; }
    for (const auto& entry : entries)
    {
        const auto target = root / fs::path(entry.name);
        if (!SafeName(fs::path(entry.name)) || fs::exists(target))
        { error = "Cannot extract over existing file: " + target.string(); return false; }
    }
    for (const auto& entry : entries)
    {
        const auto target = root / fs::path(entry.name);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (ec) { error = ec.message(); return false; }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        in.clear(); in.seekg(static_cast<std::streamoff>(entry.offset));
        if (!out || !Copy(in, out, entry.size))
        { error = "Could not extract " + target.string(); return false; }
    }
    return true;
}

bool ExtractRig(const fs::path& rig, std::string& error)
{
    const auto pack = PackPath(rig);
    if (!Extract(pack, pack.parent_path(), error)) return false;
    std::error_code ec;
    fs::remove(pack, ec);
    if (ec) { error = ec.message(); return false; }
    fs::remove(fs::path(pack.string() + ".meta"), ec);
    return true;
}
}
