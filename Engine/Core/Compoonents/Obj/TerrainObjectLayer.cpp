#include "TerrainObjectLayer.h"
#include "Core/Object.h"
#include "Engine/Editor/UI/IEditorUi.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include "Core/Serialization/HexBytes.h"
#include "Core/Serialization/PrefabLoadProfile.h"
#include <glm/gtc/matrix_transform.hpp>

namespace Engine::Components
{
TerrainObjectLayer::TerrainObjectLayer()
{
    SetTypeName(COMPONENT_TYPE_NAME(TerrainObjectLayer));
    RegisterField("viewDistance", viewDistance, "Object LOD");
    RegisterField("lod1Distance", lod1Distance, "Object LOD");
    RegisterField("lod2Distance", lod2Distance, "Object LOD");
    RegisterField("lodHysteresis", lodHysteresis, "Object LOD");
    RegisterField("screenLodSelection", screenLodSelection, "Object LOD");
    RegisterField("distanceLodOverride", distanceLodOverride, "Object LOD");
    RegisterField("screenLod1Pixels", screenLod1Pixels, "Object LOD");
    RegisterField("screenLod2Pixels", screenLod2Pixels, "Object LOD");
    RegisterField("screenLodHysteresis", screenLodHysteresis, "Object LOD");
    RegisterField("shadowDistance", shadowDistance, "Object LOD");
    RegisterField("shadowFadeWidth", shadowFadeWidth, "Object LOD");
    RegisterField("shadowLod", shadowLod, "Object LOD");
}

float TerrainObjectLayer::GetShadowFadeWidth() const
{
    return std::clamp(std::isfinite(shadowFadeWidth) ? shadowFadeWidth : 1.f,
        0.f, std::max(0.f, shadowDistance));
}

std::map<std::string, std::vector<std::string>> TerrainObjectLayer::GetDefinedObjectLods() const
{
    std::map<std::string, std::vector<std::string>> definitions;
    if (Owner)
        for (const auto* component : Owner->Components)
            if (const auto* provider = dynamic_cast<const TerrainObjectLodProvider*>(component))
                for (const auto& [asset, meshes] : provider->GetTerrainObjectLodMeshes())
                    definitions[asset] = meshes;
    for (const auto& placement : placements)
        if (!definitions.count(placement.asset)) definitions[placement.asset] = {placement.asset};
    for (const auto& [asset, settings] : objectLods)
        if (!settings.meshes.empty()) definitions[asset] = settings.meshes;
    for (auto& [asset, meshes] : definitions)
    {
        meshes.erase(std::remove(meshes.begin(), meshes.end(), std::string{}), meshes.end());
        if (meshes.empty()) meshes.push_back(asset);
        if (meshes.size() > MaximumObjectLods) meshes.resize(MaximumObjectLods);
    }
    return definitions;
}

std::vector<float> TerrainObjectLayer::GetObjectLodDistances(const std::string& asset, size_t lodCount) const
{
    const size_t count = std::min(lodCount, MaximumObjectLods);
    std::vector<float> result;
    const auto found = objectLods.find(asset);
    const float maximum = std::isfinite(viewDistance) ? std::clamp(viewDistance, 1.f, 100000.f) : 18.f;
    for (size_t i = 1; i < count; ++i)
    {
        float distance = count == 3 ? (i == 1 ? lod1Distance : lod2Distance) : maximum * float(i) / float(count);
        if (found != objectLods.end() && i <= found->second.distances.size())
            distance = found->second.distances[i - 1];
        if (!std::isfinite(distance)) distance = maximum * float(i) / float(count);
        result.push_back(std::clamp(distance, result.empty() ? 0.f : result.back(), maximum));
    }
    return result;
}

glm::quat TerrainObjectLayer::SurfaceRotation(const glm::vec3& normal,
    float yawDegrees)
{
    const glm::vec3 up = glm::length(normal) > 1.e-5f
        ? glm::normalize(normal) : glm::vec3(0.f, 1.f, 0.f);
    return glm::normalize(glm::rotation(glm::vec3(0.f, 1.f, 0.f), up) *
        glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0.f, 1.f, 0.f)));
}

namespace
{
void PlacementHash(uint64_t& hash,const void* data,size_t size)
{ const auto* bytes=static_cast<const uint8_t*>(data); for(size_t i=0;i<size;++i) { hash^=bytes[i]; hash*=1099511628211ull; } }
template<class T> void AppendBytes(std::vector<uint8_t>& out,const T& value)
{ const auto* bytes=reinterpret_cast<const uint8_t*>(&value); out.insert(out.end(),bytes,bytes+sizeof(value)); }
}
uint64_t TerrainObjectLayer::GetPlacementSignature() const
{
    uint64_t signature=1469598103934665603ull; std::unordered_set<uint64_t> used;
    for(uint32_t index=0;index<placements.size();++index)
    {
        const auto& p=placements[index];
        if(!p.id || used.count(p.id))
        {
            uint64_t id=1469598103934665603ull; PlacementHash(id,p.asset.data(),p.asset.size());
            PlacementHash(id,&p.position,sizeof(p.position)); PlacementHash(id,&index,sizeof(index));
            if(!id) id=1;
            while(used.count(id)) ++id;
            p.id=id;
        }
        used.insert(p.id);
        PlacementHash(signature,p.asset.data(),p.asset.size());
        PlacementHash(signature,&p.position,sizeof(p.position)); PlacementHash(signature,&p.normal,sizeof(p.normal));
        PlacementHash(signature,&p.yawDegrees,sizeof(float)); PlacementHash(signature,&p.scale,sizeof(float)); PlacementHash(signature,&p.id,sizeof(p.id));
    }
    return signature;
}
void TerrainObjectLayer::AddPrepared(int x,int z,const PreparedPlacement& record) const
{
    auto& tile=m_prepared[{x,z}]; const auto& placement=placements[record.index];
    tile.groups[placement.asset].push_back(record);
    const glm::vec3 position(record.transform.rows[0].w,record.transform.rows[1].w,record.transform.rows[2].w);
    if(!tile.initialized) { tile.minimum=tile.maximum=position; tile.initialized=true; }
    else { tile.minimum=glm::min(tile.minimum,position); tile.maximum=glm::max(tile.maximum,position); }
    tile.maximumScale=std::max(tile.maximumScale,std::abs(placement.scale));
}
const TerrainObjectLayer::PreparedTiles& TerrainObjectLayer::GetPreparedTiles(float requestedSpan) const
{
    const float span=std::max(1.f,requestedSpan);
    if(m_preparedRevision==GetConfigurationRevision() && m_preparedData==placements.data() && m_preparedCount==placements.size() && m_preparedSpan==span) return m_prepared;
    m_prepared.clear(); m_preparedSignature=GetPlacementSignature(); m_preparedSpan=span; m_loadedPrepared=false;
    for(uint32_t index=0;index<placements.size();++index)
    {
        const auto& p=placements[index]; const int x=int(std::floor(p.position.x/span)),z=int(std::floor(p.position.z/span));
        const auto rotation=glm::mat3_cast(SurfaceRotation(p.normal,p.yawDegrees));
        const auto translation=p.position-glm::vec3(x*span,0,z*span); PreparedPlacement record; record.index=index;
        for(int row=0;row<3;++row) record.transform.rows[row]={rotation[0][row]*p.scale,rotation[1][row]*p.scale,rotation[2][row]*p.scale,translation[row]};
        AddPrepared(x,z,record);
    }
    m_preparedRevision=GetConfigurationRevision(); m_preparedData=placements.data(); m_preparedCount=placements.size(); return m_prepared;
}
std::string TerrainObjectLayer::SerializePrepared() const
{
    GetPreparedTiles(m_preparedSpan);
    std::vector<uint8_t> bytes; bytes.reserve(20+placements.size()*60);
    AppendBytes(bytes,uint32_t(0x31494c54)); AppendBytes(bytes,m_preparedSpan); AppendBytes(bytes,uint32_t(placements.size())); AppendBytes(bytes,m_preparedSignature);
    for(const auto& [tile,prepared]:m_prepared) for(const auto& [asset,records]:prepared.groups) for(const auto& record:records)
    { AppendBytes(bytes,record.index); AppendBytes(bytes,int32_t(tile.first)); AppendBytes(bytes,int32_t(tile.second)); AppendBytes(bytes,record.transform); }
    return Engine::Serialization::EncodeHex(bytes);
}
void TerrainObjectLayer::LoadPrepared(const std::string& text, uint64_t placementSignature)
{
    Engine::Serialization::PrefabLoadProfile::Stage stage("placement_restore_baked");
    if(text.size()!=2*(20+placements.size()*60)) return;
    std::vector<uint8_t> bytes; if(!Engine::Serialization::DecodeHex(text,bytes)) return;
    size_t cursor=0;
    const auto read=[&](auto& value) { std::memcpy(&value,bytes.data()+cursor,sizeof(value)); cursor+=sizeof(value); };
    uint32_t magic=0,count=0; float span=0; uint64_t signature=0;
    read(magic); read(span); read(count); read(signature);
    if(magic!=0x31494c54 || !std::isfinite(span) || span<1 || count!=placements.size() || signature!=placementSignature) return;
    PreparedTiles old=std::move(m_prepared); std::unordered_set<uint32_t> indices;
    for(uint32_t i=0;i<count;++i)
    {
        PreparedPlacement record; int32_t x=0,z=0; read(record.index); read(x); read(z); read(record.transform);
        bool valid=record.index<placements.size() && indices.insert(record.index).second;
        for(const auto& row:record.transform.rows) for(int c=0;c<4;++c) valid=valid && std::isfinite(row[c]);
        if(!valid) { m_prepared=std::move(old); return; }
        AddPrepared(x,z,record);
    }
    m_preparedSpan=span; m_preparedSignature=signature; m_preparedRevision=GetConfigurationRevision();
    m_preparedData=placements.data(); m_preparedCount=placements.size(); m_loadedPrepared=true;
}

TerrainObjectLayer::JsonValue TerrainObjectLayer::Serialize() const
{
    using Engine::Serialization::PrefabLoadProfile;
    PrefabLoadProfile::Stage caller(PrefabLoadProfile::Enabled()
        ? "placement_caller_" + PrefabLoadProfile::ActiveStage() + "_" + layerName +
            "_count_" + std::to_string(placements.size()) : "");
    PrefabLoadProfile::Stage total("placement_snapshot_total");
    auto baked = [&] {
        PrefabLoadProfile::Stage stage("placement_snapshot_prepared");
        return SerializePrepared();
    }();
    JsonValue out = Component::Serialize();
    out.Set("bakedInstances",JsonValue(std::move(baked)));
    out.Set("layerName", JsonValue(layerName));
    JsonValue lodEntries = JsonValue::MakeArray();
    for (const auto& [asset, settings] : objectLods)
    {
        JsonValue meshes = JsonValue::MakeArray(), distances = JsonValue::MakeArray();
        for (const auto& mesh : settings.meshes) meshes.Push(JsonValue(mesh));
        const size_t count = settings.meshes.empty() ? settings.distances.size() + 1 : settings.meshes.size();
        for (float distance : GetObjectLodDistances(asset, count)) distances.Push(JsonValue(distance));
        lodEntries.Push(JsonValue::MakeObject().Set("asset", JsonValue(asset))
            .Set("meshes", std::move(meshes)).Set("distances", std::move(distances)));
    }
    out.Set("objectLods", std::move(lodEntries));
    {
        PrefabLoadProfile::Stage stage("placement_snapshot_records");
        JsonValue entries = JsonValue::MakeArray();
        for (const Placement& placement : placements)
        {
            JsonValue entry = JsonValue::MakeObject(6);
            JsonValue position = JsonValue::MakeArray();
            position.Push(JsonValue(placement.position.x))
                .Push(JsonValue(placement.position.y)).Push(JsonValue(placement.position.z));
            JsonValue normal = JsonValue::MakeArray();
            normal.Push(JsonValue(placement.normal.x))
                .Push(JsonValue(placement.normal.y)).Push(JsonValue(placement.normal.z));
            entry
                .Set("id", JsonValue(std::to_string(placement.id)))
                .Set("asset", JsonValue(placement.asset))
                .Set("position", std::move(position))
                .Set("normal", std::move(normal))
                .Set("yawDegrees", JsonValue(placement.yawDegrees))
                .Set("scale", JsonValue(placement.scale));
            entries.Push(std::move(entry));
        }
        out.Set("placements", std::move(entries));
    }
    return out;
}

void TerrainObjectLayer::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    viewDistance = std::clamp(viewDistance, 1.f, 100000.f);
    lod1Distance = std::clamp(lod1Distance, 0.f, viewDistance);
    lod2Distance = std::clamp(lod2Distance, lod1Distance, viewDistance);
    lodHysteresis = std::clamp(lodHysteresis, 0.f, 10.f);
    shadowDistance = std::clamp(shadowDistance, 0.f, viewDistance);
    shadowFadeWidth = GetShadowFadeWidth();
    shadowLod = std::clamp(shadowLod, -1, 2);
    layerName = value.Has("layerName") ? value["layerName"].AsString() : "Objects";
    objectLods.clear();
    const auto& lodEntries = value["objectLods"];
    for (size_t i = 0; i < lodEntries.ArraySize(); ++i)
    {
        const auto& entry = lodEntries.ArrayAt(i);
        const std::string asset = entry["asset"].AsString();
        if (asset.empty()) continue;
        ObjectLods settings;
        const auto& meshes = entry["meshes"];
        for (size_t j = 0; j < std::min(meshes.ArraySize(), MaximumObjectLods); ++j)
            if (meshes.ArrayAt(j).IsString() && !meshes.ArrayAt(j).AsString().empty())
                settings.meshes.push_back(meshes.ArrayAt(j).AsString());
        const auto& distances = entry["distances"];
        const size_t count = settings.meshes.empty() ? MaximumObjectLods : settings.meshes.size();
        for (size_t j = 0; j < distances.ArraySize() && j + 1 < count; ++j)
            settings.distances.push_back(distances.ArrayAt(j).AsFloat());
        objectLods[asset] = std::move(settings);
        const size_t normalizedCount = objectLods[asset].meshes.empty()
            ? objectLods[asset].distances.size() + 1 : objectLods[asset].meshes.size();
        objectLods[asset].distances = GetObjectLodDistances(asset, normalizedCount);
    }
    placements.clear();
    const auto& entries = value["placements"];
    placements.reserve(entries.ArraySize());
    for (size_t i = 0; i < entries.ArraySize(); ++i)
    {
        const auto& entry = entries.ArrayAt(i);
        const auto& position = entry["position"];
        if (position.ArraySize() != 3) continue;
        Placement placement;
        if(entry["id"].IsString()) { try { placement.id=std::stoull(entry["id"].AsString()); } catch(...) {} }
        placement.asset = entry["asset"].AsString();
        placement.position = {position.ArrayAt(0).AsFloat(),
            position.ArrayAt(1).AsFloat(), position.ArrayAt(2).AsFloat()};
        const auto& normal = entry["normal"];
        if (normal.ArraySize() == 3)
            placement.normal = {normal.ArrayAt(0).AsFloat(),
                normal.ArrayAt(1).AsFloat(), normal.ArrayAt(2).AsFloat()};
        placement.yawDegrees = entry["yawDegrees"].AsFloat();
        placement.scale = entry.Has("scale") ?
            std::max(0.f, entry["scale"].AsFloat()) : 1.f;
        if (std::isfinite(placement.position.x) &&
            std::isfinite(placement.position.y) &&
            std::isfinite(placement.position.z) &&
            std::isfinite(placement.scale)) placements.push_back(std::move(placement));
    }
    MarkConfigurationDirty();
    m_preparedRevision=UINT64_MAX; m_prepared.clear(); m_loadedPrepared=false;
    const auto signature = [&] {
        Engine::Serialization::PrefabLoadProfile::Stage stage("placement_restore_signature");
        return GetPlacementSignature();
    }();
    // No placement changes occur between normalization and baked validation.
    // Reuse this signature rather than rebuilding the ID set and hashing again.
    if(value["bakedInstances"].IsString()) LoadPrepared(value["bakedInstances"].AsString(), signature);
}

void TerrainObjectLayer::InstallPrepared(TerrainObjectLayer&& source)
{
    layerName = std::move(source.layerName);
    viewDistance = source.viewDistance;
    lod1Distance = source.lod1Distance;
    lod2Distance = source.lod2Distance;
    lodHysteresis = source.lodHysteresis;
    screenLodSelection = source.screenLodSelection;
    distanceLodOverride = source.distanceLodOverride;
    screenLod1Pixels = source.screenLod1Pixels;
    screenLod2Pixels = source.screenLod2Pixels;
    screenLodHysteresis = source.screenLodHysteresis;
    shadowDistance = source.shadowDistance;
    shadowFadeWidth = source.GetShadowFadeWidth();
    shadowLod = source.shadowLod;
    objectLods = std::move(source.objectLods);
    placements = std::move(source.placements);
    m_prepared = std::move(source.m_prepared);
    m_preparedSignature = source.m_preparedSignature;
    m_preparedSpan = source.m_preparedSpan;
    m_preparedCount = placements.size();
    m_loadedPrepared = source.m_loadedPrepared;
    MarkConfigurationDirty();
    m_preparedData = placements.data();
    m_preparedRevision = GetConfigurationRevision();
}

bool TerrainObjectLayer::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    ui.ValueLabel("Layer", layerName.c_str());
    changed |= ui.DragFloat("Object view distance (m)", &viewDistance,
        .25f, 1.f, 100000.f);
    ui.Tooltip("Objects beyond this distance are hidden. Distances are measured to each object tile center.");
    viewDistance = std::clamp(viewDistance, 1.f, 100000.f);
    const auto definitions = GetDefinedObjectLods();
    if (definitions.empty()) ui.Label("No terrain object assets defined.");
    for (const auto& [asset, meshes] : definitions)
    {
        ui.ValueLabel("Object asset", asset.c_str());
        auto distances = GetObjectLodDistances(asset, meshes.size());
        std::vector<std::string> labels;
        std::vector<const char*> labelPointers;
        for (size_t i = 0; i < meshes.size(); ++i) labels.push_back("LOD " + std::to_string(i));
        for (const auto& label : labels) labelPointers.push_back(label.c_str());
        const std::string sliderLabel = "Object LOD distances##" + asset;
        if (ui.LodRangeSlider(sliderLabel.c_str(), distances.data(), static_cast<int>(distances.size()), viewDistance, labelPointers.data()))
        {
            objectLods[asset].distances = std::move(distances);
            changed = true;
        }
        const std::string header = "LOD mesh definitions##" + asset;
        if (ui.CollapsingHeader(header.c_str(), false))
        {
            ui.Label("One mesh per LOD, highest detail first (up to 16). Drag segment boundaries to set distances.");
            auto editedMeshes = meshes;
            bool edited = false;
            for (size_t i = 0; i < meshes.size(); ++i)
            {
                char path[1024]{};
                std::memcpy(path, meshes[i].data(), std::min(meshes[i].size(), sizeof(path) - 1));
                const std::string field = labels[i] + " mesh##" + asset;
                if (ui.InputText(field.c_str(), path, sizeof(path)) && path[0])
                { editedMeshes[i] = path; edited = true; }
            }
            const std::string add = "Add LOD##" + asset, remove = "Remove last LOD##" + asset;
            if (meshes.size() < MaximumObjectLods && ui.Button(add.c_str()))
            { editedMeshes.push_back(meshes.back()); edited = true; }
            if (meshes.size() > 1 && ui.Button(remove.c_str()))
            { editedMeshes.pop_back(); edited = true; }
            if (edited)
            {
                auto& settings = objectLods[asset];
                settings.meshes = std::move(editedMeshes);
                settings.distances = std::move(distances);
                settings.distances = GetObjectLodDistances(asset, settings.meshes.size());
                changed = true;
            }
        }
    }
    changed |= ui.DragFloat("LOD transition buffer (m)", &lodHysteresis,
        .1f, 0.f, 10.f);
    changed |= ui.DragFloat("Shadow distance (m)", &shadowDistance,
        .25f, 0.f, viewDistance);
    ui.Tooltip("Objects stop casting shadows beyond this distance. Zero disables object shadows. Shadow quality is set in Preferences > Rendering > Realtime Shadows.");
    changed |= ui.DragFloat("Prefab shadow fade width (m)", &shadowFadeWidth,
        .25f, 0.f, shadowDistance);
    ui.Tooltip("Fades prefab shadows within the last metres of their range. A wider fade smooths transitions but makes shadows diminish earlier. Zero gives a hard cutoff. Grass tiles use GrassField's distance fade width.");
    changed |= ui.Checkbox("Screen size LOD", &screenLodSelection);
    if(screenLodSelection)
    {
        changed |= ui.Checkbox("Override with authored distances", &distanceLodOverride);
        changed |= ui.DragFloat("LOD 1 pixels", &screenLod1Pixels, 1.f, 1.f, 100000.f);
        changed |= ui.DragFloat("LOD 2 pixels", &screenLod2Pixels, 1.f, 1.f, screenLod1Pixels);
        changed |= ui.DragFloat("Screen LOD hysteresis", &screenLodHysteresis, .01f, 0.f, .49f);
    }
    const char* shadowLods[]={"Distance LODs", "Fixed LOD 0", "Fixed LOD 1", "Fixed LOD 2"};
    int shadowChoice=std::clamp(shadowLod+1,0,3);
    if(ui.Combo("Shadow geometry", &shadowChoice, shadowLods, 4))
    { shadowLod=shadowChoice-1; changed=true; }
    ui.Tooltip("Fixed shadow geometry keeps the canopy silhouette stable as the camera moves. Visible object LODs remain independent. Missing levels use the last available level.");
    viewDistance = std::clamp(viewDistance, 1.f, 100000.f);
    lod1Distance = std::clamp(lod1Distance, 0.f, viewDistance);
    lod2Distance = std::clamp(lod2Distance, lod1Distance, viewDistance);
    shadowDistance = std::clamp(shadowDistance, 0.f, viewDistance);
    shadowFadeWidth = GetShadowFadeWidth();
    shadowLod = std::clamp(shadowLod, -1, 2);
    const std::string count = "Stored placements: " +
        std::to_string(placements.size());
    ui.Label(count.c_str());
    if (changed) MarkConfigurationDirty();
    return changed;
}
}
