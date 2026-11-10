#pragma once
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
#include "Core/Serialization/SceneSerializer.h"
#include <filesystem>
#include <cstdlib>
#include <ostream>
#include <stdexcept>

// Test-only substitution. Historical studies retain the explicit original LOD1
// baseline; follow-up asset studies can opt into the installed canopy baseline.
inline unsigned ConfigureCanopy(Engine::Components::TerrainObjectLayer& layer,
    const std::filesystem::path& directory, bool candidate, std::ostream* report=nullptr)
{
    using Engine::Components::Mesh;
    unsigned replaced=0;
    for(auto& [asset,chain]:layer.objectLods) for(int variety=1;variety<=4;++variety)
    {
        const std::string name="Large_oak_tree_00"+std::to_string(variety);
        if(asset.find(name+"_LOD0.prefab")==std::string::npos) continue;
        if(chain.meshes.size()!=3) throw std::runtime_error("Expected three canopy bands");
        const bool installedBaseline=std::getenv("ENGINE_OAK_CANOPY_INSTALLED_BASELINE")!=nullptr;
        const auto path=candidate ? directory/name/(name+"_CanopyCandidate.prefab") :
            std::filesystem::path("Engine/Core/Assets/Prefabs/Woodland/Oaks")/
                (name+(installedBaseline ? "_Canopy.prefab":"_LOD1.prefab"));
        if(candidate && !std::filesystem::exists(path)) continue;
        Engine::Scene::Scene validation;
        auto* root=Engine::Serialization::SceneSerializer::InstantiatePrefab(validation,path.generic_string(),nullptr);
        if(!root) throw std::runtime_error("Canopy prefab failed to load: "+path.string());
        const auto meshes=root->GetComponentsInChildren<Mesh>();
        if(meshes.size()!=2) throw std::runtime_error("Canopy must load both bark and leaves");
        size_t triangles=0;
        for(auto* mesh:meshes)
        {
            if(mesh->GetVertices().empty()) throw std::runtime_error("Empty canopy geometry");
            triangles+=(mesh->GetIndices().empty()?mesh->GetVertices().size():mesh->GetIndices().size())/3;
        }
        if(report) *report<<name<<" variant="<<(candidate?"candidate":"baseline")
            <<" prefab="<<path.generic_string()<<" validatedCandidateTriangles="<<triangles<<'\n';
        chain.meshes[1]=path.generic_string(); ++replaced;
    }
    layer.MarkConfigurationDirty();
    return replaced;
}
