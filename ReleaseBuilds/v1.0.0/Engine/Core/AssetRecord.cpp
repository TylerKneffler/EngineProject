#include "Core/AssetRecord.h"

#include "Core/Serialization/Json.h"

#include <array>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace Engine::Core
{
std::string GenerateStableId()
{
    std::array<unsigned char, 16> bytes{};
    std::random_device random;
    for (unsigned char& byte : bytes)
        byte = static_cast<unsigned char>(random());

    // RFC 4122 version 4 and variant bits.
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);

    std::ostringstream value;
    value << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            value << '-';
        value << std::setw(2) << static_cast<unsigned>(bytes[index]);
    }
    return value.str();
}

std::string NormalizeSourcePath(const fs::path& path)
{
    std::error_code error;
    fs::path normalized = fs::weakly_canonical(path, error);
    if (error)
    {
        error.clear();
        normalized = fs::absolute(path, error);
    }
    if (error)
        normalized = path.lexically_normal();
    return normalized.generic_string();
}

Engine::Serialization::JsonValue WriteSettings(const Engine::Model::AssetImportSettings& settings)
{
    Engine::Serialization::JsonValue output = Engine::Serialization::JsonValue::MakeObject();
    for (const auto& [name, setting] : settings)
    {
        std::visit([&](const auto& value) { output.Set(name, Engine::Serialization::JsonValue(value)); }, setting);
    }
    return output;
}

Engine::Model::AssetImportSettings ReadSettings(const Engine::Serialization::JsonValue& value)
{
    Engine::Model::AssetImportSettings settings;
    if (!value.IsObject())
        return settings;
    for (std::size_t index = 0; index < value.ObjectSize(); ++index)
    {
        const Engine::Serialization::JsonValue& setting = value.ObjectValue(index);
        if (setting.IsBool())
            settings[value.ObjectKey(index)] = setting.AsBool();
        else if (setting.IsNumber())
            settings[value.ObjectKey(index)] = setting.AsDouble();
        else if (setting.IsString())
            settings[value.ObjectKey(index)] = setting.AsString();
    }
    return settings;
}
fs::path AssetRecord::SidecarPath(const fs::path& assetPath)
{
    return fs::path(assetPath.string() + ".meta");
}

std::optional<AssetRecord> AssetRecord::Load(const fs::path& assetPath)
{
    const fs::path sidecar = SidecarPath(assetPath);
    std::error_code error;
    if (!fs::exists(sidecar, error))
        return std::nullopt;

    const Engine::Serialization::JsonValue root = Engine::Serialization::JsonParseFile(sidecar.string());
    if (!root.IsObject() || !root["version"].IsNumber() ||
        !root["id"].IsString() ||
        !root["sourcePath"].IsString())
        throw std::runtime_error("Invalid asset record: " + sidecar.string());

    AssetRecord record;
    record.version = root["version"].AsInt();
    record.id = root["id"].AsString();
    record.sourcePath = root["sourcePath"].AsString();
    record.importSettings = ReadSettings(root["importSettings"]);
    if (record.version != CurrentVersion || record.id.empty() ||
        record.sourcePath.empty())
        throw std::runtime_error("Unsupported or incomplete asset record: " +
            sidecar.string());
    return record;
}

AssetRecord AssetRecord::Ensure(const fs::path& assetPath,
    const fs::path& sourcePath, const Engine::Model::AssetImportSettings& defaultSettings)
{
    if (auto existing = Load(assetPath))
    {
        bool changed = false;
        if (existing->sourcePath.empty())
        {
            existing->sourcePath = NormalizeSourcePath(sourcePath);
            changed = true;
        }
        for (const auto& [name, value] : defaultSettings)
            if (existing->importSettings.emplace(name, value).second)
                changed = true;
        if (changed && !existing->Save(assetPath))
            throw std::runtime_error("Could not update asset record: " +
                SidecarPath(assetPath).string());
        return *existing;
    }

    AssetRecord record;
    record.id = GenerateStableId();
    record.sourcePath = NormalizeSourcePath(sourcePath);
    record.importSettings = defaultSettings;
    if (!record.Save(assetPath))
        throw std::runtime_error("Could not create asset record: " +
            SidecarPath(assetPath).string());
    return record;
}

bool AssetRecord::Move(const fs::path& oldAssetPath,
    const fs::path& newAssetPath, std::error_code& error)
{
    error.clear();
    const fs::path oldSidecar = SidecarPath(oldAssetPath);
    if (!fs::exists(oldSidecar, error))
    {
        error.clear();
        return true;
    }
    fs::rename(oldSidecar, SidecarPath(newAssetPath), error);
    return !error;
}

bool AssetRecord::Save(const fs::path& assetPath) const
{
    if (version != CurrentVersion || id.empty() || sourcePath.empty())
        return false;

    Engine::Serialization::JsonValue root = Engine::Serialization::JsonValue::MakeObject();
    root.Set("version", Engine::Serialization::JsonValue(version));
    root.Set("id", Engine::Serialization::JsonValue(id));
    root.Set("sourcePath", Engine::Serialization::JsonValue(sourcePath));
    root.Set("importSettings", WriteSettings(importSettings));

    std::ofstream output(SidecarPath(assetPath), std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output << Engine::Serialization::JsonWrite(root) << '\n';
    return output.good();
}
}
