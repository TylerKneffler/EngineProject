#include "TerrainObjectLayer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/PrefabLoadProfile.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <cmath>
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
    Engine::Scene::Scene scene;
    Engine::Core::Object* root = nullptr;
    std::map<std::pair<const Mesh*, Engine::Graphics::IGraphicsProvider*>, std::weak_ptr<Mesh>> geometry;
};
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
                found->second.distances!=expected.distances) return false;
        }
        return true;
    }
    std::vector<uint64_t> generated;
    std::vector<std::shared_ptr<SharedPrefabPrototype>> prototypes;
};

void TerrainObjectLayer::PreparePrefabInstances(IGraphicsProvider* provider)
{
    m_lastPrefabPrepareTimings={};
    if (!Owner || !Owner->GetScene() || !provider) return;
    const auto now=std::chrono::steady_clock::now();
    const bool changed=m_prefabCheckedRevision!=GetConfigurationRevision() ||
        m_prefabCheckedData!=placements.data() || m_prefabCheckedCount!=placements.size();
    if (!changed && !m_prefabInstances) return;
    const bool sourceProbeActive=!changed && m_prefabInstances &&
        m_prefabInstances->provider==provider && m_prefabInstances->sourceProbeActive;
    if (!changed && m_prefabInstances->provider==provider &&
        now<m_prefabInstances->nextCheck && !sourceProbeActive) return;
    // Grass and other mesh-only layers keep their existing renderer without
    // paying for a placement signature when they have no prefab runtime.
    bool hasPrefabs=false;
    for (const auto& placement:placements)
        if (std::filesystem::path(placement.asset).extension()==".prefab") { hasPrefabs=true; break; }
    m_prefabCheckedRevision=GetConfigurationRevision(); m_prefabCheckedData=placements.data();
    m_prefabCheckedCount=placements.size(); m_hasPrefabPlacements=hasPrefabs;
    if (!hasPrefabs && !m_prefabInstances) return;
    // Check the placement signature on its normal interval. Source file probes
    // are spread over subsequent frames to avoid one synchronous filesystem
    // burst; an in-place placement edit may therefore take a few extra frames
    // to detect while a source scan is in progress.
    uint64_t signature=m_prefabInstances && sourceProbeActive
        ? m_prefabInstances->signature : UINT64_MAX;
    if (!sourceProbeActive)
    {
        const auto signatureStarted=std::chrono::steady_clock::now();
        signature=GetPlacementSignature();
        m_lastPrefabPrepareTimings.placementSignatureMs=
            std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-signatureStarted).count();
    }
    const auto readSource=[](const std::string& asset) {
        PrefabInstances::Source source;
        source.resolved=std::filesystem::absolute(Mesh::ResolveFilePath(asset)).lexically_normal().generic_string();
        std::error_code error;
        source.stamp=std::filesystem::last_write_time(source.resolved,error);
        return source;
    };
    std::map<std::string,PrefabInstances::Source> sources;
    auto* scene=Owner->GetScene();
    if (m_prefabInstances && m_prefabInstances->provider==provider &&
        m_prefabInstances->signature==signature &&
        m_prefabInstances->HasSameLods(objectLods) &&
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
    using BuildProfile=Engine::Serialization::PrefabLoadProfile;
    BuildProfile buildProfile("TerrainObjectLayer: "+layerName);
    std::map<std::string,std::vector<const Placement*>> groups;
    // Thousands of placements reference a small set of prefab paths. Resolve
    // each distinct spelling once per refresh, retaining relocation checks on
    // subsequent refreshes so newly created project overrides are still found.
    const auto key=[&](const std::string& path) -> const std::string& {
        auto [entry,inserted]=sources.try_emplace(path);
        if (inserted) entry->second=readSource(path);
        return entry->second.resolved;
    };
    std::map<std::string,std::string> bases;
    for (const auto& [base,lods]:objectLods)
        if (std::filesystem::path(base).extension()==".prefab")
        {
            bases[key(base)]=base;
            for (const auto& variant:lods.meshes) bases[key(variant)]=base;
            for (const auto& variant:lods.shadowMeshes) bases[key(variant)]=base;
        }
    for (const auto& placement:placements)
        if (std::filesystem::path(placement.asset).extension()==".prefab")
        {
            const auto base=bases.find(key(placement.asset));
            groups[base==bases.end() ? placement.asset:base->second].push_back(&placement);
        }
    if (m_prefabInstances)
        for (auto id:m_prefabInstances->generated)
            if (auto* object=scene->FindObjectById(id)) scene->RemoveObject(object);
    auto runtime=std::make_shared<PrefabInstances>();
    runtime->provider=provider; runtime->signature=signature;
    runtime->StoreConfiguration(*this);
    runtime->nextCheck=now+std::chrono::seconds(1);
    // Neighboring streamed cells frequently use the same prefab. Keep one
    // provider-free prototype while at least one such cell remains loaded.
    using PrototypeKey = std::pair<std::string, std::filesystem::file_time_type>;
    static std::map<PrototypeKey, std::weak_ptr<SharedPrefabPrototype>> prototypeCache;
    const auto create=[&](const std::string& name) {
        auto* object=scene->AddObject(name); object->runtimeOnly=true;
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
                prototype = std::make_shared<SharedPrefabPrototype>();
                prototype->root = Engine::Serialization::SceneSerializer::InstantiatePrefab(
                    prototype->scene, resolved, nullptr);
                if (!prototype->root) { prototype.reset(); return static_cast<Engine::Core::Object*>(nullptr); }
                prototypeCache[cacheKey] = prototype;
            }
            runtime->prototypes.push_back(prototype);
        }
        return prototype->root;
    };
    for (const auto& [asset,records]:groups)
    {
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
        std::map<std::pair<int,int>,std::shared_ptr<Mesh::InstanceData>> tileInstances;
        std::vector<std::string> chain{asset};
        std::vector<std::string> shadowChain;
        if (const auto lods=objectLods.find(asset); lods!=objectLods.end())
        {
            if (!lods->second.meshes.empty()) chain=lods->second.meshes;
            shadowChain=lods->second.shadowMeshes;
        }
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
                    // Expand indexed geometry once per shared prefab part, never per placement.
                    auto& prototype = definitions.at(meshes[lod]);
                    auto& sharedGeometry = prototype->geometry[{original, provider}];
                    auto source = sharedGeometry.lock();
                    if (!source)
                    {
                        source = std::make_shared<Mesh>();
                        BuildProfile::Stage stage("batch_source_geometry");
                        std::vector<Mesh::Vertex> vertices;
                        {
                            BuildProfile::Stage copyStage("batch_geometry_copy_expand");
                            vertices=original->GetVertices();
                            if (!original->GetIndices().empty())
                            {
                                std::vector<Mesh::Vertex> expanded;
                                expanded.reserve(original->GetIndices().size());
                                for (auto index:original->GetIndices())
                                    expanded.push_back(vertices.at(index));
                                vertices=std::move(expanded);
                            }
                        }
                        if (vertices.empty()) continue;
                        // Bake the prefab-part transform once so all parts and LODs can use
                        // the exact same resident placement array, including impostors.
                        {
                            BuildProfile::Stage transformStage("batch_geometry_transform");
                            const auto part=original->Owner->transform.GetWorldMatrix();
                            const auto tangentMatrix=glm::mat3(part);
                            const auto normalMatrix=glm::transpose(glm::inverse(tangentMatrix));
                            for (auto& vertex:vertices)
                            {
                                const auto position=glm::vec3(part*glm::vec4(vertex.pos[0],vertex.pos[1],vertex.pos[2],1));
                                auto normal=normalMatrix*glm::vec3(vertex.normal[0],vertex.normal[1],vertex.normal[2]);
                                normal=glm::length(normal)>1.e-6f ? glm::normalize(normal):glm::vec3(0,1,0);
                                auto tangent=tangentMatrix*glm::vec3(vertex.tangent[0],vertex.tangent[1],vertex.tangent[2]);
                                tangent=glm::length(tangent)>1.e-6f ? glm::normalize(tangent):glm::vec3(1,0,0);
                                for (int axis=0;axis<3;++axis)
                                {
                                    vertex.pos[axis]=position[axis];
                                    vertex.normal[axis]=normal[axis];
                                    vertex.tangent[axis]=tangent[axis];
                                }
                            }
                        }
                        {
                            BuildProfile::Stage validationStage("batch_geometry_validate_bounds");
                            if (!source->SetAuthoredVertices(std::move(vertices)))
                                throw std::runtime_error("Invalid shared prefab geometry: "+meshes[lod]);
                        }
                        {
                            BuildProfile::Stage uploadStage("batch_geometry_buffer_create");
                            source->CreateBuffer(provider->GetBufferFactory());
                        }
                        sharedGeometry = source;
                    }
                    BuildProfile::Stage instanceStage("batch_instances");
                    const auto sourceMinimum=source->GetBoundsMin();
                    const auto sourceMaximum=source->GetBoundsMax();
                    const auto sourceCenter=(sourceMinimum+sourceMaximum)*0.5f;
                    const auto sourceExtents=(sourceMaximum-sourceMinimum)*0.5f;
                    for (const auto& [tile,tileRecords]:tiles)
                    {
                        auto& instances=tileInstances[tile];
                        const bool initialize=!instances;
                        if (initialize) instances=std::make_shared<Mesh::InstanceData>();
                        glm::vec3 minimum(std::numeric_limits<float>::max()),maximum(-minimum);
                        {
                            BuildProfile::Stage boundsStage("batch_instance_transforms_bounds");
                            for (const auto* placement:tileRecords)
                            {
                                const glm::mat4 matrix=placementMatrix(*placement);
                                Mesh::InstanceTransform instance{};
                                for (int row=0;row<3;++row)
                                    instance.rows[row]={matrix[0][row],matrix[1][row],matrix[2][row],matrix[3][row]};
                                if (initialize)
                                {
                                    instances->transforms.push_back(instance);
                                    instances->placementIds.push_back(placement->id);
                                }
                                // Transform the source AABB with center/extents rather
                                // than transforming eight corners for every placement,
                                // part, and LOD. This is exact for affine transforms and
                                // avoids repeated bounds queries in large foliage layers.
                                const glm::vec3 center(matrix*glm::vec4(sourceCenter,1.f));
                                const glm::mat3 linear(matrix);
                                const auto absolute=[](const glm::vec3& value) {
                                    return glm::vec3(std::abs(value.x),std::abs(value.y),std::abs(value.z));
                                };
                                const glm::vec3 extents=absolute(linear[0])*sourceExtents.x+
                                    absolute(linear[1])*sourceExtents.y+
                                    absolute(linear[2])*sourceExtents.z;
                                minimum=glm::min(minimum,center-extents);
                                maximum=glm::max(maximum,center+extents);
                            }
                        }
                        BuildProfile::Stage objectStage("batch_instance_objects");
                        auto* object=create("Shared prefab part: "+original->Owner->name+" ("+
                            std::to_string(tile.first)+","+std::to_string(tile.second)+")");
                        auto* mesh=object->AddComponent<Mesh>();
                        mesh->screenLodSelection=screenLodSelection;
                        mesh->distanceLodOverride=distanceLodOverride;
                        mesh->screenLod1Pixels=screenLod1Pixels;
                        mesh->screenLod2Pixels=screenLod2Pixels;
                        mesh->screenLodHysteresis=screenLodHysteresis;
                        const glm::vec3 windPadding(1.5f*std::max(0.f,source->GetBoundsMax().y)*
                            std::clamp(original->foliageWindStrength,0.f,1.f));
                        const auto lodLow=source->GetBoundsMin()-windPadding;
                        const auto lodHigh=source->GetBoundsMax()+windPadding;
                        if(!instances->hasLodBounds)
                        { instances->lodMinimum=lodLow; instances->lodMaximum=lodHigh; instances->hasLodBounds=true; }
                        else
                        { instances->lodMinimum=glm::min(instances->lodMinimum,lodLow); instances->lodMaximum=glm::max(instances->lodMaximum,lodHigh); }
                        instances->maximumLod=std::max(instances->maximumLod,int(meshes.size())-1);
                        mesh->useBoundsForShadowCulling=true;
                        mesh->foliageWindStrength=original->foliageWindStrength;
                        const auto& sourcePath=original->GetFilePath();
                        mesh->treeTimingGeometry=sourcePath.find("/Woodland/Oaks/")!=std::string::npos ||
                            sourcePath.find("\\Woodland\\Oaks\\")!=std::string::npos;
                        mesh->runtimeShadowDistance=std::max(0.f,shadowDistance);
                        mesh->runtimeShadowFadeWidth=GetShadowFadeWidth();
                        mesh->prefabDedicatedShadowProxy=dedicatedShadowProxy;
                        mesh->prefabShadowProxyOnly=shadowProxyOnly;
                        if (shadowProxyOnly || !dedicatedShadowProxy)
                            mesh->prefabShadowLod=shadowLod<0 ? -1:std::min(shadowLod,int(meshes.size())-1);
                        else mesh->prefabShadowLod=-1;
                        if (meshes.size()>1)
                        {
                            mesh->prefabLodBand=int(lod);
                            mesh->prefabLodDistances={lodDistances[0],lodDistances.size()>1 ? lodDistances[1]:1.e20f};
                        }
                        mesh->SetInstancedGeometry(source,instances,{{0,source->GetVertexCount(),0,
                            static_cast<uint32_t>(tileRecords.size())}},minimum,maximum);
                        if (const auto* material=original->Owner->GetComponent<Material>())
                        {
                            BuildProfile::Stage materialStage("batch_instance_materials");
                            auto* copy=object->AddComponent<Material>();
                            {
                                BuildProfile::Stage stage("batch_material_copy_resolved");
                                copy->CopyResolvedStateFrom(*material);
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
        if (dedicatedShadowProxy) buildChain(shadowChain,shadowDistances,true);
        // Collider policy belongs to the shared prefab. None are synthesized
        // when it has no collider; adding one later applies to every placement.
        BuildProfile::Stage colliderStage("batch_colliders");
        auto* colliderRoot=root;
        const auto sameAsset=[](const std::string& a,const std::string& b) {
            return std::filesystem::absolute(Mesh::ResolveFilePath(a)).lexically_normal()==
                std::filesystem::absolute(Mesh::ResolveFilePath(b)).lexically_normal();
        };
        for (const auto& [base,lods]:objectLods)
            if (std::filesystem::path(base).extension()==".prefab" &&
                (sameAsset(base,asset) || std::any_of(lods.meshes.begin(),lods.meshes.end(),
                    [&](const auto& lod) { return sameAsset(lod,asset); }) ||
                    std::any_of(lods.shadowMeshes.begin(),lods.shadowMeshes.end(),
                        [&](const auto& lod) { return sameAsset(lod,asset); })))
            { colliderRoot=definition(base); break; }
        if (!colliderRoot) continue;
        for (auto* collider:colliderRoot->GetComponentsInChildren<PrimitiveObjectCollider>())
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
    }
    runtime->sources=std::move(sources);
    runtime->lods=objectLods;
    m_prefabInstances=std::move(runtime);
}
}
