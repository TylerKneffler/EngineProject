#include "Mesh.h"
#include "Core/Rendering/SharedMeshBuffers.h"
#include "Core/Threading/JobScheduler.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Model/MeshPackAsset.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <fstream>
#include <algorithm>
#include <sstream>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <glm/gtc/quaternion.hpp>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Engine::Components
{
namespace
{
Engine::Serialization::JsonValue FloatArray(const std::vector<float>& values)
{
    Engine::Serialization::JsonValue result = Engine::Serialization::JsonValue::MakeArray();
    for (float value : values) result.Push(Engine::Serialization::JsonValue(value));
    return result;
}

Engine::Serialization::JsonValue Vec3Array(const std::vector<glm::vec3>& values)
{
    Engine::Serialization::JsonValue result = Engine::Serialization::JsonValue::MakeArray();
    for (const glm::vec3& value : values)
        result.Push(Engine::Serialization::JsonValue::MakeArray().Push(Engine::Serialization::JsonValue(value.x))
            .Push(Engine::Serialization::JsonValue(value.y)).Push(Engine::Serialization::JsonValue(value.z)));
    return result;
}

std::vector<glm::vec3> ReadVec3Array(const Engine::Serialization::JsonValue& value)
{
    std::vector<glm::vec3> result;
    for (size_t i = 0; i < value.ArraySize(); ++i)
    {
        const Engine::Serialization::JsonValue& item = value.ArrayAt(i);
        result.emplace_back(item.ArrayAt(0).AsFloat(), item.ArrayAt(1).AsFloat(),
            item.ArrayAt(2).AsFloat());
    }
    return result;
}

Engine::Serialization::JsonValue Vec4Array(const std::vector<glm::vec4>& values)
{
    auto result = Engine::Serialization::JsonValue::MakeArray();
    for (const glm::vec4& value : values)
        result.Push(Engine::Serialization::JsonValue::MakeArray()
            .Push(Engine::Serialization::JsonValue(value.x))
            .Push(Engine::Serialization::JsonValue(value.y))
            .Push(Engine::Serialization::JsonValue(value.z))
            .Push(Engine::Serialization::JsonValue(value.w)));
    return result;
}

std::vector<glm::vec4> ReadVec4Array(const Engine::Serialization::JsonValue& value)
{
    std::vector<glm::vec4> result;
    for (size_t i = 0; i < value.ArraySize(); ++i)
    {
        const auto& item = value.ArrayAt(i);
        result.emplace_back(item.ArrayAt(0).AsFloat(), item.ArrayAt(1).AsFloat(),
            item.ArrayAt(2).AsFloat(), item.ArrayAt(3).AsFloat());
    }
    return result;
}

}
bool Mesh::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    ui.ValueLabel("Asset", m_filePath.empty() ? "(generated mesh)" : m_filePath.c_str());
    const std::string vertexCount = std::to_string(GetVertexCount());
    const std::string triangleCount = std::to_string(
        m_indices.empty() ? m_vertices.size() / 3 : m_indices.size() / 3);
    ui.ValueLabel("Vertices", vertexCount.c_str());
    ui.ValueLabel("Triangles", triangleCount.c_str());
    ui.ValueLabel("GPU Buffer", m_ready ? "Ready" : "Not prepared");

    if (m_hasBounds)
    {
        char minimum[96]{}, maximum[96]{};
        std::snprintf(minimum, sizeof(minimum), "%.3f, %.3f, %.3f",
            m_boundsMin.x, m_boundsMin.y, m_boundsMin.z);
        std::snprintf(maximum, sizeof(maximum), "%.3f, %.3f, %.3f",
            m_boundsMax.x, m_boundsMax.y, m_boundsMax.z);
        ui.ValueLabel("Bounds Minimum", minimum);
        ui.ValueLabel("Bounds Maximum", maximum);
    }

    const std::string morphCount = std::to_string(m_morphTargets.size());
    ui.ValueLabel("Morph Targets", morphCount.c_str());
    bool changed = false;
    changed=ui.Checkbox("Use Light Probes",&useLightProbes)||changed;
    changed=ui.Checkbox("GPU LOD Selection",&gpuLodSelection)||changed;
    changed=ui.Checkbox("Screen size LOD",&screenLodSelection)||changed;
    if(screenLodSelection)
    {
        changed=ui.Checkbox("Override with authored distances",&distanceLodOverride)||changed;
        changed=ui.DragFloat("LOD 1 pixels",&screenLod1Pixels,1.f,1.f,100000.f)||changed;
        changed=ui.DragFloat("LOD 2 pixels",&screenLod2Pixels,1.f,1.f,screenLod1Pixels)||changed;
        changed=ui.DragFloat("Screen LOD hysteresis",&screenLodHysteresis,.01f,0.f,.49f)||changed;
    }
    if(gpuLodSelection || screenLodSelection)
    {
        changed=ui.DragFloat("LOD 1 Distance",&gpuLod1Distance,.1f,0.f,100000.f)||changed;
        changed=ui.DragFloat("LOD 2 Distance",&gpuLod2Distance,.1f,gpuLod1Distance,100000.f)||changed;
    }
    if (!m_morphWeights.empty() &&
        ui.PropertyGroupHeader("Morph Weights", false))
    {
        for (size_t index = 0; index < m_morphWeights.size(); ++index)
        {
            ui.PushId(&m_morphWeights[index]);
            const std::string label = "Target " + std::to_string(index);
            changed = ui.DragFloat(label.c_str(), &m_morphWeights[index],
                0.01f, -1.f, 1.f) || changed;
            ui.PopId();
        }
    }
    if (changed)
    {
        SyncMorphWeights();
    }
    return changed;
}

Mesh::JsonValue Mesh::Serialize() const
{
    JsonValue result = Component::Serialize();
    if (m_morphTargets.empty()) return result;
    result.Set("morphNodeIndex", JsonValue(static_cast<int>(m_morphNodeIndex)));
    result.Set("morphWeights", FloatArray(m_morphWeights));
    if (!m_morphFilePath.empty())
    {
        result.Set("morphFile", JsonValue(m_morphFilePath));
        return result;
    }
    JsonValue targets = JsonValue::MakeArray();
    for (const MorphTarget& target : m_morphTargets)
    {
        JsonValue channels = JsonValue::MakeArray();
        for (const auto& channel : target.uvChannels)
            channels.Push(Vec4Array(channel));
        targets.Push(JsonValue::MakeObject()
            .Set("name", JsonValue(target.name))
            .Set("positions", Vec3Array(target.positions))
            .Set("normals", Vec3Array(target.normals))
            .Set("tangents", Vec3Array(target.tangents))
            .Set("uv", Vec4Array(target.uv))
            .Set("uvChannels", std::move(channels)));
    }
    result.Set("morphTargets", std::move(targets));
    return result;
}

void Mesh::DeserializeLegacyMorphTargets(const JsonValue& value)
{
    std::vector<MorphTarget> targets;
    const JsonValue& list = value["targets"];
    for (size_t i = 0; i < list.ArraySize(); ++i)
    {
        MorphTarget target;
        if (list.ArrayAt(i).Has("name"))
            target.name = list.ArrayAt(i)["name"].AsString();
        target.positions = ReadVec3Array(list.ArrayAt(i)["positions"]);
        target.normals = ReadVec3Array(list.ArrayAt(i)["normals"]);
        target.tangents = ReadVec3Array(list.ArrayAt(i)["tangents"]);
        target.uv = ReadVec4Array(list.ArrayAt(i)["uv"]);
        const JsonValue& channels = list.ArrayAt(i)["uvChannels"];
        for (size_t channel = 0; channel < target.uvChannels.size() &&
            channel < channels.ArraySize(); ++channel)
            target.uvChannels[channel] = ReadVec4Array(channels.ArrayAt(channel));
        targets.push_back(std::move(target));
    }
    std::vector<float> weights;
    for (size_t i = 0; i < value["weights"].ArraySize(); ++i)
        weights.push_back(value["weights"].ArrayAt(i).AsFloat());
    SetMorphData(static_cast<unsigned>(value["nodeIndex"].AsInt()),
        std::move(targets), std::move(weights));
}

void Mesh::Deserialize(const JsonValue& v)
{
    DeferGraphicsBuffers();
    if(v.Has("foliageWindStrength")) foliageWindStrength=std::clamp(v["foliageWindStrength"].AsFloat(),0.f,1.f);
    if(v.Has("gpuLodSelection")) gpuLodSelection=v["gpuLodSelection"].AsBool();
    if(v.Has("screenLodSelection")) screenLodSelection=v["screenLodSelection"].AsBool();
    if(v.Has("distanceLodOverride")) distanceLodOverride=v["distanceLodOverride"].AsBool();
    if(v.Has("screenLod1Pixels")) screenLod1Pixels=v["screenLod1Pixels"].AsFloat();
    if(v.Has("screenLod2Pixels")) screenLod2Pixels=v["screenLod2Pixels"].AsFloat();
    if(v.Has("screenLodHysteresis")) screenLodHysteresis=v["screenLodHysteresis"].AsFloat();
    if(v.Has("gpuLod1Distance")) gpuLod1Distance=std::max(0.f,v["gpuLod1Distance"].AsFloat());
    if(v.Has("gpuLod2Distance")) gpuLod2Distance=std::max(gpuLod1Distance,v["gpuLod2Distance"].AsFloat());
    if (v.Has("file") && !v["file"].AsString().empty())
        LoadFromFile(v["file"].AsString());
    if (v.Has("morphFile") && !v["morphFile"].AsString().empty())
    {
        std::vector<float> weights;
        for (size_t index = 0; index < v["morphWeights"].ArraySize(); ++index)
            weights.push_back(v["morphWeights"].ArrayAt(index).AsFloat());
        if (!LoadMorphFile(v["morphFile"].AsString(),
                static_cast<unsigned>(v["morphNodeIndex"].AsInt()),
                std::move(weights)))
            throw std::runtime_error("Mesh: invalid morph file: " +
                v["morphFile"].AsString());
    }
    else if (v.Has("morphTargets"))
    {
        JsonValue legacy = JsonValue::MakeObject()
            .Set("nodeIndex", v["morphNodeIndex"])
            .Set("weights", v["morphWeights"])
            .Set("targets", v["morphTargets"]);
        DeserializeLegacyMorphTargets(legacy);
    }
}

void Mesh::OnAfterDeserialize(IGraphicsProvider* graphicsProvider)
{
    if (graphicsProvider && !DeferGraphicsPreparation(graphicsProvider))
        PrepareGraphicsResources(graphicsProvider);
}

void Mesh::PrepareGraphicsResources(IGraphicsProvider* graphicsProvider)
{
    if (graphicsProvider && !m_ready)
        CreateBuffer(graphicsProvider->GetBufferFactory());
}
} // namespace Engine::Components

