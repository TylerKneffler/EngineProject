#include "TerrainObjectLayer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
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
struct TerrainObjectLayer::PrefabInstances
{
    IGraphicsProvider* provider=nullptr;
    uint64_t signature=UINT64_MAX;
    std::chrono::steady_clock::time_point nextCheck{};
    std::map<std::string,std::filesystem::file_time_type> stamps;
    std::vector<uint64_t> generated;
};

void TerrainObjectLayer::PreparePrefabInstances(IGraphicsProvider* provider)
{
    if (!Owner || !Owner->GetScene() || !provider) return;
    const auto now=std::chrono::steady_clock::now();
    const bool changed=m_prefabCheckedRevision!=GetConfigurationRevision() ||
        m_prefabCheckedData!=placements.data() || m_prefabCheckedCount!=placements.size();
    if (!changed && !m_prefabInstances) return;
    if (!changed && m_prefabInstances->provider==provider && now<m_prefabInstances->nextCheck) return;
    std::map<std::string,std::vector<const Placement*>> groups;
    // Grass and other mesh-only layers keep their existing renderer.
    bool hasPrefabs=false;
    for (const auto& placement:placements)
        if (std::filesystem::path(placement.asset).extension()==".prefab") { hasPrefabs=true; break; }
    m_prefabCheckedRevision=GetConfigurationRevision(); m_prefabCheckedData=placements.data();
    m_prefabCheckedCount=placements.size(); m_hasPrefabPlacements=hasPrefabs;
    if (!hasPrefabs && !m_prefabInstances) return;
    const auto signature=GetPlacementSignature();
    if (m_prefabInstances && m_prefabInstances->provider==provider &&
        m_prefabInstances->signature==signature && now<m_prefabInstances->nextCheck && !changed) return;
    // Thousands of placements reference a small set of prefab paths. Resolve
    // each distinct spelling once per refresh, retaining relocation checks on
    // subsequent refreshes so newly created project overrides are still found.
    std::map<std::string,std::string> resolvedKeys;
    const auto key=[&](const std::string& path) -> const std::string& {
        auto [entry,inserted]=resolvedKeys.try_emplace(path);
        if (inserted)
            entry->second=std::filesystem::absolute(Mesh::ResolveFilePath(path)).lexically_normal().generic_string();
        return entry->second;
    };
    std::map<std::string,std::string> bases;
    for (const auto& [base,lods]:objectLods)
        if (std::filesystem::path(base).extension()==".prefab")
        {
            bases[key(base)]=base;
            for (const auto& variant:lods.meshes) bases[key(variant)]=base;
        }
    for (const auto& placement:placements)
        if (std::filesystem::path(placement.asset).extension()==".prefab")
        {
            const auto base=bases.find(key(placement.asset));
            groups[base==bases.end() ? placement.asset:base->second].push_back(&placement);
        }
    std::map<std::string,std::filesystem::file_time_type> stamps;
    for (const auto& [asset,records]:groups)
    {
        std::error_code error;
        stamps[asset]=std::filesystem::last_write_time(Mesh::ResolveFilePath(asset),error);
    }
    for (const auto& [asset,lods]:objectLods)
        if (std::filesystem::path(asset).extension()==".prefab")
        {
            std::error_code error;
            stamps[asset]=std::filesystem::last_write_time(Mesh::ResolveFilePath(asset),error);
            for (const auto& variant:lods.meshes)
                stamps[variant]=std::filesystem::last_write_time(Mesh::ResolveFilePath(variant),error);
        }
    if (m_prefabInstances && m_prefabInstances->provider==provider &&
        m_prefabInstances->signature==signature && m_prefabInstances->stamps==stamps && !changed)
    { m_prefabInstances->nextCheck=now+std::chrono::seconds(1); return; }
    auto* scene=Owner->GetScene();
    if (m_prefabInstances)
        for (auto id:m_prefabInstances->generated)
            if (auto* object=scene->FindObjectById(id)) scene->RemoveObject(object);
    auto runtime=std::make_shared<PrefabInstances>();
    runtime->provider=provider; runtime->signature=signature; runtime->stamps=std::move(stamps);
    runtime->nextCheck=now+std::chrono::seconds(1);
    // Definitions are needed only while deriving batches and collider copies.
    // Retaining them duplicates every source CPU mesh for the layer lifetime.
    Engine::Scene::Scene prototypes;
    const auto create=[&](const std::string& name) {
        auto* object=scene->AddObject(name); object->runtimeOnly=true;
        scene->MoveObject(object,Owner,Engine::Scene::Scene::ObjectPlacement::AsChild);
        object->transform.position={0,0,0}; object->transform.rotation={0,0,0};
        object->transform.scale={1,1,1};
        runtime->generated.push_back(scene->GetObjectId(object)); return object;
    };
    std::map<std::string,Engine::Core::Object*> definitions;
    const auto definition=[&](const std::string& asset) {
        auto& root=definitions[asset];
        if (!root) root=Engine::Serialization::SceneSerializer::InstantiatePrefab(
            prototypes,Mesh::ResolveFilePath(asset),nullptr);
        return root;
    };
    for (const auto& [asset,records]:groups)
    {
        auto* root=definition(asset);
        if (!root) continue;
        const auto placementMatrix=[](const Placement& placement) {
            return glm::translate(glm::mat4(1),placement.position)*
                glm::toMat4(SurfaceRotation(placement.normal,placement.yawDegrees))*
                glm::scale(glm::mat4(1),glm::vec3(placement.scale));
        };
        // CPU objects provide coarse spatial bounds; the compute path still
        // selects visibility and LOD for each placement. Small 64 m groups
        // multiplied every prefab part/band into thousands of CPU components.
        constexpr float prefabGroupSpan=256.f;
        std::map<std::pair<int,int>,std::vector<const Placement*>> tiles;
        for (const auto* placement:records)
            tiles[{int(std::floor(placement->position.x/prefabGroupSpan)),
                int(std::floor(placement->position.z/prefabGroupSpan))}].push_back(placement);
        std::map<std::pair<int,int>,std::shared_ptr<Mesh::InstanceData>> tileInstances;
        std::vector<std::string> chain{asset};
        if (const auto lods=objectLods.find(asset); lods!=objectLods.end() && !lods->second.meshes.empty())
            chain=lods->second.meshes;
        // The renderer's three resident bands support material changes per level.
        if (chain.size()>3) chain={chain.front(),chain[1],chain.back()};
        const auto distances=GetObjectLodDistances(asset,chain.size());
        for (size_t lod=0;lod<chain.size();++lod)
        {
        auto* lodRoot=definition(chain[lod]);
        if (!lodRoot) continue;
        for (auto* original:lodRoot->GetComponentsInChildren<Mesh>())
        {
            if (!original->Owner->IsEnabledInHierarchy()) continue;
            // Expand indexed geometry once per shared prefab part, never per placement.
            auto source=std::make_shared<Mesh>();
            auto vertices=original->GetVertices();
            if (!original->GetIndices().empty())
            {
                std::vector<Mesh::Vertex> expanded; expanded.reserve(original->GetIndices().size());
                for (auto index:original->GetIndices()) expanded.push_back(vertices.at(index));
                vertices=std::move(expanded);
            }
            if (vertices.empty()) continue;
            // Bake the prefab-part transform once so all parts and LODs can use
            // the exact same resident placement array, including impostors.
            const auto part=original->Owner->transform.GetWorldMatrix();
            const auto normalMatrix=glm::transpose(glm::inverse(glm::mat3(part)));
            for (auto& vertex:vertices)
            {
                const auto position=glm::vec3(part*glm::vec4(vertex.pos[0],vertex.pos[1],vertex.pos[2],1));
                auto normal=normalMatrix*glm::vec3(vertex.normal[0],vertex.normal[1],vertex.normal[2]);
                normal=glm::length(normal)>1.e-6f ? glm::normalize(normal):glm::vec3(0,1,0);
                auto tangent=glm::mat3(part)*glm::vec3(vertex.tangent[0],vertex.tangent[1],vertex.tangent[2]);
                tangent=glm::length(tangent)>1.e-6f ? glm::normalize(tangent):glm::vec3(1,0,0);
                for (int axis=0;axis<3;++axis) { vertex.pos[axis]=position[axis]; vertex.normal[axis]=normal[axis]; vertex.tangent[axis]=tangent[axis]; }
            }
            if (!source->SetAuthoredVertices(std::move(vertices)))
                throw std::runtime_error("Invalid shared prefab geometry: "+chain[lod]);
            source->CreateBuffer(provider->GetBufferFactory());
            for (const auto& [tile,tileRecords]:tiles)
            {
            auto& instances=tileInstances[tile];
            const bool initialize=!instances;
            if (initialize) instances=std::make_shared<Mesh::InstanceData>();
            glm::vec3 minimum(std::numeric_limits<float>::max()),maximum(-minimum);
            for (const auto* placement:tileRecords)
            {
                const glm::mat4 matrix=placementMatrix(*placement);
                Mesh::InstanceTransform instance{};
                for (int row=0;row<3;++row)
                    instance.rows[row]={matrix[0][row],matrix[1][row],matrix[2][row],matrix[3][row]};
                if (initialize) { instances->transforms.push_back(instance); instances->placementIds.push_back(placement->id); }
                for (int corner=0;corner<8;++corner)
                {
                    const auto low=source->GetBoundsMin(),high=source->GetBoundsMax();
                    const auto point=glm::vec3(matrix*glm::vec4(corner&1 ? high.x:low.x,
                        corner&2 ? high.y:low.y,corner&4 ? high.z:low.z,1));
                    minimum=glm::min(minimum,point); maximum=glm::max(maximum,point);
                }
            }
            auto* object=create("Shared prefab part: "+original->Owner->name+" ("+
                std::to_string(tile.first)+","+std::to_string(tile.second)+")");
            auto* mesh=object->AddComponent<Mesh>();
            mesh->useBoundsForShadowCulling=true;
            mesh->foliageWindStrength=original->foliageWindStrength;
            mesh->runtimeShadowDistance=std::max(0.f,shadowDistance);
            mesh->prefabShadowLod=shadowLod<0 ? -1:std::min(shadowLod,int(chain.size())-1);
            if (chain.size()>1)
            {
                mesh->prefabLodBand=int(lod);
                mesh->prefabLodDistances={distances[0],distances.size()>1 ? distances[1]:1.e20f};
            }
            mesh->SetInstancedGeometry(source,instances,{{0,source->GetVertexCount(),0,
                static_cast<uint32_t>(tileRecords.size())}},minimum,maximum);
            if (const auto* material=original->Owner->GetComponent<Material>())
            {
                auto* copy=object->AddComponent<Material>();
                copy->Deserialize(material->Serialize()); copy->OnAfterDeserialize(provider);
                if (shadowDistance<=0) copy->castsShadows=false;
            }
            }
        }
        }
        // Collider policy belongs to the shared prefab. None are synthesized
        // when it has no collider; adding one later applies to every placement.
        auto* colliderRoot=root;
        const auto sameAsset=[](const std::string& a,const std::string& b) {
            return std::filesystem::absolute(Mesh::ResolveFilePath(a)).lexically_normal()==
                std::filesystem::absolute(Mesh::ResolveFilePath(b)).lexically_normal();
        };
        for (const auto& [base,lods]:objectLods)
            if (std::filesystem::path(base).extension()==".prefab" &&
                (sameAsset(base,asset) || std::any_of(lods.meshes.begin(),lods.meshes.end(),
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
    m_prefabInstances=std::move(runtime);
}
}
