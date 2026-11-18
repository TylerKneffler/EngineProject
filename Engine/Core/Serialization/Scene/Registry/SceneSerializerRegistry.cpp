#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/Scene/Scene.h"
#include "Core/Scene/Settings/SceneSettingsSerialization.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Scene/Object.h"
#include "Core/Components/Component.h"
#include "Core/Components/Script.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Mesh/MeshInstanceLayer.h"
#include "Core/Components/Mesh/MeshLodProfile.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Lighting/Light.h"
#include "Core/Components/Lighting/LightProbe.h"
#include "Core/Components/Sprite/Sprite.h"
#include "Core/Components/Audio/AudioSource.h"
#include "Core/Components/Physics/PrimitiveObjectCollider.h"
#include "Core/Components/Physics/MeshObjectCollider.h"
#include "Core/Components/Physics/Cloth.h"
#include "Core/Components/Physics/RigidBody.h"
#include "Core/Components/UI/Canvas.h"
#include "Core/Components/UI/UIButton.h"
#include "Core/Components/UI/UIImage.h"
#include "Core/Components/UI/UIObject.h"
#include "Core/Components/UI/UIText.h"
#include "Core/Components/Sprite/SpriteAnimationManager.h"
#include "Core/Components/Particles/ParticleEmitter.h"
#include "Core/Components/Animation/Model.h"
#include "Core/Components/Animation/AnimationManager.h"
#include "Core/Components/Cinematics/SceneCameraLightTrack.h"
#include "Core/Components/Animation/Skeleton.h"
#include "Core/Components/Animation/AnimationBone.h"
#include "Core/Components/Animation/BoneGroup.h"
#include "Core/Components/Animation/SkinnedMesh.h"
#include "Core/Components/Animation/SoftTissueSurface.h"
#include "Core/Components/Animation/IKBone.h"
#include "Core/Components/Animation/TwoBoneIK.h"
#include "Core/Components/Animation/ChainIK.h"
#include "Core/Components/Animation/RigController.h"
#include "Core/Components/Animation/RigConstraints.h"
#include "Core/Components/Animation/RigMorphControls.h"
#include "Core/Components/Physics/RigPhysicsBinding.h"
#include "Core/Components/Materials/MaterialPropertyMorph.h"
#include "Core/Components/Constraints/Subconstraints/AimConstraint.h"
#include "Core/Components/Constraints/Subconstraints/ParentConstraint.h"
#include "Core/Components/Constraints/Subconstraints/PositionConstraint.h"
#include "Core/Components/Constraints/Subconstraints/RotationConstraint.h"
#include "Core/Components/Physics/SpatialManipulator.h"
#include "Core/Components/Path/SplinePath.h"
#include "Core/Components/Path/SplineGeometry.h"
#include "Core/Components/Path/SplineInstancePlacementProvider.h"
#include "Core/Components/Path/SplineFollower.h"
#include "Core/Components/Cinematics/CameraTrack.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include <pugixml.hpp>
#include <fstream>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace Engine::Serialization
{
namespace
{
struct RegisteredTypeMetadata
{
    bool editorAddable = true;
    bool script = false;
};

struct RegistryPickerCache
{
    std::unordered_map<std::string, RegisteredTypeMetadata> metadata;
    std::vector<std::string> componentTypes;
    std::vector<std::string> scriptTypes;
    bool dirty = true;
};

RegistryPickerCache& PickerCache()
{
    static RegistryPickerCache cache;
    return cache;
}

void CacheRegistrationMetadata(const std::string& typeName,
    const Engine::Core::Component& prototype)
{
    PickerCache().metadata[typeName] = {
        prototype.editorAddable,
        dynamic_cast<const Engine::Core::Script*>(&prototype) != nullptr
    };
    PickerCache().dirty = true;
}

void RebuildPickerCache()
{
    RegistryPickerCache& cache = PickerCache();
    if (!cache.dirty)
        return;

    cache.componentTypes.clear();
    cache.scriptTypes.clear();
    cache.componentTypes.reserve(cache.metadata.size());
    cache.scriptTypes.reserve(cache.metadata.size());
    for (const auto& entry : cache.metadata)
    {
        if (entry.second.editorAddable)
            cache.componentTypes.push_back(entry.first);
        if (entry.second.script)
            cache.scriptTypes.push_back(entry.first);
    }
    std::sort(cache.componentTypes.begin(), cache.componentTypes.end(),
        [](const std::string& left, const std::string& right)
        {
            return std::lexicographical_compare(
                left.begin(), left.end(), right.begin(), right.end(),
                [](unsigned char a, unsigned char b)
                {
                    return std::tolower(a) < std::tolower(b);
                });
        });
    std::sort(cache.scriptTypes.begin(), cache.scriptTypes.end());
    cache.dirty = false;
}
}

// ---- Registry ---------------------------------------------------------------
std::unordered_map<std::string, SceneSerializer::Factory>& SceneSerializer::GetRegistry()
{
    static std::unordered_map<std::string, Factory> s_registry;
    return s_registry;
}

void SceneSerializer::Register(const std::string& typeName, Factory factory)
{
    if (!factory)
        throw std::invalid_argument("Cannot register an empty component factory");
    std::unique_ptr<Engine::Core::Component> prototype(factory());
    if (!prototype)
        throw std::invalid_argument("Registered component factories must create an instance");
    CacheRegistrationMetadata(typeName, *prototype);
    GetRegistry()[typeName] = std::move(factory);
}

void SceneSerializer::Unregister(const std::string& typeName)
{
    GetRegistry().erase(typeName);
    PickerCache().metadata.erase(typeName);
    PickerCache().dirty = true;
}

SceneSerializer::Factory SceneSerializer::GetRegisteredFactory(
    const std::string& typeName)
{
    auto found = GetRegistry().find(typeName);
    return found == GetRegistry().end() ? Factory{} : found->second;
}

void SceneSerializer::Register(Factory factory)
{
    if (!factory)
        throw std::invalid_argument("Cannot register an empty component factory");

    std::unique_ptr<Engine::Core::Component> prototype(factory());
    if (!prototype || prototype->GetTypeName().empty())
        throw std::invalid_argument("Registered components must provide a type name");

    const std::string typeName = prototype->GetTypeName();
    CacheRegistrationMetadata(typeName, *prototype);
    GetRegistry()[typeName] = std::move(factory);
}

void SceneSerializer::EnsureBuiltinsRegistered()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;

    RegisterComponentType<Engine::Components::Mesh>();
    RegisterComponentType<Engine::Components::MeshInstanceLayer>();
    RegisterComponentType<Engine::Components::MeshLodProfile>();
    RegisterComponentType<Engine::Components::Terrain>();
    RegisterComponentType<Engine::Components::TerrainObjectLayer>();
    RegisterComponentType<Engine::Components::TerrainManager>();
    RegisterComponentType<Engine::Components::Material>();
    RegisterComponentType<Engine::Components::Camera>();
    RegisterComponentType<Engine::Components::Light>();
    RegisterComponentType<Engine::Components::LightProbe>();
    RegisterComponentType<Engine::Components::LightProbeGroup>();
    RegisterComponentType<Engine::Components::Sprite>();
    RegisterComponentType<Engine::Components::AudioSource>();
    RegisterComponentType<Engine::Components::RigidBody>();
    RegisterComponentType<Engine::Components::PrimitiveObjectCollider>();
    RegisterComponentType<Engine::Components::MeshObjectCollider>();
    RegisterComponentType<Engine::Components::Cloth>();
    RegisterComponentType<Engine::Components::Canvas>();
    RegisterComponentType<Engine::Components::UIButton>();
    RegisterComponentType<Engine::Components::UIImage>();
    RegisterComponentType<Engine::Components::UIObject>();
    RegisterComponentType<Engine::Components::UIText>();
    RegisterComponentType<Engine::Components::SpriteAnimationManager>();
    RegisterComponentType<Engine::Components::ParticleEmitter>();
    RegisterComponentType<Engine::Components::Model>();
    RegisterComponentType<Engine::Components::AnimationManager>();
    RegisterComponentType<Engine::Components::SceneCameraLightTrack>();
    RegisterComponentType<Engine::Components::Skeleton>();
    RegisterComponentType<Engine::Components::AnimationBone>();
    RegisterComponentType<Engine::Components::BoneGroup>();
    RegisterComponentType<Engine::Components::SkinnedMesh>();
    RegisterComponentType<Engine::Components::SoftTissueSurface>();
    RegisterComponentType<Engine::Components::IKBone>();
    RegisterComponentType<Engine::Components::TwoBoneIK>();
    RegisterComponentType<Engine::Components::ChainIK>();
    RegisterComponentType<Engine::Components::RigController>();
    RegisterComponentType<Engine::Components::RigConstraints>();
    RegisterComponentType<Engine::Components::RigMorphControls>();
    RegisterComponentType<Engine::Components::RigPhysicsBinding>();
    RegisterComponentType<Engine::Components::MaterialPropertyMorph>();
    RegisterComponentType<Engine::Components::AimConstraint>();
    RegisterComponentType<Engine::Components::ParentConstraint>();
    RegisterComponentType<Engine::Components::PositionConstraint>();
    RegisterComponentType<Engine::Components::RotationConstraint>();
    RegisterComponentType<Engine::Components::SpatialManipulator>();
    RegisterComponentType<SplinePath>();
    RegisterComponentType<SplineGeometry>();
    RegisterComponentType<SplineInstancePlacementProvider>();
    RegisterComponentType<SplineFollower>();
    RegisterComponentType<CameraTrack>();
    RegisterComponentType<Engine::Rendering::BakedLightingData>();
}

Engine::Core::Component* SceneSerializer::CreateRegisteredComponent(const std::string& typeName)
{
    EnsureBuiltinsRegistered();
    auto& registry = GetRegistry();
    auto found = registry.find(typeName);
    if (found == registry.end())
    {
        found = std::find_if(registry.begin(), registry.end(), [&typeName](const auto& entry)
        {
            return entry.first.size() == typeName.size() &&
                std::equal(entry.first.begin(), entry.first.end(), typeName.begin(),
                    [](unsigned char left, unsigned char right)
                    {
                        return std::tolower(left) == std::tolower(right);
                    });
        });
    }
    return found != registry.end() && found->second ? found->second() : nullptr;
}

const std::vector<std::string>& SceneSerializer::GetRegisteredComponentTypes()
{
    EnsureBuiltinsRegistered();
    RebuildPickerCache();
    return PickerCache().componentTypes;
}

std::vector<std::string> SceneSerializer::GetRegisteredScriptTypes()
{
    EnsureBuiltinsRegistered();
    RebuildPickerCache();
    return PickerCache().scriptTypes;
}

}
