#include "Core/Compoonents/Mesh/MeshInstanceLayer.h"

#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Obj/ObjectLodProvider.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace
{
std::atomic<uint64_t> g_nextMeshInstancePlacementId{1};

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
        std::string result = Engine::Components::Mesh::ResolveFilePath(path);
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        return result;
    };
    return normalize(left) == normalize(right);
}

void ApplyProfile(Engine::Components::Mesh& mesh,
    const Engine::Components::ObjectLodProfileData& profile,
    size_t lodCount)
{
    const bool hasLods = lodCount > 1;
    mesh.gpuLodSelection = hasLods;
    mesh.screenLodSelection = hasLods && profile.screenSizeSelection;
    mesh.distanceLodOverride = profile.distanceOverride;
    if (!profile.distances.empty() && std::isfinite(profile.distances[0]))
        mesh.gpuLod1Distance = std::max(0.f, profile.distances[0]);
    if (profile.distances.size() > 1 && std::isfinite(profile.distances[1]))
        mesh.gpuLod2Distance = std::max(mesh.gpuLod1Distance,
            profile.distances[1]);
    if (!profile.screenThresholdsPixels.empty() &&
        std::isfinite(profile.screenThresholdsPixels[0]))
        mesh.screenLod1Pixels = std::clamp(
            profile.screenThresholdsPixels[0], 0.f, 100000.f);
    if (profile.screenThresholdsPixels.size() > 1 &&
        std::isfinite(profile.screenThresholdsPixels[1]))
        mesh.screenLod2Pixels = std::min(mesh.screenLod1Pixels,
            std::clamp(profile.screenThresholdsPixels[1], 0.f, 100000.f));
    mesh.screenLodHysteresis = std::clamp(std::isfinite(profile.hysteresis)
        ? profile.hysteresis : .1f, 0.f, .5f);
    mesh.runtimeShadowDistance = std::max(0.f,
        std::isfinite(profile.shadowDistance) ? profile.shadowDistance : 0.f);
    mesh.lodTransitionWidth = profile.ditherTransition
        ? std::clamp(std::isfinite(profile.transitionWidth)
            ? profile.transitionWidth : .1f, 0.f, .5f) : 0.f;
    float lastLodDistance = 0.f;
    if (lodCount > 1 && !profile.distances.empty())
    {
        const size_t index = std::min(lodCount - 2, profile.distances.size() - 1);
        if (std::isfinite(profile.distances[index]))
            lastLodDistance = std::max(0.f, profile.distances[index]);
    }
    mesh.finalLodCullDistance = profile.cullAfterLastLod
        ? std::max(std::max(0.f, std::isfinite(profile.finalLodCullDistance)
            ? profile.finalLodCullDistance : 0.f), lastLodDistance + .001f) : 0.f;
    mesh.cullAfterLastLod = mesh.finalLodCullDistance > 0.f;
    mesh.useBoundsForShadowCulling = true;
}
}

namespace Engine::Components
{
MeshInstanceLayer::MeshInstanceLayer()
{
    SetTypeName(COMPONENT_TYPE_NAME(MeshInstanceLayer));
    singlecomponent = true;
    RegisterField("sourceMeshAsset", sourceMeshAsset, "Instances");
    RegisterField("groupSpanMeters", groupSpanMeters, "Instances");
}

void MeshInstanceLayer::SetSourceMeshAsset(std::string asset)
{
    if (sourceMeshAsset == asset) return;
    sourceMeshAsset = std::move(asset);
    MarkConfigurationDirty();
    QueuePreparation();
}

void MeshInstanceLayer::SetSourceSet(MeshInstanceSourceSet sources,
    IGraphicsProvider* provider)
{
    m_providerSourceSet = std::move(sources);
    m_sourceSetProvider = m_providerSourceSet.Empty() ? nullptr : provider;
    m_hasRequestedSignature = false;
    MarkConfigurationDirty();
    QueuePreparation();
}

void MeshInstanceLayer::SetPlacements(std::vector<Placement> placements)
{
    std::unordered_set<uint64_t> used;
    used.reserve(placements.size());
    for (auto& placement : placements)
    {
        if (placement.id == 0 || !used.insert(placement.id).second)
        {
            do { placement.id = g_nextMeshInstancePlacementId.fetch_add(1,
                std::memory_order_relaxed); }
            while (placement.id == 0 || !used.insert(placement.id).second);
        }
    }
    m_placements = std::move(placements);
    MarkConfigurationDirty();
    QueuePreparation();
}

void MeshInstanceLayer::SetProviderPlacements(std::vector<Placement> placements)
{
    std::unordered_set<uint64_t> ids;
    ids.reserve(placements.size());
    for (size_t index = 0; index < placements.size(); ++index)
    {
        auto& placement = placements[index];
        if (placement.id && ids.insert(placement.id).second) continue;
        uint64_t generated = HashValue(1469598103934665603ull,
            static_cast<uint64_t>(index + 1));
        while (!generated || !ids.insert(generated).second) ++generated;
        placement.id = generated;
    }
    if (placements.size() == m_providerPlacements.size())
    {
        bool same = true;
        for (size_t index = 0; index < placements.size() && same; ++index)
        {
            same = placements[index].id == m_providerPlacements[index].id &&
                std::memcmp(&placements[index].transform,
                    &m_providerPlacements[index].transform,
                    sizeof(glm::mat4)) == 0;
        }
        if (same) return;
    }
    m_providerPlacements = std::move(placements);
    MarkConfigurationDirty();
    QueuePreparation();
}

uint64_t MeshInstanceLayer::AddPlacement(const glm::mat4& transform)
{
    uint64_t id = g_nextMeshInstancePlacementId.fetch_add(1,
        std::memory_order_relaxed);
    if (id == 0) id = g_nextMeshInstancePlacementId.fetch_add(1,
        std::memory_order_relaxed);
    m_placements.push_back({id, transform});
    MarkConfigurationDirty();
    QueuePreparation();
    return id;
}

bool MeshInstanceLayer::SetPlacementTransform(uint64_t placementId,
    const glm::mat4& transform)
{
    const auto found = std::find_if(m_placements.begin(), m_placements.end(),
        [&](const Placement& placement) { return placement.id == placementId; });
    if (found == m_placements.end()) return false;
    found->transform = transform;
    MarkConfigurationDirty();
    QueuePreparation();
    return true;
}

bool MeshInstanceLayer::RemovePlacement(uint64_t placementId)
{
    const auto found = std::find_if(m_placements.begin(), m_placements.end(),
        [&](const Placement& placement) { return placement.id == placementId; });
    if (found == m_placements.end()) return false;
    m_placements.erase(found);
    MarkConfigurationDirty();
    QueuePreparation();
    return true;
}

void MeshInstanceLayer::ClearPlacements()
{
    if (m_placements.empty()) return;
    m_placements.clear();
    MarkConfigurationDirty();
    QueuePreparation();
}

void MeshInstanceLayer::QueuePreparation()
{
    if (Owner && Owner->GetScene())
        DeferGraphicsPreparation(Owner->GetScene()->GetGraphicsProvider());
}

void MeshInstanceLayer::RemoveGeneratedObjects()
{
    if (!Owner || !Owner->GetScene())
    {
        m_generatedObjects.clear();
        return;
    }
    auto* scene = Owner->GetScene();
    for (uint64_t id : m_generatedObjects)
        if (auto* object = scene->FindObjectById(id))
            scene->RemoveObject(object);
    m_generatedObjects.clear();
}

void MeshInstanceLayer::RemovePendingGeneratedObjects()
{
    if (!Owner || !Owner->GetScene())
    {
        m_pendingGeneratedObjects.clear();
        return;
    }
    auto* scene = Owner->GetScene();
    for (uint64_t id : m_pendingGeneratedObjects)
        if (auto* object = scene->FindObjectById(id))
            scene->RemoveObject(object);
    m_pendingGeneratedObjects.clear();
}

ObjectLodProfileData MeshInstanceLayer::ResolveLodProfile() const
{
    ObjectLodProfileData profile;
    std::string base = sourceMeshAsset;
    if (base.empty() && Owner)
        if (const auto* mesh = Owner->GetComponent<Mesh>())
            base = mesh->GetFilePath();
    if (!Owner || base.empty()) return profile;
    for (const auto* component : Owner->Components)
        if (const auto* provider = dynamic_cast<const ObjectLodProvider*>(component))
        {
            const auto profiles = provider->GetObjectLodProfiles();
            auto found = profiles.find(base);
            if (found == profiles.end())
                found = std::find_if(profiles.begin(), profiles.end(),
                    [&](const auto& entry) { return SameAsset(entry.first, base); });
            if (found != profiles.end())
            {
                profile = found->second;
                break;
            }
        }
    return profile;
}

std::vector<std::string> MeshInstanceLayer::ResolveLodPaths(
    const ObjectLodProfileData& profile) const
{
    std::vector<std::string> paths;
    std::string base = sourceMeshAsset;
    if (base.empty() && Owner)
        if (const auto* mesh = Owner->GetComponent<Mesh>())
            base = mesh->GetFilePath();
    if (base.empty()) return paths;
    std::vector<std::string> mainPart = profile.meshes;
    mainPart.erase(std::remove(mainPart.begin(), mainPart.end(), std::string{}),
        mainPart.end());
    if (mainPart.empty()) mainPart.push_back(base);
    else if (!SameAsset(mainPart.front(), base)) mainPart.insert(mainPart.begin(), base);
    if (mainPart.size() > 3)
        mainPart = {mainPart.front(), mainPart[1], mainPart.back()};
    paths = std::move(mainPart);
    for (const auto& part : profile.parts)
    {
        if (part.key.empty()) continue;
        for (const auto& path : part.meshes)
            if (!path.empty()) paths.push_back(path);
    }
    return paths;
}

uint64_t MeshInstanceLayer::ConfigurationSignature(
    const ObjectLodProfileData& profile,
    const std::vector<std::string>& paths,
    const std::vector<Placement>& placements,
    IGraphicsProvider* provider) const
{
    uint64_t hash = 1469598103934665603ull;
    const uintptr_t providerIdentity = reinterpret_cast<uintptr_t>(provider);
    hash = HashValue(hash, providerIdentity);
    hash = HashValue(hash, m_providerSourceSet.GetRevision());
    hash = HashString(hash, sourceMeshAsset);
    hash = HashValue(hash, groupSpanMeters);
    for (const auto& path : paths) hash = HashString(hash, path);
    hash = HashValue(hash, paths.size());
    for (const auto& placement : placements)
    {
        hash = HashValue(hash, placement.id);
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                hash = HashValue(hash, placement.transform[column][row]);
    }
    hash = HashValue(hash, placements.size());
    for (float distance : profile.distances) hash = HashValue(hash, distance);
    hash = HashValue(hash, profile.distances.size());
    for (float pixels : profile.screenThresholdsPixels) hash = HashValue(hash, pixels);
    hash = HashValue(hash, profile.screenThresholdsPixels.size());
    hash = HashValue(hash, profile.hysteresis);
    hash = HashValue(hash, profile.screenSizeSelection);
    hash = HashValue(hash, profile.distanceOverride);
    hash = HashValue(hash, profile.ditherTransition);
    hash = HashValue(hash, profile.transitionWidth);
    hash = HashValue(hash, profile.shadowDistance);
    hash = HashValue(hash, profile.cullAfterLastLod);
    hash = HashValue(hash, profile.finalLodCullDistance);
    for (const auto& part : profile.parts)
    {
        hash = HashString(hash, part.key);
        for (const auto& path : part.meshes) hash = HashString(hash, path);
        hash = HashValue(hash, part.meshes.size());
    }
    hash = HashValue(hash, profile.parts.size());
    if (Owner)
        if (const auto* material = Owner->GetComponent<Material>())
            hash = HashValue(hash, material->GetConfigurationRevision());
    return hash;
}

void MeshInstanceLayer::OnAfterDeserialize(IGraphicsProvider* provider)
{
    if (provider && !DeferGraphicsPreparation(provider))
        PrepareGraphicsResources(provider);
}

void MeshInstanceLayer::PrepareGraphicsResources(IGraphicsProvider* provider)
{
    if (!provider || !Owner || !Owner->GetScene()) return;
    if (!m_providerSourceSet.Empty() && m_sourceSetProvider != provider)
    {
        RemovePendingGeneratedObjects();
        RemoveGeneratedObjects();
        m_lastError = "The source provider must publish a source set prepared for the active graphics provider.";
        m_building = false;
        return;
    }
    const auto profile = ResolveLodProfile();
    const auto paths = m_providerSourceSet.Empty()
        ? ResolveLodPaths(profile) : std::vector<std::string>{};
    std::vector<Placement> placements = m_placements;
    std::unordered_set<uint64_t> placementIds;
    placementIds.reserve(placements.size());
    for (const auto& placement : placements)
        placementIds.insert(placement.id);
    for (size_t index = 0; index < m_providerPlacements.size(); ++index)
    {
        Placement placement = m_providerPlacements[index];
        if (!placement.id || !placementIds.insert(placement.id).second)
        {
            placement.id = HashValue(1469598103934665603ull,
                static_cast<uint64_t>(index + 1));
            while (!placement.id || !placementIds.insert(placement.id).second)
                ++placement.id;
        }
        placements.push_back(std::move(placement));
    }
    const uint64_t signature = ConfigurationSignature(profile, paths,
        placements, provider);
    const bool providerChanged = m_loadedProvider && m_loadedProvider != provider;
    const bool missingPublishedObjects = !m_generatedObjects.empty() && Owner->GetScene() &&
        std::any_of(m_generatedObjects.begin(), m_generatedObjects.end(),
            [&](uint64_t id) { return Owner->GetScene()->FindObjectById(id) == nullptr; });
    if (missingPublishedObjects) m_hasRequestedSignature = false;
    if (!m_hasRequestedSignature || signature != m_requestedSignature)
    {
        if (providerChanged)
        {
            RemoveGeneratedObjects();
            m_loadedPaths.clear();
            m_loadedSources.clear();
        }
        RemovePendingGeneratedObjects();
        m_pendingGroups.clear();
        m_pendingTilePlacements.clear();
        m_nextPlacementToGroup = 0;
        m_nextGroup = 0;
        m_pendingGroupsInitialized = false;
        m_requestedSignature = signature;
        m_hasRequestedSignature = true;
        m_pendingPaths = paths;
        m_pendingPlacements = std::move(placements);
        m_pendingSources.clear();
        m_pendingSourceSet.Clear();
        m_nextSource = 0;
        m_lastError.clear();
        if (!m_providerSourceSet.Empty())
            m_pendingSourceSet = m_providerSourceSet;
        else if (paths == m_loadedPaths && m_loadedSources.size() == paths.size())
        {
            m_pendingSources = m_loadedSources;
            m_nextSource = paths.size();
        }
        m_building = true;
    }
    if (!m_building) return;

    if (m_pendingPlacements.empty() ||
        (m_pendingSourceSet.Empty() && paths.empty()))
    {
        RemovePendingGeneratedObjects();
        m_pendingGroups.clear();
        m_pendingTilePlacements.clear();
        m_nextPlacementToGroup = 0;
        m_nextGroup = 0;
        m_pendingGroupsInitialized = false;
        RemoveGeneratedObjects();
        m_loadedPaths.clear();
        m_loadedSources.clear();
        m_pendingPlacements.clear();
        m_pendingSourceSet.Clear();
        m_loadedProvider = provider;
        m_building = false;
        return;
    }

    if (m_providerSourceSet.Empty() && m_nextSource < m_pendingPaths.size())
    {
        try
        {
            auto source = std::make_shared<Mesh>();
            source->LoadFromFile(m_pendingPaths[m_nextSource]);
            source->PrepareGraphicsResources(provider);
            if (!source->IsReady())
                throw std::runtime_error("Mesh instance source is not graphics-ready");
            m_pendingSources.push_back(std::move(source));
            ++m_nextSource;
        }
        catch (const std::exception& error)
        {
            m_lastError = error.what();
            m_building = false;
            return;
        }
        catch (...)
        {
            m_lastError = "Unknown error while preparing mesh instance source";
            m_building = false;
            return;
        }
        if (m_nextSource < m_pendingPaths.size())
        {
            DeferGraphicsPreparation(provider);
            return;
        }
    }

    try
    {
        if (!m_providerSourceSet.Empty())
        {
            size_t renderableParts = 0;
            for (const auto& [key, part] : m_pendingSourceSet.GetParts())
            {
                if (part.representations.empty())
                    throw std::runtime_error("Provider source part has no mesh representations: " + key);
                if (part.representations.size() > 3)
                    throw std::runtime_error(
                        "Provider source parts support at most three resident LOD representations: " + key);
                for (const auto& representation : part.representations)
                    if (!representation || !representation->IsReady())
                        throw std::runtime_error(
                            "Provider source set contains a mesh that is not graphics-ready");
                ++renderableParts;
            }
            if (!renderableParts)
                throw std::runtime_error("Provider source set has no renderable parts");
        }
        else if (m_pendingSourceSet.Empty())
        {
            const auto addPart = [&](const std::string& key, size_t count,
                size_t& sourceIndex)
            {
                if (key.empty() || !count || sourceIndex + count > m_pendingSources.size())
                    throw std::runtime_error("LOD representation part has invalid key or mesh chain");
                if (m_pendingSourceSet.FindPart(key))
                    throw std::runtime_error("LOD representation part keys must be unique: " + key);
                MeshInstanceSourceSet::Part part;
                part.key = key;
                part.representations.insert(part.representations.end(),
                    m_pendingSources.begin() + sourceIndex,
                    m_pendingSources.begin() + sourceIndex + count);
                sourceIndex += count;
                if (const auto* material = Owner->GetComponent<Material>())
                {
                    part.materialState = material->Serialize();
                    part.hasMaterialState = true;
                }
                m_pendingSourceSet.SetPart(std::move(part));
            };
            size_t sourceIndex = 0;
            std::string base = sourceMeshAsset;
            if (base.empty())
                if (const auto* mesh = Owner->GetComponent<Mesh>())
                    base = mesh->GetFilePath();
            size_t mainCount = std::count_if(profile.meshes.begin(),
                profile.meshes.end(), [](const std::string& path) { return !path.empty(); });
            if (mainCount == 0) mainCount = 1;
            else if (!profile.meshes.empty() && !SameAsset(profile.meshes.front(), base))
                ++mainCount;
            mainCount = std::min<size_t>(mainCount, 3);
            addPart(base, mainCount, sourceIndex);
            for (const auto& authoredPart : profile.parts)
            {
                if (authoredPart.key.empty()) continue;
                const size_t count = std::count_if(authoredPart.meshes.begin(),
                    authoredPart.meshes.end(), [](const std::string& path) { return !path.empty(); });
                if (!count) continue;
                addPart(authoredPart.key, count, sourceIndex);
            }
            if (sourceIndex != m_pendingSources.size())
                throw std::runtime_error("LOD representation paths did not map to source parts");
        }
        if (!Publish(provider, profile))
        {
            DeferGraphicsPreparation(provider);
            return;
        }
        m_loadedPaths = m_pendingPaths;
        m_loadedSources = m_pendingSources;
        m_loadedProvider = provider;
        m_building = false;
    }
    catch (const std::exception& error)
    {
        RemovePendingGeneratedObjects();
        m_pendingGroups.clear();
        m_pendingTilePlacements.clear();
        m_nextPlacementToGroup = 0;
        m_nextGroup = 0;
        m_pendingGroupsInitialized = false;
        m_lastError = error.what();
        m_building = false;
    }
    catch (...)
    {
        RemovePendingGeneratedObjects();
        m_pendingGroups.clear();
        m_pendingTilePlacements.clear();
        m_nextPlacementToGroup = 0;
        m_nextGroup = 0;
        m_pendingGroupsInitialized = false;
        m_lastError = "Unknown error while publishing mesh instance groups";
        m_building = false;
    }
}

bool MeshInstanceLayer::Publish(IGraphicsProvider* provider,
    const ObjectLodProfileData& profile)
{
    if (m_pendingSourceSet.Empty() || m_pendingPlacements.empty()) return false;
    auto* scene = Owner->GetScene();
    if (!m_pendingGroupsInitialized)
    {
        const float span = std::clamp(std::isfinite(groupSpanMeters)
            ? groupSpanMeters : 64.f, 1.f, 4096.f);
        constexpr size_t placementGroupingBudget = 4096;
        const size_t groupEnd = std::min(m_pendingPlacements.size(),
            m_nextPlacementToGroup + placementGroupingBudget);
        for (; m_nextPlacementToGroup < groupEnd; ++m_nextPlacementToGroup)
        {
            const size_t index = m_nextPlacementToGroup;
            const auto& placement = m_pendingPlacements[index];
            const double x = std::floor(double(placement.transform[3].x) / span);
            const double z = std::floor(double(placement.transform[3].z) / span);
            if (x < double(std::numeric_limits<int64_t>::min()) ||
                x > double(std::numeric_limits<int64_t>::max()) ||
                z < double(std::numeric_limits<int64_t>::min()) ||
                z > double(std::numeric_limits<int64_t>::max()))
                throw std::runtime_error("Mesh instance placement is outside supported group coordinates");
            m_pendingTilePlacements[
                {static_cast<int64_t>(x), static_cast<int64_t>(z)}].push_back(index);
        }
        if (m_nextPlacementToGroup < m_pendingPlacements.size()) return false;

        constexpr size_t maximumPlacementsPerGroup = 1024;
        m_pendingGroups.reserve(m_pendingTilePlacements.size());
        for (auto& [tile, indices] : m_pendingTilePlacements)
            for (size_t first = 0, chunk = 0; first < indices.size();
                first += maximumPlacementsPerGroup)
            {
                const size_t last = std::min(indices.size(),
                    first + maximumPlacementsPerGroup);
                for (const auto& [partKey, part] : m_pendingSourceSet.GetParts())
                {
                    if (part.representations.empty()) continue;
                    PendingGroup pending;
                    pending.tile = tile;
                    pending.chunk = static_cast<uint32_t>(chunk);
                    pending.sourcePartKey = partKey;
                    pending.placementIndices.assign(
                        indices.begin() + first, indices.begin() + last);
                    m_pendingGroups.push_back(std::move(pending));
                }
                ++chunk;
            }
        m_pendingGroupsInitialized = true;
        return false;
    }

    if (m_nextGroup >= m_pendingGroups.size())
    {
        RemoveGeneratedObjects();
        m_generatedObjects = std::move(m_pendingGeneratedObjects);
        m_pendingGroups.clear();
        m_pendingTilePlacements.clear();
        m_nextPlacementToGroup = 0;
        m_nextGroup = 0;
        m_pendingGroupsInitialized = false;
        return true;
    }

    const PendingGroup& group = m_pendingGroups[m_nextGroup];
    const auto* sourcePart = m_pendingSourceSet.FindPart(group.sourcePartKey);
    if (!sourcePart || sourcePart->representations.empty())
        throw std::runtime_error("Mesh instance source part disappeared during publication");
    const auto& representations = sourcePart->representations;
    std::vector<glm::mat4> transforms;
    std::vector<uint64_t> ids;
    transforms.reserve(group.placementIndices.size());
    ids.reserve(group.placementIndices.size());
    for (const size_t index : group.placementIndices)
    {
        const auto& placement = m_pendingPlacements.at(index);
        transforms.push_back(placement.transform * sourcePart->sourceTransform);
        ids.push_back(placement.id);
    }
    MeshInstanceLayerData layerData;
    layerData.SetPlacements(std::move(transforms), std::move(ids));
    layerData.GetInstanceData()->stableId = HashValue(
        HashValue(HashValue(HashValue(HashString(1469598103934665603ull,
            group.sourcePartKey), scene->GetObjectId(Owner)), group.tile.first),
            group.tile.second), group.chunk);

    std::vector<MeshInstanceLayerData::Batch> batches;
    batches.reserve(representations.size());
    for (const auto& source : representations)
        batches.push_back(layerData.BuildBatch(*source));
    const auto& instances = layerData.GetInstanceData();
    instances->maximumLod = static_cast<int>(representations.size()) - 1;

    auto* object = scene->AddObject("Mesh instance group (" +
        std::to_string(group.tile.first) + "," + std::to_string(group.tile.second) + "," +
        std::to_string(group.chunk) + ") " + group.sourcePartKey);
    if (!object) throw std::runtime_error("Could not create mesh instance group object");
    object->runtimeOnly = true;
    if (!scene->SetParentLocal(object, Owner))
    {
        scene->RemoveObject(object);
        throw std::runtime_error("Could not parent mesh instance group object");
    }
    const uint64_t generatedId = scene->GetObjectId(object);
    try
    {
        auto* mesh = object->AddComponent<Mesh>();
        const auto& baseSource = representations.front();
        const uint32_t elementCount = baseSource->GetIndexBuffer()
            ? baseSource->GetIndexCount() : baseSource->GetVertexCount();
        const std::vector<Mesh::InstanceRange> ranges{{0, elementCount, 0,
            static_cast<uint32_t>(group.placementIndices.size())}};
        mesh->SetInstancedGeometry(baseSource, instances, ranges,
            batches.front().minimum, batches.front().maximum);
        ApplyProfile(*mesh, profile, representations.size());
        for (size_t lod = 1; lod < representations.size(); ++lod)
        {
            auto variant = std::make_unique<Mesh>();
            const auto& source = representations[lod];
            const uint32_t variantElementCount = source->GetIndexBuffer()
                ? source->GetIndexCount() : source->GetVertexCount();
            variant->SetInstancedGeometry(source, instances,
                {{0, variantElementCount, 0,
                    static_cast<uint32_t>(group.placementIndices.size())}},
                batches[lod].minimum, batches[lod].maximum);
            if (!mesh->StoreRuntimeLod(static_cast<int>(lod), std::move(variant)))
                throw std::runtime_error("Could not install mesh instance LOD");
        }
        if (sourcePart->hasMaterialState)
        {
            auto* material = object->AddComponent<Material>();
            material->Deserialize(sourcePart->materialState);
            material->OnAfterDeserialize(provider);
        }
    }
    catch (...)
    {
        if (auto* failed = scene->FindObjectById(generatedId))
            scene->RemoveObject(failed);
        throw;
    }
    m_pendingGeneratedObjects.push_back(generatedId);
    ++m_nextGroup;
    return false;
}

void MeshInstanceLayer::OnDestroy()
{
    RemovePendingGeneratedObjects();
    RemoveGeneratedObjects();
}

Engine::Serialization::JsonValue MeshInstanceLayer::Serialize() const
{
    using JsonValue = Engine::Serialization::JsonValue;
    JsonValue output = Component::Serialize();
    JsonValue placements = JsonValue::MakeArray(m_placements.size());
    for (const auto& placement : m_placements)
    {
        JsonValue matrix = JsonValue::MakeArray(16);
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                matrix.Push(JsonValue(placement.transform[column][row]));
        placements.Push(JsonValue::MakeObject()
            .Set("id", JsonValue(std::to_string(placement.id)))
            .Set("transform", std::move(matrix)));
    }
    output.Set("placements", std::move(placements));
    return output;
}

void MeshInstanceLayer::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    std::vector<Placement> placements;
    const auto& entries = value["placements"];
    placements.reserve(entries.ArraySize());
    for (size_t index = 0; index < entries.ArraySize(); ++index)
    {
        const auto& entry = entries.ArrayAt(index);
        const auto& matrix = entry["transform"];
        if (matrix.ArraySize() != 16) continue;
        Placement placement;
        try
        {
            const std::string id = entry["id"].AsString();
            if (!id.empty()) placement.id = std::stoull(id);
        }
        catch (...) { placement.id = 0; }
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                placement.transform[column][row] = matrix.ArrayAt(
                    size_t(column * 4 + row)).AsFloat();
        placements.push_back(placement);
    }
    SetPlacements(std::move(placements));
}

bool MeshInstanceLayer::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = Component::DrawProperties(ui);
    for (auto& placement : m_placements)
    {
        const std::string suffix = "##" + std::to_string(placement.id);
        const std::string label = "Placement " + std::to_string(placement.id);
        ui.Label(label.c_str());
        changed |= ui.DragFloat(("X" + suffix).c_str(),
            &placement.transform[3].x, .1f, -100000.f, 100000.f);
        changed |= ui.DragFloat(("Y" + suffix).c_str(),
            &placement.transform[3].y, .1f, -100000.f, 100000.f);
        changed |= ui.DragFloat(("Z" + suffix).c_str(),
            &placement.transform[3].z, .1f, -100000.f, 100000.f);
    }
    if (ui.Button("Add mesh placement"))
    {
        AddPlacement(glm::mat4(1.f));
        changed = true;
    }
    if (!m_placements.empty() && ui.Button("Remove last mesh placement"))
    {
        m_placements.pop_back();
        changed = true;
    }
    if (changed)
    {
        MarkConfigurationDirty();
        QueuePreparation();
    }
    if (!m_lastError.empty()) ui.ValueLabel("Instance layer error", m_lastError.c_str());
    return changed;
}
}
