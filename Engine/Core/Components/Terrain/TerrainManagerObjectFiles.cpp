#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainObjectArchive.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/Json.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Rendering/DetailBudget.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>

// Object layer settings by name, and the per-cell object files of procedural
// cells (TerrainManager.h, "Object placements of procedural cells").
namespace Engine::Components
{
using Engine::Serialization::JsonValue;

// ---- layer settings --------------------------------------------------------

TerrainObjectLayer::Settings TerrainObjectLayer::Settings::From(const TerrainObjectLayer& layer)
{
    Settings s;
    s.viewDistance = layer.viewDistance; s.lod1Distance = layer.lod1Distance;
    s.lod2Distance = layer.lod2Distance; s.lodHysteresis = layer.lodHysteresis;
    s.screenLodSelection = layer.screenLodSelection;
    s.screenLod1Pixels = layer.screenLod1Pixels; s.screenLod2Pixels = layer.screenLod2Pixels;
    s.shadowDistance = layer.shadowDistance; s.farShadowDistance = layer.farShadowDistance;
    s.shadowFadeWidth = layer.shadowFadeWidth; s.shadowLod = layer.shadowLod;
    s.farObjects = layer.farObjects;
    s.snapToGround = layer.snapToGround ? 1 : 0;
    s.conformToGround = layer.conformToGround ? 1 : 0;
    if (layer.Owner)
        for (const auto* component : layer.Owner->Components)
            if (component && component != &layer)
                s.components.Push(JsonValue::MakeObject().Set("type", JsonValue(component->GetTypeName()))
                    .Set("value", component->Serialize()));
    return s;
}

std::string TerrainObjectLayer::Settings::ComponentsKey() const
{
    return components.ArraySize() ? Engine::Serialization::JsonWriteCompact(components) : std::string{};
}

void TerrainObjectLayer::Settings::ApplyTo(TerrainObjectLayer& layer) const
{
    // Components first: each is configured only when its value changed
    // since it was last applied (a GrassField rebuilds its tiles on change).
    if (layer.Owner)
    {
        for (size_t i = 0; i < components.ArraySize(); ++i)
        {
            const auto& entry = components.ArrayAt(i);
            const std::string type = entry["type"].AsString();
            if (type.empty() || type == "TerrainObjectLayer") continue;
            Engine::Core::Component* component = nullptr;
            for (auto* existing : layer.Owner->Components)
                if (existing && existing->GetTypeName() == type) { component = existing; break; }
            const std::string key = Engine::Serialization::JsonWriteCompact(entry["value"]);
            auto& applied = layer.m_appliedComponents[type];
            if (component && applied == key) continue;
            if (!component)
            {
                component = Engine::Serialization::SceneSerializer::CreateRegisteredComponent(type);
                if (!component) continue;
                component->Deserialize(entry["value"]);
                layer.Owner->AddComponent(component);
            }
            else
                component->Deserialize(entry["value"]);
            applied = key;
        }
        if (!detailLayer.empty())
            layer.Owner->layer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault(detailLayer.c_str());
    }
    if (snapToGround >= 0) layer.snapToGround = snapToGround != 0;
    if (conformToGround >= 0) layer.conformToGround = conformToGround != 0;
    Settings current = From(layer);
    current.components = components;
    current.detailLayer = detailLayer;
    if (snapToGround < 0) current.snapToGround = -1;
    if (conformToGround < 0) current.conformToGround = -1;
    if (current == *this) return;
    layer.viewDistance = std::max(1.f, viewDistance);
    layer.lod1Distance = std::clamp(lod1Distance, 0.f, layer.viewDistance);
    layer.lod2Distance = std::clamp(lod2Distance, layer.lod1Distance, layer.viewDistance);
    layer.lodHysteresis = std::clamp(lodHysteresis, 0.f, 10.f);
    layer.screenLodSelection = screenLodSelection;
    layer.screenLod1Pixels = screenLod1Pixels; layer.screenLod2Pixels = screenLod2Pixels;
    layer.shadowDistance = std::clamp(shadowDistance, 0.f, layer.viewDistance);
    layer.farShadowDistance = std::clamp(farShadowDistance, 0.f, layer.viewDistance);
    layer.shadowFadeWidth = shadowFadeWidth;
    layer.shadowLod = std::clamp(shadowLod, -1, 2);
    layer.farObjects = farObjects;
    layer.MarkConfigurationDirty();
}

JsonValue TerrainObjectLayer::Settings::Serialize() const
{
    return JsonValue::MakeObject()
        .Set("viewDistance", JsonValue(viewDistance)).Set("lod1Distance", JsonValue(lod1Distance))
        .Set("lod2Distance", JsonValue(lod2Distance)).Set("lodHysteresis", JsonValue(lodHysteresis))
        .Set("screenLodSelection", JsonValue(screenLodSelection))
        .Set("screenLod1Pixels", JsonValue(screenLod1Pixels)).Set("screenLod2Pixels", JsonValue(screenLod2Pixels))
        .Set("shadowDistance", JsonValue(shadowDistance)).Set("farShadowDistance", JsonValue(farShadowDistance))
        .Set("shadowFadeWidth", JsonValue(shadowFadeWidth)).Set("shadowLod", JsonValue(shadowLod))
        .Set("farObjects", JsonValue(farObjects))
        .Set("snapToGround", JsonValue(snapToGround)).Set("conformToGround", JsonValue(conformToGround))
        .Set("detailLayer", JsonValue(detailLayer)).Set("components", components);
}

TerrainObjectLayer::Settings TerrainObjectLayer::Settings::Deserialize(const JsonValue& value)
{
    Settings s;
    const auto number = [&](const char* key, float fallback) {
        return value.Has(key) ? value[key].AsFloat() : fallback;
    };
    s.viewDistance = number("viewDistance", s.viewDistance);
    s.lod1Distance = number("lod1Distance", s.lod1Distance);
    s.lod2Distance = number("lod2Distance", s.lod2Distance);
    s.lodHysteresis = number("lodHysteresis", s.lodHysteresis);
    s.screenLodSelection = value.Has("screenLodSelection") && value["screenLodSelection"].AsBool();
    s.screenLod1Pixels = number("screenLod1Pixels", s.screenLod1Pixels);
    s.screenLod2Pixels = number("screenLod2Pixels", s.screenLod2Pixels);
    s.shadowDistance = number("shadowDistance", s.shadowDistance);
    s.farShadowDistance = number("farShadowDistance", s.farShadowDistance);
    s.shadowFadeWidth = number("shadowFadeWidth", s.shadowFadeWidth);
    s.shadowLod = value.Has("shadowLod") ? value["shadowLod"].AsInt() : s.shadowLod;
    s.farObjects = !value.Has("farObjects") || value["farObjects"].AsBool();
    s.snapToGround = value.Has("snapToGround") ? value["snapToGround"].AsInt() : -1;
    s.conformToGround = value.Has("conformToGround") ? value["conformToGround"].AsInt() : -1;
    s.detailLayer = value.Has("detailLayer") ? value["detailLayer"].AsString() : std::string{};
    if (value["components"].IsArray()) s.components = value["components"];
    return s;
}

const TerrainObjectLayer::Settings* TerrainManager::GetObjectLayerSettings(const std::string& name) const
{
    const auto found = m_objectLayerSettings.find(name);
    return found == m_objectLayerSettings.end() ? nullptr : &found->second;
}

void TerrainManager::SetObjectLayerSettings(const std::string& name,
    const TerrainObjectLayer::Settings& settings)
{
    if (name.empty()) return;
    m_objectLayerSettings[name] = settings;
    for (const auto& [cell, terrain] : m_loaded)
        if (terrain && terrain->Owner)
            for (auto* child : terrain->Owner->Children)
                if (auto* layer = child ? child->GetComponent<TerrainObjectLayer>() : nullptr)
                    if (layer->layerName == name) settings.ApplyTo(*layer);
    MarkConfigurationDirty();
}

void TerrainManager::ApplyObjectLayerSettings(TerrainObjectLayer& layer) const
{
    if (const auto* settings = GetObjectLayerSettings(layer.layerName)) settings->ApplyTo(layer);
}

// ---- object files -----------------------------------------------------------

namespace
{
constexpr uint64_t FileMagic = 0x314a424f524e5254ull; // TRNROBJ1 (in the file's "magic")

// Contents of the object files written this session, by path and revision
// (an empty optional: no file), so undo can bring any of them back.
struct FileHistory
{
    std::mutex mutex;
    std::map<std::string, std::map<uint64_t, std::optional<JsonValue>>> files;
};
FileHistory& History()
{
    static FileHistory history;
    return history;
}

uint64_t NewRevision()
{
    static std::mt19937_64 random(static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x9e3779b97f4a7c15ull);
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    uint64_t revision = 0;
    while (!revision) revision = random();
    return revision;
}

bool ReadFile(const std::string& path, uint64_t& revision, JsonValue& layers)
{
    std::error_code error;
    if (path.empty() || !std::filesystem::exists(path, error)) return false;
    try
    {
        const JsonValue file = Engine::Serialization::AssetParseFile(path);
        if (!file.Has("revision") || !file["layers"].IsArray()) return false;
        revision = std::stoull(file["revision"].AsString());
        layers = file["layers"];
        return true;
    }
    catch (...) { return false; }
}
}

bool TerrainManager::UsesObjectFiles(Cell cell) const
{
    return !cellDataDirectory.empty() && m_proceduralCells.count(cell) != 0;
}

std::string TerrainManager::GetObjectFilePath(Cell cell) const
{
    return cellDataDirectory.empty() ? std::string{} :
        (std::filesystem::path(cellDataDirectory) /
            (std::to_string(cell.first) + "_" + std::to_string(cell.second) + ".objects")).string();
}

const std::set<TerrainManager::Cell>& TerrainManager::ObjectFileCells() const
{
    if (m_objectFileCells) return *m_objectFileCells;
    m_objectFileCells.emplace();
    std::error_code error;
    if (cellDataDirectory.empty() || !std::filesystem::is_directory(cellDataDirectory, error))
        return *m_objectFileCells;
    for (const auto& entry : std::filesystem::directory_iterator(cellDataDirectory, error))
    {
        if (entry.path().extension() != ".objects") continue;
        int x = 0, z = 0;
        if (std::sscanf(entry.path().stem().string().c_str(), "%d_%d", &x, &z) == 2)
            m_objectFileCells->insert({x, z});
    }
    return *m_objectFileCells;
}

uint64_t TerrainManager::ObjectFileRevision(Cell cell) const
{
    if (const auto found = m_objectFileRevisions.find(cell); found != m_objectFileRevisions.end())
        return found->second;
    uint64_t revision = 0;
    JsonValue layers;
    if (ObjectFileCells().count(cell) && ReadFile(GetObjectFilePath(cell), revision, layers))
        m_objectLayers[cell] = std::move(layers);
    m_objectFileRevisions[cell] = revision;
    return revision;
}

std::shared_ptr<const TerrainObjectArchive> TerrainManager::GetObjectArchive() const
{
    if (objectArchive.empty()) return nullptr;
    if (m_objectArchiveOpened != objectArchive)
    {
        m_objectArchiveOpened = objectArchive;
        m_objectArchive = TerrainObjectArchive::Open(Engine::Components::Mesh::ResolveFilePath(objectArchive));
        if (m_objectArchive)
        {
            const auto& header = m_objectArchive->GetHeader();
            if (header.cellsX != cellsX || header.cellsY != cellsY ||
                header.cellWidth != cellWidth || header.cellDepth != cellDepth)
                m_objectArchive.reset();   // baked for another grid
        }
    }
    return m_objectArchive;
}

bool TerrainManager::GetSavedObjectLayers(Cell cell, JsonValue& layers) const
{
    if (const auto found = m_objectLayers.find(cell); found != m_objectLayers.end())
    { layers = found->second; return true; }
    if (UsesObjectFiles(cell) && ObjectFileCells().count(cell))
    {
        ObjectFileRevision(cell);   // reads the file into m_objectLayers
        if (const auto found = m_objectLayers.find(cell); found != m_objectLayers.end())
        { layers = found->second; return true; }
    }
    if (!m_proceduralCells.count(cell)) return false;
    const auto archive = GetObjectArchive();
    return archive && archive->Covers(cell) && archive->ReadCellLayers(cell, layers) && layers.ArraySize();
}

JsonValue TerrainManager::CollectObjectLayers(const Engine::Core::Object& cellObject)
{
    JsonValue layers = JsonValue::MakeArray();
    for (const auto* child : cellObject.Children)
        if (child)
            if (const auto* layer = child->GetComponent<TerrainObjectLayer>())
                layers.Push(layer->Serialize());
    return layers;
}

uint64_t TerrainManager::HashCellObjects(const Engine::Core::Object& cellObject)
{
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
    };
    const auto text = [&](const std::string& value) { mix(value.data(), value.size()); mix("\0", 1); };
    for (const auto* child : cellObject.Children)
    {
        const auto* layer = child ? child->GetComponent<TerrainObjectLayer>() : nullptr;
        if (!layer) continue;
        text(layer->layerName);
        const auto settings = TerrainObjectLayer::Settings::From(*layer);
        const float values[] = {settings.viewDistance, settings.lod1Distance, settings.lod2Distance,
            settings.lodHysteresis, settings.screenLod1Pixels, settings.screenLod2Pixels,
            settings.shadowDistance, settings.farShadowDistance, settings.shadowFadeWidth};
        mix(values, sizeof(values));
        const int flags[] = {settings.screenLodSelection, settings.shadowLod, layer->conformToGround,
            layer->snapToGround, settings.farObjects};
        mix(flags, sizeof(flags));
        const uint64_t count = layer->placements.size();
        mix(&count, sizeof(count));
        for (const auto& placement : layer->placements)
        {
            text(placement.asset);
            const float p[] = {placement.position.x, placement.position.y, placement.position.z,
                placement.normal.x, placement.normal.y, placement.normal.z,
                placement.yawDegrees, placement.scale};
            mix(p, sizeof(p));
        }
    }
    return hash;
}

void TerrainManager::SetObjectBaseline(Cell cell, const Engine::Core::Object& cellObject)
{
    if (!UsesObjectFiles(cell)) return;
    m_objectBaseline[cell] = HashCellObjects(cellObject);
}

bool TerrainManager::WriteObjectFile(Cell cell, const JsonValue* layers) const
{
    const std::string path = GetObjectFilePath(cell);
    if (path.empty()) return false;
    const uint64_t before = ObjectFileRevision(cell);
    {
        // Undo needs what was there before this session's first write.
        auto& history = History();
        std::lock_guard lock(history.mutex);
        auto& revisions = history.files[path];
        if (!revisions.count(before))
        {
            std::optional<JsonValue> previous;
            if (before)
                if (const auto found = m_objectLayers.find(cell); found != m_objectLayers.end())
                    previous = found->second;
            revisions[before] = std::move(previous);
        }
    }
    std::error_code error;
    if (!layers)
    {
        std::filesystem::remove(path, error);
        m_objectFileRevisions[cell] = 0;
        if (m_objectFileCells) m_objectFileCells->erase(cell);
        m_objectLayers.erase(cell);
        auto& history = History();
        std::lock_guard lock(history.mutex);
        history.files[path][0] = std::nullopt;
        return true;
    }
    const uint64_t revision = NewRevision();
    JsonValue file = JsonValue::MakeObject();
    file.Set("magic", JsonValue(std::to_string(FileMagic)))
        .Set("revision", JsonValue(std::to_string(revision)))
        .Set("layers", *layers);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const std::string bytes = Engine::Serialization::AssetWriteBinary(file);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        error.clear();
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error) { std::filesystem::remove(temporary, error); return false; }
    }
    m_objectFileRevisions[cell] = revision;
    ObjectFileCells();
    m_objectFileCells->insert(cell);
    m_objectLayers[cell] = *layers;
    auto& history = History();
    std::lock_guard lock(history.mutex);
    history.files[path][revision] = *layers;
    return true;
}

size_t TerrainManager::SaveObjectFiles() const
{
    size_t written = 0;
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return 0;   // play writes none
    for (const auto& [cell, terrain] : m_loaded)
    {
        if (!terrain || !terrain->Owner || !UsesObjectFiles(cell)) continue;
        const uint64_t hash = HashCellObjects(*terrain->Owner);
        if (const auto baseline = m_objectBaseline.find(cell);
            baseline != m_objectBaseline.end() && baseline->second == hash)
            continue;
        const JsonValue layers = CollectObjectLayers(*terrain->Owner);
        if (WriteObjectFile(cell, &layers)) { m_objectBaseline[cell] = hash; ++written; }
    }
    // Layers an older scene kept for unloaded file-backed cells.
    for (auto it = m_objectFilesPending.begin(); it != m_objectFilesPending.end(); )
    {
        const auto found = m_objectLayers.find(*it);
        if (!m_loaded.count(*it) && found != m_objectLayers.end())
        {
            const JsonValue layers = found->second;
            if (WriteObjectFile(*it, &layers)) ++written;
        }
        it = m_objectFilesPending.erase(it);
    }
    return written;
}

void TerrainManager::RestoreObjectFiles(const JsonValue& listed)
{
    if (cellDataDirectory.empty()) return;
    std::map<Cell, uint64_t> wanted;
    for (size_t i = 0; i < listed.ArraySize(); ++i)
    {
        const auto& entry = listed.ArrayAt(i);
        try { wanted[{entry["x"].AsInt(), entry["y"].AsInt()}] = std::stoull(entry["revision"].AsString()); }
        catch (...) {}
    }
    // Every cell either side knows of: listed, on disk, or written this session.
    std::set<Cell> cells(ObjectFileCells().begin(), ObjectFileCells().end());
    for (const auto& [cell, revision] : wanted) cells.insert(cell);
    {
        auto& history = History();
        std::lock_guard lock(history.mutex);
        for (const auto& [path, revisions] : history.files)
        {
            const auto stem = std::filesystem::path(path).stem().string();
            int x = 0, z = 0;
            if (std::filesystem::path(path).parent_path() == std::filesystem::path(cellDataDirectory) &&
                std::sscanf(stem.c_str(), "%d_%d", &x, &z) == 2)
                cells.insert({x, z});
        }
    }
    for (const Cell cell : cells)
    {
        const auto target = wanted.count(cell) ? wanted[cell] : 0ull;
        if (ObjectFileRevision(cell) == target) continue;
        std::optional<JsonValue> content;
        bool known = false;
        {
            auto& history = History();
            std::lock_guard lock(history.mutex);
            const auto file = history.files.find(GetObjectFilePath(cell));
            if (file != history.files.end())
                if (const auto found = file->second.find(target); found != file->second.end())
                { content = found->second; known = true; }
        }
        // A revision from another session (a scene saved with files that
        // changed since): the files stay as they are.
        if (!known) continue;
        if (!content) WriteObjectFile(cell, nullptr);
        else
        {
            // Back to that revision (same contents, same revision number).
            WriteObjectFile(cell, &*content);
            m_objectFileRevisions[cell] = target;
            const std::string path = GetObjectFilePath(cell);
            JsonValue file = JsonValue::MakeObject();
            file.Set("magic", JsonValue(std::to_string(FileMagic)))
                .Set("revision", JsonValue(std::to_string(target)))
                .Set("layers", *content);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            const std::string bytes = Engine::Serialization::AssetWriteBinary(file);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        m_objectBaseline.erase(cell);
        InvalidateFarObjectCell(cell);
        MarkMacroCellsDirty(cell);
    }
}

void TerrainManager::InvalidateFarObjectCell(Cell cell)
{
    if (m_farObjectCells.erase(cell)) m_farObjectQueue.push_back(cell);
}
}
