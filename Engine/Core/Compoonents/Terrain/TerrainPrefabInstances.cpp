#include "TerrainObjectLayer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/PrefabLoadProfile.h"
#include "Core/Serialization/PreparedPrefab.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Rendering/SharedMeshBuffers.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Mesh/MeshInstanceLayerData.h"
#include "Core/Compoonents/Mesh/MeshInstanceSourceSet.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <cmath>
#include <future>
#include <mutex>
#include <optional>
#include "Core/Graphics/IGraphicsProvider.h"
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace Engine::Components
{
namespace
{
// A prototype is shared only while a streamed layer uses it. It has no
// graphics provider and is never attached to the live scene.
struct SharedPrefabPrototype
{
    struct PreparedGeometry
    {
        std::shared_ptr<Mesh> mesh;
        Engine::Rendering::PreparedMeshBuffer buffer;
        std::shared_ptr<Engine::Graphics::IGraphicsBuffer> graphicsBuffer;
    };
    Engine::Scene::Scene scene;
    Engine::Core::Object* root = nullptr;
    using GeometryKey = std::pair<const Mesh*, Engine::Graphics::IGraphicsProvider*>;
    MeshInstanceSourceSet sources;
    std::map<GeometryKey, std::future<PreparedGeometry>> pendingGeometry;
};

std::string SharedSourcePartKey(const SharedPrefabPrototype::GeometryKey& key)
{
    return std::to_string(reinterpret_cast<uintptr_t>(key.first)) + ":" +
        std::to_string(reinterpret_cast<uintptr_t>(key.second));
}
}
float TerrainObjectLayer::prefabInstanceGroupSpanMeters=256.f;
void TerrainObjectLayer::SetPrefabInstanceGroupSpanForBenchmark(float span)
{
    prefabInstanceGroupSpanMeters=std::clamp(span,8.f,256.f);
}
void TerrainObjectLayer::RebuildPrefabInstancesForBenchmark()
{
    m_prefabInstances.reset();
    m_prefabCheckedRevision=UINT64_MAX;
}
struct TerrainObjectLayer::PrefabInstances
{
    struct Source
    {
        std::string resolved;
        std::filesystem::file_time_type stamp;
        bool operator==(const Source& other) const
        { return resolved==other.resolved && stamp==other.stamp; }
    };
    IGraphicsProvider* provider=nullptr;
    uint64_t signature=UINT64_MAX;
    std::chrono::steady_clock::time_point nextCheck{};
    std::chrono::steady_clock::time_point nextPlacementSignatureCheck{};
    std::map<std::string,Source> sources;
    bool sourceProbeActive=false;
    std::string sourceProbeCursor;
    std::map<std::string,Source> sourceProbeResults;
    std::map<std::string,ObjectLods> lods;
    float viewDistance=0, lod1Distance=0, lod2Distance=0, lodHysteresis=0;
    bool screenLodSelection=false, distanceLodOverride=false;
    float screenLod1Pixels=0, screenLod2Pixels=0, screenLodHysteresis=0;
    float shadowDistance=0, shadowFadeWidth=0;
    int shadowLod=0;
    bool HasSameConfiguration(const TerrainObjectLayer& layer) const
    {
        return viewDistance==layer.viewDistance && lod1Distance==layer.lod1Distance &&
            lod2Distance==layer.lod2Distance && lodHysteresis==layer.lodHysteresis &&
            screenLodSelection==layer.screenLodSelection &&
            distanceLodOverride==layer.distanceLodOverride &&
            screenLod1Pixels==layer.screenLod1Pixels &&
            screenLod2Pixels==layer.screenLod2Pixels &&
            screenLodHysteresis==layer.screenLodHysteresis &&
            shadowDistance==layer.shadowDistance &&
            shadowFadeWidth==layer.GetShadowFadeWidth() && shadowLod==layer.shadowLod;
    }
    bool HasGeneratedObjects(const Engine::Scene::Scene& scene) const
    {
        return std::all_of(generated.begin(),generated.end(),[&](uint64_t id) {
            return scene.FindObjectById(id)!=nullptr;
        });
    }
    void StoreConfiguration(const TerrainObjectLayer& layer)
    {
        viewDistance=layer.viewDistance; lod1Distance=layer.lod1Distance;
        lod2Distance=layer.lod2Distance; lodHysteresis=layer.lodHysteresis;
        screenLodSelection=layer.screenLodSelection;
        distanceLodOverride=layer.distanceLodOverride;
        screenLod1Pixels=layer.screenLod1Pixels;
        screenLod2Pixels=layer.screenLod2Pixels;
        screenLodHysteresis=layer.screenLodHysteresis;
        shadowDistance=layer.shadowDistance;
        shadowFadeWidth=layer.GetShadowFadeWidth(); shadowLod=layer.shadowLod;
    }
    bool HasSameLods(const std::map<std::string,ObjectLods>& current) const
    {
        if(lods.size()!=current.size()) return false;
        for(const auto& [asset,expected]:lods)
        {
            const auto found=current.find(asset);
            if(found==current.end() || found->second.meshes!=expected.meshes ||
                found->second.shadowMeshes!=expected.shadowMeshes ||
                found->second.distances!=expected.distances ||
                found->second.screenThresholdsPixels!=expected.screenThresholdsPixels ||
                found->second.hysteresis!=expected.hysteresis ||
                found->second.screenSizeSelection!=expected.screenSizeSelection ||
                found->second.distanceOverride!=expected.distanceOverride ||
                found->second.ditherTransition!=expected.ditherTransition ||
                found->second.transitionWidth!=expected.transitionWidth ||
                found->second.shadowDistance!=expected.shadowDistance ||
                found->second.cullAfterLastLod!=expected.cullAfterLastLod ||
                found->second.finalLodCullDistance!=expected.finalLodCullDistance) return false;
            if (found->second.parts.size() != expected.parts.size()) return false;
            for (size_t part = 0; part < expected.parts.size(); ++part)
                if (found->second.parts[part].key != expected.parts[part].key ||
                    found->second.parts[part].meshes != expected.parts[part].meshes)
                    return false;
        }
        return true;
    }
    std::vector<uint64_t> generated;
    std::vector<std::shared_ptr<SharedPrefabPrototype>> prototypes;
    std::shared_ptr<PrefabInstances> previous;
    bool building = false;
    std::string nextAsset;
    size_t nextPart = 0;
    size_t groupStart = 0;
    std::map<std::pair<int,int>, MeshInstanceLayerData> tileLayers;
    using PrototypeKey = std::pair<std::string, std::filesystem::file_time_type>;
    std::shared_ptr<std::atomic_bool> canceled = std::make_shared<std::atomic_bool>(false);
    std::map<PrototypeKey, std::future<std::unique_ptr<Engine::Serialization::PreparedPrefab>>> pending;
    std::map<PrototypeKey, std::unique_ptr<Engine::Serialization::PreparedPrefab>> prepared;
    std::map<PrototypeKey, std::future<std::shared_ptr<SharedPrefabPrototype>>> pendingPrototypes;
    ~PrefabInstances() { canceled->store(true); }
};

bool TerrainObjectLayer::IsPrefabPreparationPending() const
{
    return m_prefabSourcePreparationPending ||
        (m_prefabInstances && m_prefabInstances->building);
}

void TerrainObjectLayer::PreparePrefabInstances(IGraphicsProvider* provider, bool* uploadAvailable)
{
    using BuildProfile=Engine::Serialization::PrefabLoadProfile;
    m_lastPrefabPrepareTimings={};
    if (!Owner || !Owner->GetScene() || !provider) return;
    const auto now=std::chrono::steady_clock::now();
    const bool changed=m_prefabCheckedRevision!=GetConfigurationRevision() ||
        m_prefabCheckedData!=placements.data() || m_prefabCheckedCount!=placements.size();
    if (!changed && !m_prefabInstances && !m_prefabSourcePreparationPending) return;
    const bool sourceProbeActive=!changed && m_prefabInstances &&
        m_prefabInstances->provider==provider && m_prefabInstances->sourceProbeActive;
    if (!changed && m_prefabInstances->provider==provider && !m_prefabInstances->building &&
        now<m_prefabInstances->nextCheck && !sourceProbeActive) return;
    // Grass and other mesh-only layers keep their existing renderer without
    // paying for a placement signature when they have no prefab runtime.
    bool hasPrefabs=false;
    for (const auto& placement:placements)
        if (std::filesystem::path(placement.asset).extension()==".prefab") { hasPrefabs=true; break; }
    m_prefabCheckedRevision=GetConfigurationRevision(); m_prefabCheckedData=placements.data();
    m_prefabCheckedCount=placements.size(); m_hasPrefabPlacements=hasPrefabs;
    if (!hasPrefabs && !m_prefabInstances) return;
    std::map<std::string,ObjectLods> lodProfiles;
    for (const auto& [asset, meshes] : GetDefinedObjectLods())
    {
        ObjectLods profile = GetObjectLodProfile(asset);
        if (profile.meshes.empty()) profile.meshes = meshes;
        lodProfiles.emplace(asset,std::move(profile));
    }
    // Check the placement signature on its normal interval. Source file probes
    // are spread over subsequent frames to avoid one synchronous filesystem
    // burst; an in-place placement edit may therefore take a few extra frames
    // to detect while a source scan is in progress.
    const bool reuseBuildingSignature=!changed && m_prefabInstances &&
        m_prefabInstances->provider==provider && m_prefabInstances->building &&
        now<m_prefabInstances->nextPlacementSignatureCheck;
    uint64_t signature=m_prefabInstances && (sourceProbeActive || reuseBuildingSignature)
        ? m_prefabInstances->signature : UINT64_MAX;
    if (!sourceProbeActive && !reuseBuildingSignature)
    {
        const auto signatureStarted=std::chrono::steady_clock::now();
        signature=GetPlacementSignature();
        m_lastPrefabPrepareTimings.placementSignatureMs=
            std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-signatureStarted).count();
        if (m_prefabInstances && m_prefabInstances->building)
            m_prefabInstances->nextPlacementSignatureCheck=now+std::chrono::milliseconds(100);
    }
    const auto readSource=[](const std::string& asset) {
        BuildProfile::Stage stage("prefab_source_resolution");
        PrefabInstances::Source source;
        source.resolved=std::filesystem::absolute(Mesh::ResolveFilePath(asset)).lexically_normal().generic_string();
        std::error_code error;
        source.stamp=std::filesystem::last_write_time(source.resolved,error);
        return source;
    };
    std::map<std::string,PrefabInstances::Source> sources;
    // Resolve and stat the full set of prefab sources off the scene thread.
    // Reuse recent results across neighboring cells, while retaining the
    // existing per-source periodic probe for edits and asset relocation.
    {
        struct AsyncSource
        {
            std::shared_future<PrefabInstances::Source> future;
            std::optional<PrefabInstances::Source> value;
            std::chrono::steady_clock::time_point checked{};
        };
        static std::mutex sourceCacheMutex;
        static std::map<std::string,AsyncSource> sourceCache;
        std::map<std::string,bool> requiredSources;
        const auto addSource=[&](const std::string& path) {
            if (!path.empty() && std::filesystem::path(path).extension()==".prefab")
                requiredSources.try_emplace(path,true);
        };
        for (const auto& [base,lods]:lodProfiles)
        {
            addSource(base);
            for (const auto& path:lods.meshes) addSource(path);
            for (const auto& path:lods.shadowMeshes) addSource(path);
        }
        for (const auto& placement:placements) addSource(placement.asset);
        bool allReady=true;
        for (const auto& [path,unused]:requiredSources)
        {
            (void)unused;
            if (sources.count(path)) continue;
            std::lock_guard lock(sourceCacheMutex);
            auto& entry=sourceCache[path];
            const auto time=std::chrono::steady_clock::now();
            if (!entry.value && !entry.future.valid())
            {
                entry.future=Engine::Rendering::RenderJobs::Get().Submit([path,readSource] {
                    return readSource(path);
                }).share();
            }
            else if (entry.value && time-entry.checked>=std::chrono::seconds(1))
            {
                entry.value.reset();
                entry.future=Engine::Rendering::RenderJobs::Get().Submit([path,readSource] {
                    return readSource(path);
                }).share();
            }
            if (entry.future.valid() && entry.future.wait_for(std::chrono::seconds(0))==std::future_status::ready)
            {
                entry.value=entry.future.get();
                entry.checked=time;
            }
            if (entry.value) sources.emplace(path,*entry.value);
            else allReady=false;
        }
        m_prefabSourcePreparationPending=!allReady;
        if (!allReady)
        {
            // A future revision check must revisit the in-flight scan even
            // though this call already recorded the observed placements.
            m_prefabCheckedRevision=UINT64_MAX;
            return;
        }
    }
    auto* scene=Owner->GetScene();
    if (m_prefabInstances && !m_prefabInstances->building && m_prefabInstances->provider==provider &&
        m_prefabInstances->signature==signature &&
        m_prefabInstances->HasSameLods(lodProfiles) &&
        m_prefabInstances->HasSameConfiguration(*this) &&
        (sourceProbeActive || m_prefabInstances->HasGeneratedObjects(*scene)))
    {
        // Refresh distinct source spellings, not all placement groups. Include
        // resolved identity: overrides/relocations can preserve the timestamp.
        bool same=true;
        const auto sourceProbeStarted=std::chrono::steady_clock::now();
        if (!m_prefabInstances->sourceProbeActive)
        {
            m_prefabInstances->sourceProbeActive=true;
            m_prefabInstances->sourceProbeCursor.clear();
            m_prefabInstances->sourceProbeResults.clear();
            m_prefabInstances->nextCheck=now+std::chrono::seconds(1);
        }
        const auto next=m_prefabInstances->sourceProbeCursor.empty()
            ? m_prefabInstances->sources.begin()
            : m_prefabInstances->sources.upper_bound(m_prefabInstances->sourceProbeCursor);
        if (next!=m_prefabInstances->sources.end())
        {
            auto current=readSource(next->first);
            same=current==next->second;
            m_prefabInstances->sourceProbeResults.emplace(next->first,current);
            m_prefabInstances->sourceProbeCursor=next->first;
        }
        m_lastPrefabPrepareTimings.sourceProbeMs=
            std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-sourceProbeStarted).count();
        const bool finished=m_prefabInstances->sourceProbeResults.size()==
            m_prefabInstances->sources.size();
        if (!same || finished)
        {
            sources=std::move(m_prefabInstances->sourceProbeResults);
            m_prefabInstances->sourceProbeActive=false;
            m_prefabInstances->sourceProbeCursor.clear();
        }
        if (same && !finished) return;
        if(same)
        {
            // Scene restore/play-stop can replace the placements vector and
            // bump the component revision while restoring identical authored
            // data. Avoid tearing down/rebuilding hundreds of generated batch
            // objects when the actual content and render policy are unchanged.
            m_prefabInstances->nextCheck=now+std::chrono::seconds(1);
            return;
        }
        // A changed or newly resolved source falls through to a rebuild. The
        // remaining paths are resolved by the normal batch builder below.
    }
    BuildProfile buildProfile("TerrainObjectLayer: "+layerName);
    std::map<std::string,std::vector<const Placement*>> groups;
    // Thousands of placements reference a small set of prefab paths. Resolve
    // each distinct spelling once per refresh, retaining relocation checks on
    // subsequent refreshes so newly created project overrides are still found.
    const auto key=[&](const std::string& path) -> const std::string& {
        BuildProfile::Stage stage("prefab_source_lookup");
        auto [entry,inserted]=sources.try_emplace(path);
        if (inserted) entry->second=readSource(path);
        return entry->second.resolved;
    };
    std::map<std::string,std::string> bases;
    {
    BuildProfile::Stage stage("prefab_lod_source_index");
    for (const auto& [base,lods]:lodProfiles)
        if (std::filesystem::path(base).extension()==".prefab")
        {
            bases[key(base)]=base;
            for (const auto& variant:lods.meshes) bases[key(variant)]=base;
            for (const auto& variant:lods.shadowMeshes) bases[key(variant)]=base;
        }
    }
    // Resolve and classify each asset spelling once, not once per placement.
    // Dense streamed cells often contain thousands of identical paths, and
    // constructing filesystem paths repeatedly is expensive in debug builds.
    std::map<std::string,std::string> placementGroups;
    {
    BuildProfile::Stage stage("prefab_placement_grouping");
    for (const auto& placement:placements)
    {
        auto [group,inserted]=placementGroups.try_emplace(placement.asset);
        if (inserted && std::filesystem::path(placement.asset).extension()==".prefab")
        {
            const auto base=bases.find(key(placement.asset));
            group->second=base==bases.end() ? placement.asset:base->second;
        }
        if (!group->second.empty()) groups[group->second].push_back(&placement);
    }
    }
    const bool resume = m_prefabInstances && m_prefabInstances->building &&
        m_prefabInstances->provider == provider &&
        m_prefabInstances->signature == signature &&
        m_prefabInstances->sources == sources &&
        m_prefabInstances->HasSameLods(lodProfiles) &&
        m_prefabInstances->HasSameConfiguration(*this) &&
        m_prefabInstances->HasGeneratedObjects(*scene);
    std::shared_ptr<PrefabInstances> previous;
    if (!resume && m_prefabInstances)
    {
        if (m_prefabInstances->building)
        {
            for (auto id:m_prefabInstances->generated)
                if (auto* object=scene->FindObjectById(id)) scene->RemoveObject(object);
            previous = m_prefabInstances->previous;
        }
        else previous = m_prefabInstances;
    }
    auto runtime=resume ? m_prefabInstances : std::make_shared<PrefabInstances>();
    if (!resume)
    {
        runtime->provider=provider; runtime->signature=signature;
        runtime->nextPlacementSignatureCheck=now+std::chrono::milliseconds(100);
        runtime->previous=std::move(previous);
        runtime->StoreConfiguration(*this);
        runtime->lods=lodProfiles;
    }
    runtime->building = false;
    runtime->nextCheck=now+std::chrono::seconds(1);
    // Neighboring streamed cells frequently use the same prefab. Keep one
    // provider-free prototype while at least one such cell remains loaded.
    using PrototypeKey = PrefabInstances::PrototypeKey;
    static std::map<PrototypeKey, std::weak_ptr<SharedPrefabPrototype>> prototypeCache;
    const auto create=[&](const std::string& name) {
        auto* object=scene->AddObject(name); object->runtimeOnly=true;
        object->enabled=false;
        // New batches use layer-local coordinates. Preserving the old world
        // transform here would evaluate the growing scene graph for each batch
        // only to discard the result immediately below.
        if(!scene->SetParentLocal(object,Owner))
            throw std::runtime_error("Unable to parent generated prefab batch");
        object->transform.position={0,0,0}; object->transform.rotation={0,0,0};
        object->transform.scale={1,1,1};
        runtime->generated.push_back(scene->GetObjectId(object)); return object;
    };
    std::map<std::string,std::shared_ptr<SharedPrefabPrototype>> definitions;
    bool constructedPrototype = false;
    bool prototypeWorkStarted = false;
    const auto definition=[&](const std::string& asset) {
        auto& prototype=definitions[asset];
        if (!prototype)
        {
            BuildProfile::Stage stage("prototype_definition");
            const auto& resolved = key(asset);
            const PrototypeKey cacheKey{resolved, sources.at(asset).stamp};
            if (const auto found = prototypeCache.find(cacheKey); found != prototypeCache.end())
                prototype = found->second.lock();
            if (!prototype)
            {
                auto pendingPrototype=runtime->pendingPrototypes.find(cacheKey);
                if (pendingPrototype!=runtime->pendingPrototypes.end())
                {
                    if (pendingPrototype->second.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
                        return static_cast<Engine::Core::Object*>(nullptr);
                    try { prototype=pendingPrototype->second.get(); }
                    catch (...) { prototype.reset(); }
                    runtime->pendingPrototypes.erase(pendingPrototype);
                }
                else
                {
                    std::unique_ptr<Engine::Serialization::PreparedPrefab> prepared;
                    if (auto found=runtime->prepared.find(cacheKey);found!=runtime->prepared.end())
                    {
                        prepared=std::move(found->second);
                        runtime->prepared.erase(found);
                    }
                    auto future=Engine::Rendering::RenderJobs::Get().Submit(
                        [resolved,prepared=std::move(prepared)]() mutable {
                            auto result=std::make_shared<SharedPrefabPrototype>();
                            if (prepared)
                            {
                                Engine::Serialization::PreparedPrefab::Publication publication(*prepared);
                                result->root=Engine::Serialization::SceneSerializer::InstantiatePrefabFromValue(
                                    result->scene,resolved,prepared->document,nullptr,false);
                            }
                            else
                                result->root=Engine::Serialization::SceneSerializer::InstantiatePrefab(
                                    result->scene,resolved,nullptr);
                            return result;
                        });
                    runtime->pendingPrototypes.emplace(cacheKey,std::move(future));
                    prototypeWorkStarted=true;
                    return static_cast<Engine::Core::Object*>(nullptr);
                }
                if (!prototype) return static_cast<Engine::Core::Object*>(nullptr);
                if (!prototype->root) { prototype.reset(); return static_cast<Engine::Core::Object*>(nullptr); }
                constructedPrototype = true;
                prototypeCache[cacheKey] = prototype;
            }
            if (std::find(runtime->prototypes.begin(), runtime->prototypes.end(), prototype) == runtime->prototypes.end())
                runtime->prototypes.push_back(prototype);
        }
        return prototype->root;
    };
    bool processed = false;
    for (const auto& [asset,records]:groups)
    {
        if (resume && asset < runtime->nextAsset) continue;
        if (processed)
        {
            runtime->building = true;
            runtime->nextAsset = asset;
            break;
        }
        processed = true;
        std::vector<std::string> required{asset};
        if (const auto found = lodProfiles.find(asset); found != lodProfiles.end())
        {
            const auto addResidentLods = [&](std::vector<std::string> paths) {
                paths.erase(std::remove(paths.begin(), paths.end(), std::string{}), paths.end());
                if (paths.size() > 3) paths = {paths.front(), paths[1], paths.back()};
                required.insert(required.end(), paths.begin(), paths.end());
            };
            addResidentLods(found->second.meshes);
            addResidentLods(found->second.shadowMeshes);
        }
        bool waiting = false;
        for (const auto& path : required)
        {
            if (path.empty()) continue;
            const auto& resolved = key(path);
            const PrototypeKey cacheKey{resolved, sources.at(path).stamp};
            if (const auto cached = prototypeCache.find(cacheKey);
                cached != prototypeCache.end() && !cached->second.expired()) continue;
            if (auto pendingPrototype=runtime->pendingPrototypes.find(cacheKey);
                pendingPrototype!=runtime->pendingPrototypes.end())
            {
                if (pendingPrototype->second.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
                    waiting=true;
                continue;
            }
            if (runtime->prepared.count(cacheKey)) continue;
            auto pending = runtime->pending.find(cacheKey);
            if (pending == runtime->pending.end())
            {
                auto canceled = runtime->canceled;
                auto future = Engine::Rendering::RenderJobs::Get().Submit([resolved, canceled] {
                    return Engine::Serialization::PreparedPrefab::Load(resolved, canceled);
                });
                runtime->pending.emplace(cacheKey, std::move(future));
                waiting = true;
            }
            else if (pending->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                waiting = true;
            else
            {
                try { runtime->prepared[cacheKey] = pending->second.get(); }
                catch (...) { runtime->prepared[cacheKey] = nullptr; }
                runtime->pending.erase(pending);
            }
        }
        if (waiting)
        {
            runtime->building = true;
            runtime->nextAsset = asset;
            break;
        }
        // Publishing a prefab prototype and creating a graphics buffer can each
        // take several milliseconds. Keep them in separate preparation steps,
        // even when worker-produced geometry is already ready to upload.
        bool publishedPrototype = false;
        for (const auto& path : required)
        {
            if (path.empty()) continue;
            if (definition(path) && constructedPrototype)
            {
                publishedPrototype = true;
                break;
            }
            if (prototypeWorkStarted) break;
        }
        if (prototypeWorkStarted) publishedPrototype=true;
        if (publishedPrototype)
        {
            runtime->building = true;
            runtime->nextAsset = asset;
            break;
        }
        bool geometryWaiting = false;
        bool uploadedGeometry = false;
        for (const auto& path : required)
        {
            if (path.empty()) continue;
            auto* prototypeRoot = definition(path);
            if (!prototypeRoot) continue;
            const auto& prototype = definitions.at(path);
            for (auto* original : prototypeRoot->GetComponentsInChildren<Mesh>())
            {
                if (!original->Owner->IsEnabledInHierarchy()) continue;
                const SharedPrefabPrototype::GeometryKey geometryKey{original, provider};
                const std::string sourceKey = SharedSourcePartKey(geometryKey);
                if (const auto* part = prototype->sources.FindPart(sourceKey);
                    part && !part->representations.empty()) continue;
                auto pendingGeometry = prototype->pendingGeometry.find(geometryKey);
                if (pendingGeometry == prototype->pendingGeometry.end())
                {
                    auto vertices = original->GetVertices();
                    auto indices = original->GetIndices();
                    const auto part = original->Owner->transform.GetWorldMatrix();
                    auto* bufferFactory = provider->GetBufferFactory();
                    const bool createBufferOnWorker = uploadAvailable &&
                        bufferFactory->SupportsWorkerThreadStaticVertexBufferCreation();
                    auto future = Engine::Rendering::RenderJobs::Get().Submit(
                        [vertices = std::move(vertices), indices = std::move(indices), part,
                            bufferFactory, createBufferOnWorker]() mutable {
                            if (!indices.empty())
                            {
                                std::vector<Mesh::Vertex> expanded;
                                expanded.reserve(indices.size());
                                for (auto index : indices) expanded.push_back(vertices.at(index));
                                vertices = std::move(expanded);
                            }
                            if (vertices.empty()) return SharedPrefabPrototype::PreparedGeometry{};
                            const auto tangentMatrix = glm::mat3(part);
                            const auto normalMatrix = glm::transpose(glm::inverse(tangentMatrix));
                            for (auto& vertex : vertices)
                            {
                                const auto position = glm::vec3(part * glm::vec4(vertex.pos[0], vertex.pos[1], vertex.pos[2], 1));
                                auto normal = normalMatrix * glm::vec3(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
                                normal = glm::length(normal) > 1.e-6f ? glm::normalize(normal) : glm::vec3(0, 1, 0);
                                auto tangent = tangentMatrix * glm::vec3(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]);
                                tangent = glm::length(tangent) > 1.e-6f ? glm::normalize(tangent) : glm::vec3(1, 0, 0);
                                for (int axis = 0; axis < 3; ++axis)
                                { vertex.pos[axis] = position[axis]; vertex.normal[axis] = normal[axis]; vertex.tangent[axis] = tangent[axis]; }
                            }
                            auto source = std::make_shared<Mesh>();
                            if (!source->SetAuthoredVertices(std::move(vertices)))
                                throw std::runtime_error("Invalid shared prefab geometry");
                            auto buffer = Engine::Rendering::PrepareMeshBuffer(source->GetVertices().data(),
                                source->GetVertices().size() * sizeof(Mesh::Vertex));
                            std::shared_ptr<Engine::Graphics::IGraphicsBuffer> graphicsBuffer;
                            if (createBufferOnWorker)
                                graphicsBuffer = Engine::Rendering::AcquireMeshBuffer(bufferFactory,
                                    Engine::Graphics::IGraphicsBuffer::Usage::VertexBuffer,
                                    source->GetVertices().data(),
                                    source->GetVertices().size() * sizeof(Mesh::Vertex),
                                    sizeof(Mesh::Vertex), &buffer);
                            return SharedPrefabPrototype::PreparedGeometry{
                                std::move(source), std::move(buffer), std::move(graphicsBuffer)};
                        });
                    prototype->pendingGeometry.emplace(geometryKey, std::move(future));
                    if (createBufferOnWorker) *uploadAvailable = false;
                    geometryWaiting = true;
                }
                else if (uploadedGeometry || (uploadAvailable && !*uploadAvailable) ||
                    pendingGeometry->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                    geometryWaiting = true;
                else
                {
                    auto prepared = pendingGeometry->second.get();
                    prototype->pendingGeometry.erase(pendingGeometry);
                    if (prepared.mesh)
                    {
                        if (prepared.graphicsBuffer)
                            prepared.mesh->InstallPreparedStaticVertexBuffer(
                                std::move(prepared.graphicsBuffer), provider->GetBufferFactory());
                        else
                        {
                            if (uploadAvailable) *uploadAvailable = false;
                            prepared.mesh->CreateBuffer(provider->GetBufferFactory(), &prepared.buffer);
                        }
                    }
                    prototype->sources.SetRepresentations(sourceKey,
                        {std::move(prepared.mesh)});
                    uploadedGeometry = true;
                }
            }
        }
        if (geometryWaiting || uploadedGeometry)
        {
            runtime->building = true;
            runtime->nextAsset = asset;
            break;
        }
        auto* root=definition(asset);
        if (!root) continue;
        const auto placementMatrix=[this](const Placement& placement) {
            return PlacementTransform(placement);
        };
        // CPU objects provide coarse spatial bounds; the compute path still
        // selects visibility and LOD for each placement. Small 64 m groups
        // multiplied every prefab part/band into thousands of CPU components.
        const float prefabGroupSpan=std::clamp(prefabInstanceGroupSpanMeters,8.f,256.f);
        std::map<std::pair<int,int>,std::vector<const Placement*>> tiles;
        for (const auto* placement:records)
            tiles[{int(std::floor(placement->position.x/prefabGroupSpan)),
                int(std::floor(placement->position.z/prefabGroupSpan))}].push_back(placement);
        std::map<std::pair<int,int>,MeshInstanceLayerData> tileLayers;
        for (const auto& [tile,tileRecords]:tiles)
        {
            std::vector<glm::mat4> transforms;
            std::vector<uint64_t> ids;
            transforms.reserve(tileRecords.size());
            ids.reserve(tileRecords.size());
            for (const auto* placement:tileRecords)
            {
                transforms.push_back(placementMatrix(*placement));
                ids.push_back(placement->id);
            }
            tileLayers[tile].SetPlacements(std::move(transforms),std::move(ids));
        }
        auto& tileInstances = runtime->tileLayers;
        if (runtime->nextPart == 0)
        {
            tileInstances=std::move(tileLayers);
            runtime->groupStart = runtime->generated.size();
        }
        const auto profileEntry=lodProfiles.find(asset);
        const auto lodProfile=profileEntry==lodProfiles.end()
            ? GetObjectLodProfile(asset) : profileEntry->second;
        std::vector<std::string> chain=lodProfile.meshes.empty()
            ? std::vector<std::string>{asset} : lodProfile.meshes;
        std::vector<std::string> shadowChain=lodProfile.shadowMeshes;
        chain.erase(std::remove(chain.begin(),chain.end(),std::string{}),chain.end());
        if(chain.empty()) chain.push_back(asset);
        shadowChain.erase(std::remove(shadowChain.begin(),shadowChain.end(),std::string{}),shadowChain.end());
        const bool dedicatedShadowProxy=!shadowChain.empty();
        if(!dedicatedShadowProxy) shadowChain=chain;
        // The renderer's three resident bands support material changes per level.
        if (chain.size()>3) chain={chain.front(),chain[1],chain.back()};
        if (shadowChain.size()>3) shadowChain={shadowChain.front(),shadowChain[1],shadowChain.back()};
        const auto distances=GetObjectLodDistances(asset,chain.size());
        const auto shadowDistances=GetObjectLodDistances(asset,shadowChain.size());
        size_t partIndex = 0;
        bool processedPart = false;
        bool moreParts = false;
        const auto buildChain=[&](const std::vector<std::string>& meshes,
            const std::vector<float>& lodDistances, bool shadowProxyOnly)
        {
            for (size_t lod=0;lod<meshes.size();++lod)
            {
                auto* lodRoot=definition(meshes[lod]);
                if (!lodRoot) continue;
                for (auto* original:lodRoot->GetComponentsInChildren<Mesh>())
                {
                    if (!original->Owner->IsEnabledInHierarchy()) continue;
                    if (partIndex++ < runtime->nextPart) continue;
                    if (processedPart) { moreParts = true; return; }
                    processedPart = true;
                    runtime->nextPart = partIndex;
                    // Expand indexed geometry once per shared prefab part, never per placement.
                    auto& prototype = definitions.at(meshes[lod]);
                    const auto geometryKey = SharedPrefabPrototype::GeometryKey{
                        original, provider};
                    const std::string sourceKey = SharedSourcePartKey(geometryKey);
                    auto* sourcePart = prototype->sources.FindPart(sourceKey);
                    if (!sourcePart || sourcePart->representations.empty()) continue;
                    auto source = sourcePart->representations.front();
                    if (!source) continue;
                    BuildProfile::Stage instanceStage("batch_instances");
                    for (const auto& [tile,tileRecords]:tiles)
                    {
                        auto& layerData=tileInstances[tile];
                        const glm::vec3 windPadding(1.5f*std::max(0.f,source->GetBoundsMax().y)*
                            std::clamp(original->foliageWindStrength,0.f,1.f));
                        MeshInstanceLayerData::Batch batch;
                        {
                            BuildProfile::Stage boundsStage("batch_instance_transforms_bounds");
                            batch=layerData.BuildBatch(*source,windPadding);
                        }
                        const auto& instances=layerData.GetInstanceData();
                        BuildProfile::Stage objectStage("batch_instance_objects");
                        auto* object=create("Shared prefab part: "+original->Owner->name+" ("+
                            std::to_string(tile.first)+","+std::to_string(tile.second)+")");
                        auto* mesh=object->AddComponent<Mesh>();
                        mesh->screenLodSelection=lodProfile.screenSizeSelection || screenLodSelection;
                        mesh->distanceLodOverride=lodProfile.distanceOverride || distanceLodOverride;
                        mesh->screenLod1Pixels=lodProfile.screenThresholdsPixels.size()>0 &&
                            std::isfinite(lodProfile.screenThresholdsPixels[0])
                            ? std::clamp(lodProfile.screenThresholdsPixels[0],0.f,100000.f)
                            : screenLod1Pixels;
                        const float requestedLod2Pixels=lodProfile.screenThresholdsPixels.size()>1 &&
                            std::isfinite(lodProfile.screenThresholdsPixels[1])
                            ? std::clamp(lodProfile.screenThresholdsPixels[1],0.f,100000.f)
                            : screenLod2Pixels;
                        mesh->screenLod2Pixels=std::min(mesh->screenLod1Pixels,
                            requestedLod2Pixels);
                        mesh->screenLodHysteresis=lodProfile.screenSizeSelection
                            ? std::clamp(std::isfinite(lodProfile.hysteresis)
                                ? lodProfile.hysteresis : screenLodHysteresis,0.f,.5f)
                            : screenLodHysteresis;
                        instances->maximumLod=std::max(instances->maximumLod,int(meshes.size())-1);
                        mesh->useBoundsForShadowCulling=true;
                        mesh->foliageWindStrength=original->foliageWindStrength;
                        const auto& sourcePath=original->GetFilePath();
                        mesh->treeTimingGeometry=sourcePath.find("/Woodland/Oaks/")!=std::string::npos ||
                            sourcePath.find("\\Woodland\\Oaks\\")!=std::string::npos;
                        mesh->runtimeShadowDistance=std::max(0.f,
                            lodProfile.shadowDistance > 0.f
                                ? std::min(shadowDistance,lodProfile.shadowDistance)
                                : shadowDistance);
                        mesh->cullAfterLastLod = lodProfile.cullAfterLastLod &&
                            std::isfinite(lodProfile.finalLodCullDistance) &&
                            lodProfile.finalLodCullDistance > 0.f;
                        mesh->finalLodCullDistance = mesh->cullAfterLastLod
                            ? lodProfile.finalLodCullDistance : 0.f;
                        mesh->runtimeShadowFadeWidth=GetShadowFadeWidth();
                        mesh->dedicatedShadowProxy=dedicatedShadowProxy;
                        mesh->shadowProxyOnly=shadowProxyOnly;
                        if (shadowProxyOnly || !dedicatedShadowProxy)
                            mesh->fixedShadowLodBand=shadowLod<0 ? -1:std::min(shadowLod,int(meshes.size())-1);
                        else mesh->fixedShadowLodBand=-1;
                        if (meshes.size()>1)
                        {
                            mesh->instanceLodBand=int(lod);
                            mesh->instanceLodDistances={lodDistances[0],lodDistances.size()>1 ? lodDistances[1]:1.e20f};
                        }
                        mesh->SetInstancedGeometry(source,instances,{{0,source->GetVertexCount(),0,
                            static_cast<uint32_t>(tileRecords.size())}},batch.minimum,batch.maximum);
                        if (const auto* material=original->Owner->GetComponent<Material>())
                        {
                            sourcePart = prototype->sources.FindPart(sourceKey);
                            if (!sourcePart->hasMaterialState)
                                prototype->sources.SetMaterialState(sourceKey,
                                    material->Serialize());
                            sourcePart = prototype->sources.FindPart(sourceKey);
                            BuildProfile::Stage materialStage("batch_instance_materials");
                            auto* copy=object->AddComponent<Material>();
                            {
                                BuildProfile::Stage stage("batch_material_copy_resolved");
                                copy->CopyResolvedStateFrom(*material,
                                    &sourcePart->materialState);
                            }
                            {
                                BuildProfile::Stage stage("batch_material_graphics");
                                copy->OnAfterDeserialize(provider);
                            }
                            if (shadowDistance<=0) copy->castsShadows=false;
                        }
                    }
                }
            }
        };
        buildChain(chain,distances,false);
        if (dedicatedShadowProxy && !moreParts) buildChain(shadowChain,shadowDistances,true);
        if (moreParts)
        {
            runtime->building = true;
            runtime->nextAsset = asset;
            break;
        }
        // Collider policy belongs to the shared prefab. None are synthesized
        // when it has no collider; adding one later applies to every placement.
        BuildProfile::Stage colliderStage("batch_colliders");
        auto* colliderRoot=root;
        // Groups have already been canonicalized to their authored base above.
        // Resolving every LOD path again here performs many filesystem probes
        // for every batch part, even when the prefab has no colliders.
        if (const auto found = lodProfiles.find(asset); found != lodProfiles.end())
            colliderRoot = definition(found->first);
        if (colliderRoot) for (auto* collider:colliderRoot->GetComponentsInChildren<PrimitiveObjectCollider>())
        {
            if (!collider->Owner->IsEnabledInHierarchy()) continue;
            for (const auto* placement:records)
            {
                auto* object=create("Prefab collider: "+colliderRoot->name);
                const auto matrix=placementMatrix(*placement)*collider->Owner->transform.GetWorldMatrix();
                object->transform.position=glm::vec3(matrix[3]);
                object->transform.rotation=glm::eulerAngles(glm::quat_cast(glm::mat3(matrix)/placement->scale));
                object->transform.scale=glm::vec3(placement->scale);
                auto* copy=object->AddComponent<PrimitiveObjectCollider>(); copy->Deserialize(collider->Serialize());
                auto* body=object->AddComponent<RigidBody>();
                if (auto* originalBody=collider->Owner->GetComponent<RigidBody>()) body->Deserialize(originalBody->Serialize());
                else body->bodyType="Static";
            }
        }
        if (!runtime->previous)
            for (size_t i = runtime->groupStart; i < runtime->generated.size(); ++i)
                if (auto* object = scene->FindObjectById(runtime->generated[i])) object->enabled = true;
        runtime->nextPart = 0;
        tileInstances.clear();
    }
    if (!runtime->building && runtime->previous)
    {
        for (auto id : runtime->previous->generated)
            if (auto* object = scene->FindObjectById(id)) scene->RemoveObject(object);
        runtime->previous.reset();
        for (auto id : runtime->generated)
            if (auto* object = scene->FindObjectById(id)) object->enabled = true;
    }
    runtime->sources=std::move(sources);
    runtime->lods=lodProfiles;
    m_prefabInstances=std::move(runtime);
}
}
