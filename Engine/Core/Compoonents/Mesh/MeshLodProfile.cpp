#include "Core/Compoonents/Mesh/MeshLodProfile.h"

#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Model/MeshLodGenerator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

namespace
{
uint64_t HashBytes(uint64_t hash, const void* source, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(source);
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

template<class T>
uint64_t HashValue(uint64_t hash, const T& value)
{
    return HashBytes(hash, &value, sizeof(value));
}

uint64_t HashString(uint64_t hash, const std::string& value)
{
    hash = HashValue(hash, value.size());
    return HashBytes(hash, value.data(), value.size());
}

bool SameAsset(const std::string& left, const std::string& right)
{
    if (left.empty() || right.empty()) return false;
    auto normalize = [](const std::string& path)
    {
        std::string result = std::filesystem::path(
            Engine::Components::Mesh::ResolveFilePath(path)).lexically_normal().generic_string();
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        return result;
    };
    return normalize(left) == normalize(right);
}
}

namespace Engine::Components
{
MeshLodProfile::MeshLodProfile()
{
    SetTypeName(COMPONENT_TYPE_NAME(MeshLodProfile));
    singlecomponent = true;
    RegisterField("baseMeshAsset", baseMeshAsset, "LOD");
    RegisterField("hysteresis", hysteresis, "LOD");
    RegisterField("shadowDistance", shadowDistance, "LOD");
    RegisterField("screenSizeSelection", screenSizeSelection, "LOD");
    RegisterField("distanceOverride", distanceOverride, "LOD");
    RegisterField("ditherTransition", ditherTransition, "LOD");
    RegisterField("transitionWidth", transitionWidth, "LOD");
    RegisterField("cullAfterLastLod", cullAfterLastLod, "LOD");
    RegisterField("finalLodCullDistance", finalLodCullDistance, "LOD");
}

std::map<std::string, ObjectLodProfileData>
MeshLodProfile::GetObjectLodProfiles() const
{
    std::string base = baseMeshAsset;
    if (base.empty() && Owner)
        if (const auto* mesh = Owner->GetComponent<Mesh>())
            base = mesh->GetFilePath();
    if (base.empty()) return {};

    ObjectLodProfileData profile;
    profile.meshes.reserve(alternateMeshes.size() + 1);
    profile.meshes.push_back(base);
    for (const auto& mesh : alternateMeshes)
    {
        if (mesh.empty()) break;
        profile.meshes.push_back(mesh);
    }
    profile.shadowMeshes = shadowMeshes;
    profile.parts = representationParts;
    profile.distances = distances;
    profile.screenThresholdsPixels = screenThresholdsPixels;
    profile.hysteresis = hysteresis;
    profile.screenSizeSelection = screenSizeSelection;
    profile.distanceOverride = distanceOverride;
    profile.ditherTransition = ditherTransition;
    profile.transitionWidth = transitionWidth;
    profile.shadowDistance = shadowDistance;
    profile.cullAfterLastLod = cullAfterLastLod;
    profile.finalLodCullDistance = finalLodCullDistance;
    return {{base, std::move(profile)}};
}

uint64_t MeshLodProfile::GeometrySignature() const
{
    uint64_t hash = 1469598103934665603ull;
    hash = HashString(hash, baseMeshAsset);
    for (const auto& mesh : alternateMeshes) hash = HashString(hash, mesh);
    return HashValue(hash, alternateMeshes.size());
}

void MeshLodProfile::ApplySelectionPolicy(Mesh& mesh,
    size_t residentLodCount) const
{
    const bool hasAlternates = residentLodCount > 1;
    const bool nextGpuLodSelection = hasAlternates;
    const bool nextScreenLodSelection = hasAlternates && screenSizeSelection;
    const bool nextDistanceOverride = distanceOverride;
    float nextLod1Distance = mesh.gpuLod1Distance;
    float nextLod2Distance = mesh.gpuLod2Distance;
    if (!distances.empty() && std::isfinite(distances[0]))
        nextLod1Distance = std::max(0.f, distances[0]);
    if (distances.size() > 1 && std::isfinite(distances[1]))
        nextLod2Distance = std::max(nextLod1Distance, distances[1]);
    float nextScreenLod1Pixels = mesh.screenLod1Pixels;
    float nextScreenLod2Pixels = mesh.screenLod2Pixels;
    if (!screenThresholdsPixels.empty() &&
        std::isfinite(screenThresholdsPixels[0]))
        nextScreenLod1Pixels = std::clamp(screenThresholdsPixels[0], 0.f, 100000.f);
    if (screenThresholdsPixels.size() > 1 &&
        std::isfinite(screenThresholdsPixels[1]))
        nextScreenLod2Pixels = std::min(nextScreenLod1Pixels,
            std::clamp(screenThresholdsPixels[1], 0.f, 100000.f));
    const float nextHysteresis = std::clamp(std::isfinite(hysteresis)
        ? hysteresis : .1f, 0.f, .5f);
    const float nextShadowDistance = std::max(0.f,
        std::isfinite(shadowDistance) ? shadowDistance : 0.f);
    const float nextTransitionWidth = ditherTransition
        ? std::clamp(std::isfinite(transitionWidth) ? transitionWidth : .1f,
            0.f, .5f) : 0.f;
    const float lastLodDistance = residentLodCount > 2 ? nextLod2Distance
        : residentLodCount > 1 ? nextLod1Distance : 0.f;
    const float nextCullDistance = cullAfterLastLod
        ? std::max(std::max(0.f, std::isfinite(finalLodCullDistance)
            ? finalLodCullDistance : 0.f), lastLodDistance + .001f) : 0.f;
    const bool changed = mesh.gpuLodSelection != nextGpuLodSelection ||
        mesh.screenLodSelection != nextScreenLodSelection ||
        mesh.distanceLodOverride != nextDistanceOverride ||
        mesh.gpuLod1Distance != nextLod1Distance ||
        mesh.gpuLod2Distance != nextLod2Distance ||
        mesh.screenLod1Pixels != nextScreenLod1Pixels ||
        mesh.screenLod2Pixels != nextScreenLod2Pixels ||
        mesh.screenLodHysteresis != nextHysteresis ||
        mesh.runtimeShadowDistance != nextShadowDistance ||
        mesh.lodTransitionWidth != nextTransitionWidth ||
        mesh.cullAfterLastLod != (nextCullDistance > 0.f) ||
        mesh.finalLodCullDistance != nextCullDistance;
    mesh.gpuLodSelection = nextGpuLodSelection;
    mesh.screenLodSelection = nextScreenLodSelection;
    mesh.distanceLodOverride = nextDistanceOverride;
    mesh.gpuLod1Distance = nextLod1Distance;
    mesh.gpuLod2Distance = nextLod2Distance;
    mesh.screenLod1Pixels = nextScreenLod1Pixels;
    mesh.screenLod2Pixels = nextScreenLod2Pixels;
    mesh.screenLodHysteresis = nextHysteresis;
    mesh.runtimeShadowDistance = nextShadowDistance;
    mesh.lodTransitionWidth = nextTransitionWidth;
    mesh.cullAfterLastLod = nextCullDistance > 0.f;
    mesh.finalLodCullDistance = nextCullDistance;
    if (changed) mesh.MarkConfigurationDirty();
}

void MeshLodProfile::PrepareGraphicsResources(IGraphicsProvider* provider)
{
    if (!provider || !Owner) return;
    auto* mesh = Owner->GetComponent<Mesh>();
    if (!mesh) return;
    if (Owner->GetComponent<Engine::Components::SkinnedMesh>()) return;
    if (!mesh->IsReady()) mesh->PrepareGraphicsResources(provider);
    if (!mesh->IsReady() || mesh->HasMorphTargets() || mesh->HasSurfaceOffsets())
        return;

    const std::string base = baseMeshAsset.empty()
        ? mesh->GetFilePath() : baseMeshAsset;
    if (!SameAsset(base, mesh->GetFilePath())) return;
    if (alternateMeshes.empty() && !m_hasPreparedSignature)
    {
        ApplySelectionPolicy(*mesh, 1);
        return;
    }
    size_t targetLods = 1;
    while (targetLods < 3 && targetLods <= alternateMeshes.size() &&
        !alternateMeshes[targetLods - 1].empty())
        ++targetLods;
    uint64_t signature = GeometrySignature();
    signature = HashString(signature, mesh->GetFilePath());
    if (!m_hasPreparedSignature || signature != m_preparedSignature)
    {
        mesh->ClearRuntimeLods();
        m_preparedSignature = signature;
        m_hasPreparedSignature = true;
        m_nextAlternate = 0;
        m_nextBoundsScan = 0;
        m_hasScannedBounds = mesh->HasBounds();
        if (m_hasScannedBounds)
        {
            m_scannedBoundsMin = mesh->GetBoundsMin();
            m_scannedBoundsMax = mesh->GetBoundsMax();
            mesh->SetRuntimeLodBoundsEnvelope(m_scannedBoundsMin,
                m_scannedBoundsMax);
        }
        m_preparationFailed = false;
        m_lastError.clear();
    }

    if (m_preparationFailed)
    {
        ApplySelectionPolicy(*mesh, 1);
        return;
    }

    const size_t alternateCount = targetLods - 1;
    if (m_nextBoundsScan < alternateCount)
    {
        try
        {
            Mesh boundsSource;
            boundsSource.LoadFromFile(alternateMeshes[m_nextBoundsScan]);
            if (!boundsSource.HasBounds())
                throw std::runtime_error("LOD mesh has no conservative bounds");
            m_scannedBoundsMin = m_hasScannedBounds
                ? glm::min(m_scannedBoundsMin, boundsSource.GetBoundsMin())
                : boundsSource.GetBoundsMin();
            m_scannedBoundsMax = m_hasScannedBounds
                ? glm::max(m_scannedBoundsMax, boundsSource.GetBoundsMax())
                : boundsSource.GetBoundsMax();
            m_hasScannedBounds = true;
            mesh->SetRuntimeLodBoundsEnvelope(m_scannedBoundsMin,
                m_scannedBoundsMax);
            ++m_nextBoundsScan;
        }
        catch (const std::exception& error)
        {
            m_preparationFailed = true;
            m_lastError = error.what();
        }
        catch (...)
        {
            m_preparationFailed = true;
            m_lastError = "Unknown error while scanning LOD bounds";
        }
        if (m_preparationFailed)
        {
            ApplySelectionPolicy(*mesh, 1);
            return;
        }
        if (m_nextBoundsScan < alternateCount)
        {
            DeferGraphicsPreparation(provider);
            return;
        }
    }

    if (m_nextAlternate < targetLods - 1)
    {
        const size_t alternateIndex = m_nextAlternate;
        const std::string& path = alternateMeshes[alternateIndex];
        try
        {
            if (SameAsset(path, base))
                throw std::runtime_error("LOD profile repeats its base mesh asset");
            for (size_t previous = 0; previous < alternateIndex; ++previous)
                if (SameAsset(path, alternateMeshes[previous]))
                    throw std::runtime_error("LOD profile repeats an alternate mesh asset");
            auto alternate = std::make_unique<Mesh>();
            alternate->LoadFromFile(path);
            alternate->foliageWindStrength = mesh->foliageWindStrength;
            alternate->deformationBoundsPadding = mesh->deformationBoundsPadding;
            alternate->PrepareGraphicsResources(provider);
            if (!alternate->IsReady() ||
                !mesh->StoreRuntimeLod(static_cast<int>(alternateIndex + 1),
                    std::move(alternate)))
                throw std::runtime_error("LOD mesh could not be installed on its base mesh");
            ++m_nextAlternate;
        }
        catch (const std::exception& error)
        {
            m_preparationFailed = true;
            m_lastError = error.what();
        }
        catch (...)
        {
            m_preparationFailed = true;
            m_lastError = "Unknown error while preparing LOD mesh";
        }
        ApplySelectionPolicy(*mesh, m_nextAlternate + 1);
        if (!m_preparationFailed && m_nextAlternate < targetLods - 1)
            DeferGraphicsPreparation(provider);
        return;
    }

    ApplySelectionPolicy(*mesh, m_nextAlternate + 1);
}

void MeshLodProfile::OnDestroy()
{
    if (!Owner) return;
    if (auto* mesh = Owner->GetComponent<Mesh>())
    {
        mesh->ClearRuntimeLods();
        mesh->gpuLodSelection = false;
        mesh->screenLodSelection = false;
        mesh->cullAfterLastLod = false;
        mesh->finalLodCullDistance = 0.f;
        mesh->lodTransitionWidth = .25f;
        mesh->runtimeShadowDistance = 0.f;
        mesh->MarkConfigurationDirty();
    }
}

Engine::Serialization::JsonValue MeshLodProfile::Serialize() const
{
    using JsonValue = Engine::Serialization::JsonValue;
    JsonValue output = Component::Serialize();
    const auto toArray = [](const auto& values)
    {
        JsonValue array = JsonValue::MakeArray(values.size());
        for (const auto& value : values) array.Push(JsonValue(value));
        return array;
    };
    output.Set("alternateMeshes", toArray(alternateMeshes));
    output.Set("shadowMeshes", toArray(shadowMeshes));
    output.Set("lodDistances", toArray(distances));
    output.Set("screenThresholdsPixels", toArray(screenThresholdsPixels));
    JsonValue parts = JsonValue::MakeArray(representationParts.size());
    for (const auto& part : representationParts)
    {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("key", JsonValue(part.key));
        entry.Set("meshes", toArray(part.meshes));
        parts.Push(std::move(entry));
    }
    output.Set("representationParts", std::move(parts));
    return output;
}

void MeshLodProfile::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    const auto readStrings = [&](const char* name, std::vector<std::string>& out)
    {
        out.clear();
        const auto& array = value[name];
        for (size_t index = 0; index < std::min<size_t>(array.ArraySize(), 31); ++index)
            if (array.ArrayAt(index).IsString()) out.push_back(array.ArrayAt(index).AsString());
    };
    const auto readFloats = [&](const char* name, std::vector<float>& out)
    {
        out.clear();
        const auto& array = value[name];
        for (size_t index = 0; index < std::min<size_t>(array.ArraySize(), 31); ++index)
            out.push_back(array.ArrayAt(index).AsFloat());
    };
    readStrings("alternateMeshes", alternateMeshes);
    readStrings("shadowMeshes", shadowMeshes);
    readFloats("lodDistances", distances);
    readFloats("screenThresholdsPixels", screenThresholdsPixels);
    representationParts.clear();
    const auto& serializedParts = value["representationParts"];
    for (size_t index = 0; index < std::min<size_t>(serializedParts.ArraySize(), 32); ++index)
    {
        const auto& entry = serializedParts.ArrayAt(index);
        if (!entry.IsObject()) continue;
        ObjectLodPartProfileData part;
        if (entry["key"].IsString()) part.key = entry["key"].AsString();
        const auto& paths = entry["meshes"];
        for (size_t path = 0; path < std::min<size_t>(paths.ArraySize(), 3); ++path)
            if (paths.ArrayAt(path).IsString())
                part.meshes.push_back(paths.ArrayAt(path).AsString());
        if (!part.key.empty() && !part.meshes.empty())
            representationParts.push_back(std::move(part));
    }
    m_hasPreparedSignature = false;
    m_lastError.clear();
    MarkConfigurationDirty();
}

bool MeshLodProfile::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = Component::DrawProperties(ui);
    ui.ValueLabel("Base mesh", baseMeshAsset.empty()
        ? (Owner && Owner->GetComponent<Mesh>()
            ? Owner->GetComponent<Mesh>()->GetFilePath().c_str() : "(no mesh)")
        : baseMeshAsset.c_str());
    const auto editPaths = [&](const char* title,
        std::vector<std::string>& paths, size_t maximum)
    {
        ui.Label(title);
        for (size_t index = 0; index < paths.size(); ++index)
        {
            ui.PushId((title + std::to_string(index)).c_str());
            char buffer[1024]{};
            std::memcpy(buffer, paths[index].data(),
                std::min(paths[index].size(), sizeof(buffer) - 1));
            const std::string label = std::string(title) + " " +
                std::to_string(index + 1);
            if (ui.InputText(label.c_str(), buffer, sizeof(buffer)) && buffer[0])
            {
                paths[index] = buffer;
                changed = true;
            }
            if (ui.Button((std::string("Generate LOD ") +
                    std::to_string(index + 1)).c_str()))
            {
                const std::string sourcePath = index == 0
                    ? (baseMeshAsset.empty() && Owner && Owner->GetComponent<Mesh>()
                        ? Owner->GetComponent<Mesh>()->GetFilePath() : baseMeshAsset)
                    : paths[index - 1];
                std::vector<Engine::Model::MeshLodGenerator::Result> generated;
                const std::string suffix = "_LOD" + std::to_string(index + 1);
                if (Engine::Model::MeshLodGenerator::Generate(sourcePath, 1,
                        .5f, generated, m_lastError, suffix))
                {
                    paths[index] = generated.front().path;
                    m_lastError.clear();
                    changed = true;
                }
            }
            ui.PopId();
        }
        if (paths.size() < maximum && ui.Button((std::string("Add ") + title).c_str()))
        {
            paths.emplace_back();
            changed = true;
        }
        if (!paths.empty() && ui.Button((std::string("Remove last ") + title).c_str()))
        {
            paths.pop_back();
            changed = true;
        }
    };
    editPaths("Alternate mesh", alternateMeshes, 2);
    editPaths("Shadow mesh", shadowMeshes, 3);
    if (ui.Button("Add representation part"))
    {
        ObjectLodPartProfileData part;
        part.key = "part_" + std::to_string(representationParts.size() + 1);
        representationParts.push_back(std::move(part));
        changed = true;
    }
    for (size_t partIndex = 0; partIndex < representationParts.size(); ++partIndex)
    {
        auto& part = representationParts[partIndex];
        const std::string partLabel = "Part " + std::to_string(partIndex + 1);
        char key[256]{};
        std::memcpy(key, part.key.data(), std::min(part.key.size(), sizeof(key) - 1));
        if (ui.InputText((partLabel + " key").c_str(), key, sizeof(key)) && key[0])
        {
            part.key = key;
            changed = true;
        }
        for (size_t lod = 0; lod < part.meshes.size(); ++lod)
        {
            char path[1024]{};
            std::memcpy(path, part.meshes[lod].data(),
                std::min(part.meshes[lod].size(), sizeof(path) - 1));
            if (ui.InputText((partLabel + " LOD " + std::to_string(lod)).c_str(),
                path, sizeof(path)) && path[0])
            {
                part.meshes[lod] = path;
                changed = true;
            }
        }
        if (part.meshes.size() < 3 && ui.Button(("Add " + partLabel + " LOD").c_str()))
        {
            part.meshes.emplace_back();
            changed = true;
        }
        if (!part.meshes.empty() && ui.Button(("Remove last " + partLabel + " LOD").c_str()))
        {
            part.meshes.pop_back();
            changed = true;
        }
    }
    if (!representationParts.empty() && ui.Button("Remove last representation part"))
    {
        representationParts.pop_back();
        changed = true;
    }
    if (!m_lastError.empty()) ui.ValueLabel("LOD preparation error", m_lastError.c_str());
    const size_t previousDistanceCount = distances.size();
    while (distances.size() < alternateMeshes.size())
        distances.push_back(distances.empty() ? 8.f : distances.back() * 2.f);
    while (distances.size() > alternateMeshes.size()) distances.pop_back();
    changed |= previousDistanceCount != distances.size();
    const size_t previousPixelCount = screenThresholdsPixels.size();
    while (screenThresholdsPixels.size() < alternateMeshes.size())
        screenThresholdsPixels.push_back(screenThresholdsPixels.empty()
            ? 300.f : std::max(1.f, screenThresholdsPixels.back() * .25f));
    while (screenThresholdsPixels.size() > alternateMeshes.size())
        screenThresholdsPixels.pop_back();
    changed |= previousPixelCount != screenThresholdsPixels.size();
    for (size_t index = 0; index < distances.size(); ++index)
    {
        const std::string label = "LOD " + std::to_string(index + 1) + " distance (m)";
        if (ui.DragFloat(label.c_str(), &distances[index], .25f, 0.f, 100000.f))
            changed = true;
        if (index && distances[index] < distances[index - 1])
            distances[index] = distances[index - 1];
    }
    for (size_t index = 0; index < screenThresholdsPixels.size(); ++index)
    {
        const std::string label = "LOD " + std::to_string(index + 1) + " screen pixels";
        if (ui.DragFloat(label.c_str(), &screenThresholdsPixels[index], .25f, 0.f, 100000.f))
            changed = true;
        if (index && screenThresholdsPixels[index] > screenThresholdsPixels[index - 1])
            screenThresholdsPixels[index] = screenThresholdsPixels[index - 1];
    }
    changed |= ui.Checkbox("Cull beyond final LOD", &cullAfterLastLod);
    changed |= ui.DragFloat("Final LOD cull distance (m)",
        &finalLodCullDistance, 1.f, 0.f, 100000.f);
    if (changed)
    {
        MarkConfigurationDirty();
        if (Owner && Owner->GetScene())
            DeferGraphicsPreparation(Owner->GetScene()->GetGraphicsProvider());
    }
    return changed;
}
}
