#include "TerrainObjectLayer.h"
#include "Engine/Editor/UI/IEditorUi.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include "Core/Serialization/HexBytes.h"
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
    RegisterField("shadowDistance", shadowDistance, "Object LOD");
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
void TerrainObjectLayer::LoadPrepared(const std::string& text)
{
    if(text.size()!=2*(20+placements.size()*60)) return;
    std::vector<uint8_t> bytes; if(!Engine::Serialization::DecodeHex(text,bytes)) return;
    size_t cursor=0;
    const auto read=[&](auto& value) { std::memcpy(&value,bytes.data()+cursor,sizeof(value)); cursor+=sizeof(value); };
    uint32_t magic=0,count=0; float span=0; uint64_t signature=0;
    read(magic); read(span); read(count); read(signature);
    if(magic!=0x31494c54 || !std::isfinite(span) || span<1 || count!=placements.size() || signature!=GetPlacementSignature()) return;
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
    GetPreparedTiles(m_preparedSpan);
    JsonValue out = Component::Serialize();
    out.Set("bakedInstances",JsonValue(SerializePrepared()));
    out.Set("layerName", JsonValue(layerName));
    JsonValue entries = JsonValue::MakeArray();
    for (const Placement& placement : placements)
        entries.Push(JsonValue::MakeObject()
            .Set("id", JsonValue(std::to_string(placement.id)))
            .Set("asset", JsonValue(placement.asset))
            .Set("position", JsonValue::MakeArray()
                .Push(JsonValue(placement.position.x))
                .Push(JsonValue(placement.position.y))
                .Push(JsonValue(placement.position.z)))
            .Set("normal", JsonValue::MakeArray()
                .Push(JsonValue(placement.normal.x))
                .Push(JsonValue(placement.normal.y))
                .Push(JsonValue(placement.normal.z)))
            .Set("yawDegrees", JsonValue(placement.yawDegrees))
            .Set("scale", JsonValue(placement.scale)));
    return out.Set("placements", std::move(entries));
}

void TerrainObjectLayer::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    viewDistance = std::clamp(viewDistance, 1.f, 100000.f);
    lod1Distance = std::clamp(lod1Distance, 0.f, viewDistance);
    lod2Distance = std::clamp(lod2Distance, lod1Distance, viewDistance);
    lodHysteresis = std::clamp(lodHysteresis, 0.f, 10.f);
    shadowDistance = std::clamp(shadowDistance, 0.f, viewDistance);
    layerName = value.Has("layerName") ? value["layerName"].AsString() : "Objects";
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
    GetPlacementSignature();
    if(value["bakedInstances"].IsString()) LoadPrepared(value["bakedInstances"].AsString());
}

void TerrainObjectLayer::InstallPrepared(TerrainObjectLayer&& source)
{
    layerName = std::move(source.layerName);
    viewDistance = source.viewDistance;
    lod1Distance = source.lod1Distance;
    lod2Distance = source.lod2Distance;
    lodHysteresis = source.lodHysteresis;
    shadowDistance = source.shadowDistance;
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
    changed |= ui.DragFloat("View distance (m)", &viewDistance,
        .25f, 1.f, 100000.f);
    changed |= ui.DragFloat("LOD 1 starts (m)", &lod1Distance,
        .25f, 0.f, viewDistance);
    changed |= ui.DragFloat("LOD 2 starts (m)", &lod2Distance,
        .25f, lod1Distance, viewDistance);
    changed |= ui.DragFloat("LOD hysteresis (m)", &lodHysteresis,
        .1f, 0.f, 10.f);
    changed |= ui.DragFloat("Shadow distance (m)", &shadowDistance,
        .25f, 0.f, viewDistance);
    viewDistance = std::clamp(viewDistance, 1.f, 100000.f);
    lod1Distance = std::clamp(lod1Distance, 0.f, viewDistance);
    lod2Distance = std::clamp(lod2Distance, lod1Distance, viewDistance);
    shadowDistance = std::clamp(shadowDistance, 0.f, viewDistance);
    const std::string count = "Stored placements: " +
        std::to_string(placements.size());
    ui.Label(count.c_str());
    if (changed) MarkConfigurationDirty();
    return changed;
}
}
