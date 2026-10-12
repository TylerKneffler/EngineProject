#include "AbrImporter.h"
#include "BrushPreset.h"
#include "PngPaintWriter.h"
#include <algorithm>
#include <fstream>
#include <iterator>

namespace Engine::Editor::Paint
{
namespace
{
struct Reader
{
    const std::vector<uint8_t>& data;
    size_t at = 0, limit;
    bool Take(size_t count)
    { if (count > limit - at) return false; at += count; return true; }
    bool U8(uint8_t& value)
    { if (!Take(1)) return false; value = data[at - 1]; return true; }
    bool U16(uint16_t& value)
    { if (!Take(2)) return false; value = (data[at - 2] << 8) | data[at - 1]; return true; }
    bool U32(uint32_t& value)
    { if (!Take(4)) return false; value = (uint32_t(data[at - 4]) << 24) |
        (uint32_t(data[at - 3]) << 16) | (data[at - 2] << 8) | data[at - 1];
        return true; }
};

bool DecodeMask(Reader& reader, uint32_t width, uint32_t height,
    uint16_t depth, uint8_t compression, std::vector<uint8_t>& mask)
{
    if (!width || !height || width > 4096 || height > 4096 ||
        uint64_t(width) * height > 16000000 || depth != 8 ||
        compression > 1) return false;
    mask.resize(size_t(width) * height);
    if (compression == 0)
    {
        if (!reader.Take(mask.size())) return false;
        std::copy_n(reader.data.begin() + reader.at - mask.size(),
            mask.size(), mask.begin());
        return true;
    }
    std::vector<uint16_t> lengths(height);
    for (auto& length : lengths) if (!reader.U16(length)) return false;
    for (uint32_t y = 0; y < height; ++y)
    {
        if (lengths[y] > reader.limit - reader.at) return false;
        const size_t end = reader.at + lengths[y];
        uint32_t x = 0;
        while (reader.at < end)
        {
            uint8_t code;
            if (!reader.U8(code)) return false;
            const int count = code <= 127 ? code + 1 :
                code == 128 ? 0 : 257 - code;
            if (count > int(width - x)) return false;
            if (code <= 127)
            {
                if (size_t(count) > end - reader.at) return false;
                for (int i = 0; i < count; ++i)
                    mask[size_t(y) * width + x++] = reader.data[reader.at++];
            }
            else if (code != 128)
            {
                if (reader.at >= end) return false;
                const uint8_t value = reader.data[reader.at++];
                for (int i = 0; i < count; ++i)
                    mask[size_t(y) * width + x++] = value;
            }
        }
        if (x != width) return false;
    }
    return true;
}

bool ParseSample(Reader& r, AbrTip& tip, bool old, bool named)
{
    uint32_t ignored, top, left, bottom, right;
    uint16_t spacing = 25, shortBound, depth;
    uint8_t flag, compression;
    if (old)
    {
        if (!r.U32(ignored) || !r.U16(spacing)) return false;
        if (named)
        {
            uint32_t count;
            if (!r.U32(count) || count > 1024 ||
                count > (r.limit - r.at) / 2) return false;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint16_t character;
                if (!r.U16(character)) return false;
                if (character >= 32 && character < 127)
                    tip.name.push_back(char(character));
            }
        }
        if (!r.U8(flag)) return false;
        for (int i = 0; i < 4; ++i) if (!r.U16(shortBound)) return false;
    }
    if (!r.U32(top) || !r.U32(left) || !r.U32(bottom) ||
        !r.U32(right) || !r.U16(depth) || !r.U8(compression) ||
        bottom <= top || right <= left) return false;
    tip.width = right - left; tip.height = bottom - top;
    tip.spacing = std::clamp(spacing / 100.f, .01f, 1.f);
    return DecodeMask(r, tip.width, tip.height, depth, compression, tip.mask);
}

std::filesystem::path UniquePath(std::filesystem::path path)
{
    const auto original = path;
    for (int suffix = 2; std::filesystem::exists(path); ++suffix)
        path = original.parent_path() / (original.stem().string() + " " +
            std::to_string(suffix) + original.extension().string());
    return path;
}
}

bool ReadAbrTips(const std::vector<uint8_t>& bytes,
    std::vector<AbrTip>& tips, std::string& notes)
{
    tips.clear(); notes.clear();
    Reader r{bytes, 0, bytes.size()};
    uint16_t version, count;
    if (!r.U16(version) || !r.U16(count))
    { notes = "Truncated ABR header."; return false; }
    int skipped = 0;
    if (version == 1 || version == 2)
    {
        for (uint16_t i = 0; i < count; ++i)
        {
            uint16_t type; uint32_t size;
            if (!r.U16(type) || !r.U32(size) || size > r.limit - r.at)
            { notes = "Truncated ABR brush record."; return false; }
            const size_t end = r.at + size;
            if (type == 2)
            {
                Reader record{bytes, r.at, end}; AbrTip tip;
                if (ParseSample(record, tip, true, version == 2))
                {
                    if (tip.name.empty()) tip.name = "Brush " + std::to_string(i + 1);
                    tips.push_back(std::move(tip));
                }
                else ++skipped;
            }
            else ++skipped;
            r.at = end;
        }
    }
    else if ((version == 6 || version == 10) && (count == 1 || count == 2))
    {
        bool found = false;
        while (r.limit - r.at >= 12)
        {
            const size_t start = r.at;
            if (bytes[start] == '8' && bytes[start + 1] == 'B' &&
                bytes[start + 2] == 'I' && bytes[start + 3] == 'M' &&
                bytes[start + 4] == 's' && bytes[start + 5] == 'a' &&
                bytes[start + 6] == 'm' && bytes[start + 7] == 'p')
            { r.at += 8; found = true; break; }
            ++r.at;
        }
        if (!found)
        { notes = "ABR has no sampled-tip section."; return false; }
        uint32_t sectionSize;
        if (!r.U32(sectionSize) || sectionSize > r.limit - r.at)
        { notes = "Truncated ABR sample section."; return false; }
        const size_t sectionEnd = r.at + sectionSize;
        while (r.at + 4 <= sectionEnd)
        {
            uint32_t size;
            if (!r.U32(size) || !size || size > sectionEnd - r.at)
            { notes = "Invalid ABR sample record."; return false; }
            const size_t recordStart = r.at;
            const size_t end = recordStart + size;
            Reader record{bytes, recordStart, end}; AbrTip tip;
            tip.name = "Brush " + std::to_string(tips.size() + skipped + 1);
            if (record.Take(count == 1 ? 47 : 301) &&
                ParseSample(record, tip, false, false))
                tips.push_back(std::move(tip));
            else ++skipped;
            r.at = recordStart + ((size + 3u) & ~size_t(3u));
            if (r.at > sectionEnd) break;
        }
    }
    else
    { notes = "Unsupported ABR version or subversion: " +
        std::to_string(version) + "/" + std::to_string(count) + ".";
      return false; }
    notes = std::to_string(tips.size()) + " sampled tips decoded; " +
        std::to_string(skipped) + " unsupported or invalid brushes skipped. "
        "Photoshop dynamics, blend modes, and v6/v10 descriptor settings "
        "are not imported; review each preset before painting.";
    return !tips.empty();
}

bool ImportAbrPack(const std::filesystem::path& source,
    const std::filesystem::path& assetsDirectory, std::string& report)
{
    std::ifstream input(source, std::ios::binary);
    if (!input) { report = "Could not open ABR pack."; return false; }
    const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>{input}, {});
    std::vector<AbrTip> tips;
    if (!ReadAbrTips(bytes, tips, report)) return false;
    const auto directory = assetsDirectory / "Brushes";
    const auto tipDirectory = directory / "Tips";
    const auto sourceDirectory = directory / "Sources";
    std::error_code error;
    std::filesystem::create_directories(tipDirectory, error);
    if (!error) std::filesystem::create_directories(sourceDirectory, error);
    if (error) { report = error.message(); return false; }
    const auto sourceCopy = UniquePath(sourceDirectory / source.filename());
    std::filesystem::copy_file(source, sourceCopy, error);
    if (error) { report = error.message(); return false; }
    int imported = 0;
    for (const AbrTip& tip : tips)
    {
        std::string name = source.stem().string() + " " + tip.name;
        for (char& c : name)
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        const auto png = UniquePath(tipDirectory / (name + ".png"));
        std::vector<uint8_t> rgba(size_t(tip.width) * tip.height * 4, 0);
        for (size_t i = 0; i < tip.mask.size(); ++i)
            rgba[i * 4 + 3] = tip.mask[i];
        std::string errorText;
        if (!SavePng(png, tip.width, tip.height, rgba, errorText))
        { report += " Failed tip: " + name + " (" + errorText + ")."; continue; }
        BrushPreset preset;
        preset.name = name;
        std::error_code relativeError;
        auto relative = std::filesystem::relative(png,
            std::filesystem::current_path(), relativeError);
        preset.tipPath = relativeError ? png.string() : relative.string();
        preset.size = static_cast<float>(std::clamp<uint32_t>(
            std::max(tip.width, tip.height), 2, 250));
        preset.spacing = tip.spacing;
        preset.hardness = 1.f;
        preset.strength = 1.f;
        if (SaveBrushPreset(UniquePath(directory / (name + ".brush")),
            preset, errorText)) ++imported;
        else report += " Failed preset: " + name + " (" + errorText + ").";
    }
    report = std::to_string(imported) + " brush assets imported. Source: " +
        sourceCopy.string() + ". " + report;
    return imported > 0;
}
}
