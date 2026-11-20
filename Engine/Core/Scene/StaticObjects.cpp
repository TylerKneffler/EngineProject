#include "pch.h"
#include "Core/Scene/StaticObjects.h"

#include "Core/Components/Materials/Material.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Physics/PrimitiveObjectCollider.h"
#include "Core/Components/Physics/RigidBody.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Engine::Scene::StaticObjects
{
namespace
{
using Engine::Core::Component;
using Engine::Core::Object;
using Engine::Components::TerrainObjectLayer;

std::mutex g_sinkMutex;
std::function<void(const std::string&)> g_sink;

// Components that drive their owner's transform (or its bones) every frame.
// Matched by type name so this file does not depend on every such component;
// anything missing here is still caught while playing by MotionWatch.
const char* const MovingComponents[] = {
    "FirstPersonController", "SplineFollower", "TransformConstraint",
    "AnimationManager", "Skeleton", "IKBone", "ChainIK", "TwoBoneIK",
    "GroundedFootIK", "CameraTrack", "SceneCameraLightTrack", "Cloth"};

// Components whose serialized state is large and holds no object names;
// skipped when looking for references by name.
const char* const DataComponents[] = {
    "Mesh", "Material", "Terrain", "TerrainObjectLayer", "GrassField",
    "MeshInstanceLayer", "BakedLightingData"};

template<size_t N>
bool Listed(const char* const (&names)[N], const std::string& type)
{
    return std::any_of(std::begin(names), std::end(names),
        [&](const char* name) { return type == name; });
}

std::string Label(const Object& object)
{
    return "Static object '" + object.name + "'";
}

// Contradictions between the flag and one object's own components and parent.
void AppendContradictions(const Object& object, std::vector<Note>& notes)
{
    if (object.isStatic && object.Parent && !object.Parent->IsStaticInHierarchy())
        notes.push_back({Severity::Warning, "its parent '" + object.Parent->name +
            "' is not static, so moving the parent moves it"});
    for (const Component* component : object.Components)
    {
        if (!component) continue;
        const std::string type = component->GetTypeName();
        if (const auto* body = dynamic_cast<const Engine::Components::RigidBody*>(component))
        {
            if (body->bodyType != "Static")
                notes.push_back({Severity::Warning, "it has a " + body->bodyType +
                    " rigid body, which moves it; set the body type to Static or clear the flag"});
        }
        else if (Listed(MovingComponents, type))
            notes.push_back({Severity::Warning, "its " + type +
                " component moves it; clear the flag or remove the component"});
    }
}

void CollectHierarchy(const Object& object, std::vector<const Object*>& output)
{
    output.push_back(&object);
    for (const Object* child : object.Children)
        if (child) CollectHierarchy(*child, output);
}

// The placement (cell-local to an identity owner, i.e. world space) that
// reproduces a world matrix, or false when the matrix is not a rotation with
// a uniform positive scale.
bool PlacementFromMatrix(const glm::mat4& matrix, TerrainObjectLayer::Placement& placement,
    std::string& reason)
{
    const glm::vec3 axes[3] = {glm::vec3(matrix[0]), glm::vec3(matrix[1]), glm::vec3(matrix[2])};
    const float lengths[3] = {glm::length(axes[0]), glm::length(axes[1]), glm::length(axes[2])};
    const float largest = std::max({lengths[0], lengths[1], lengths[2]});
    const float smallest = std::min({lengths[0], lengths[1], lengths[2]});
    char text[160];
    if (!(smallest > 1.e-6f) || !std::isfinite(largest))
    {
        reason = "its scale is zero or invalid";
        return false;
    }
    if (largest - smallest > largest * .002f)
    {
        std::snprintf(text, sizeof(text), "its world scale is not uniform (%.3g, %.3g, %.3g)",
            lengths[0], lengths[1], lengths[2]);
        reason = text;
        return false;
    }
    const glm::mat3 rotation(axes[0] / lengths[0], axes[1] / lengths[1], axes[2] / lengths[2]);
    if (glm::determinant(rotation) < 0.f)
    {
        reason = "it is mirrored (negative scale)";
        return false;
    }
    if (std::abs(glm::dot(rotation[0], rotation[1])) > 2.e-3f ||
        std::abs(glm::dot(rotation[0], rotation[2])) > 2.e-3f ||
        std::abs(glm::dot(rotation[1], rotation[2])) > 2.e-3f)
    {
        reason = "its world transform is sheared (rotated under a non-uniformly scaled parent)";
        return false;
    }
    placement.position = glm::vec3(matrix[3]);
    placement.scale = (lengths[0] + lengths[1] + lengths[2]) / 3.f;
    placement.normal = rotation[1];
    const glm::quat total = glm::quat_cast(rotation);
    const glm::quat aligned = TerrainObjectLayer::SurfaceRotation(placement.normal, 0.f);
    const glm::quat turn = glm::normalize(glm::inverse(aligned) * total);
    placement.yawDegrees = glm::degrees(2.f * std::atan2(turn.y, turn.w));
    // The layer rebuilds the rotation from normal and yaw; keep the object
    // unless that gives back the same orientation.
    const glm::mat3 rebuilt = glm::mat3_cast(
        TerrainObjectLayer::SurfaceRotation(placement.normal, placement.yawDegrees));
    for (int axis = 0; axis < 3; ++axis)
        if (glm::dot(rebuilt[axis], rotation[axis]) < .99999f)
        {
            reason = "its rotation could not be expressed as a placement";
            return false;
        }
    return true;
}

struct Candidate
{
    bool considered = false;   // a static prefab instance root with geometry
    bool bakeable = false;
    Severity severity = Severity::Info;
    std::string reason;        // why it stays a scene object
    TerrainObjectLayer::Placement placement;
    std::string prefab;
    std::vector<float> lodDistances;
    bool cullAfterLastLod = false;
    float finalLodCullDistance = 0.f;
};

Candidate Evaluate(const Object& object)
{
    Candidate result;
    if (!object.Prefab || !object.IsStaticInHierarchy()) return result;
    // An instance nested inside another prefab instance is the outer one's.
    if (object.Parent && object.Parent->GetPrefabInstanceRoot()) return result;
    std::vector<const Object*> hierarchy;
    CollectHierarchy(object, hierarchy);
    const Engine::Components::Mesh* firstMesh = nullptr;
    std::string blocking;
    bool transparent = false, movingBody = false;
    for (const Object* member : hierarchy)
        for (const Component* component : member->Components)
        {
            if (!component) continue;
            if (const auto* mesh = dynamic_cast<const Engine::Components::Mesh*>(component))
            {
                if (!firstMesh || (!firstMesh->gpuLodSelection && mesh->gpuLodSelection))
                    firstMesh = mesh;
            }
            else if (const auto* material =
                dynamic_cast<const Engine::Components::Material*>(component))
                transparent |= material->alphaMode == "Blend";
            else if (const auto* body =
                dynamic_cast<const Engine::Components::RigidBody*>(component))
                movingBody |= body->bodyType != "Static";
            else if (!dynamic_cast<const Engine::Components::PrimitiveObjectCollider*>(component) &&
                blocking.empty())
                blocking = component->GetTypeName();
        }
    if (!firstMesh) return result; // not geometry: a light, an emitter, a marker
    result.considered = true;
    result.prefab = object.Prefab->GetPath();
    if (!blocking.empty())
    {
        result.reason = "the prefab has a " + blocking +
            " component, so it stays a scene object";
        return result;
    }
    if (transparent)
    {
        result.reason = "it has a blended (transparent) material, so it stays a scene object";
        return result;
    }
    if (movingBody)
    {
        result.reason = "its rigid body is not Static"; // reported as a contradiction
        return result;
    }
    if (!object.IsEnabledInHierarchy())
    {
        result.reason = "it is disabled";
        return result;
    }
    result.severity = Severity::Warning;
    if (!PlacementFromMatrix(object.transform.GetWorldMatrix(), result.placement, result.reason))
    {
        result.reason = "not batched: " + result.reason;
        return result;
    }
    if (Engine::Serialization::SceneSerializer::HasPrefabOverrides(object, false))
    {
        result.reason = "not batched: this instance has prefab overrides; "
            "revert them or apply them to the prefab";
        return result;
    }
    if (firstMesh->gpuLodSelection)
        result.lodDistances = {firstMesh->gpuLod1Distance,
            std::max(firstMesh->gpuLod1Distance, firstMesh->gpuLod2Distance)};
    result.cullAfterLastLod = firstMesh->cullAfterLastLod;
    result.finalLodCullDistance = firstMesh->finalLodCullDistance;
    result.severity = Severity::Info;
    result.bakeable = true;
    return result;
}

void CollectStrings(const Engine::Serialization::JsonValue& value,
    std::unordered_set<std::string>& output)
{
    if (value.IsString())
    {
        if (!value.AsString().empty()) output.insert(value.AsString());
    }
    else if (value.IsArray())
        for (size_t index = 0; index < value.ArraySize(); ++index)
            CollectStrings(value.ArrayAt(index), output);
    else if (value.IsObject())
        for (size_t index = 0; index < value.ObjectSize(); ++index)
            CollectStrings(value.ObjectValue(index), output);
}

// Every string stored in a component field anywhere in the scene, except in
// the given objects' own hierarchies: the names components may look up.
std::unordered_set<std::string> ReferencedStrings(const Engine::Scene::Scene& scene,
    const std::unordered_set<const Object*>& excludedRoots)
{
    std::unordered_set<std::string> strings;
    for (const auto& owned : scene.GetObjects())
    {
        const Object* object = owned.get();
        if (!object || object->runtimeOnly) continue;
        bool excluded = false;
        for (const Object* current = object; current && !excluded; current = current->Parent)
            excluded = excludedRoots.count(current) > 0;
        if (excluded) continue;
        for (const Component* component : object->Components)
        {
            if (!component || Listed(DataComponents, component->GetTypeName())) continue;
            try { CollectStrings(component->Serialize(), strings); }
            catch (...) {}
        }
    }
    return strings;
}
}

void SetWarningSink(std::function<void(const std::string&)> sink)
{
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    g_sink = std::move(sink);
}

void Warn(const std::string& message)
{
    std::function<void(const std::string&)> sink;
    {
        std::lock_guard<std::mutex> lock(g_sinkMutex);
        sink = g_sink;
    }
    if (sink)
    {
        sink(message);
        return;
    }
    std::fprintf(stderr, "[Static] %s\n", message.c_str());
#ifdef _WIN32
    OutputDebugStringA(("[Static] " + message + "\n").c_str());
#endif
}

std::vector<Note> Describe(const Engine::Core::Object& object)
{
    std::vector<Note> notes;
    if (!object.IsStaticInHierarchy()) return notes;
    AppendContradictions(object, notes);
    const Candidate candidate = Evaluate(object);
    if (candidate.bakeable)
        notes.push_back({Severity::Info, "Drawn as an instanced placement while playing, "
            "together with the other static instances of this prefab (needs at least two)."});
    else if (candidate.considered)
        notes.push_back({candidate.severity, candidate.reason});
    else if (!object.isStatic)
        notes.push_back({Severity::Info, "Static through its parent."});
    return notes;
}

BakeResult BakeForPlay(Engine::Scene::Scene& scene)
{
    BakeResult result;
    const auto warn = [&](const Object& object, const std::string& text)
    {
        result.warnings.push_back(Label(object) + ": " + text + ".");
        Warn(result.warnings.back());
    };
    struct Pending { Object* object; Candidate candidate; };
    std::vector<Pending> pending;
    for (const auto& owned : scene.GetObjects())
    {
        Object* object = owned.get();
        if (!object || object->runtimeOnly || !object->IsStaticInHierarchy()) continue;
        if (object->isStatic) ++result.staticObjects;
        std::vector<Note> notes;
        AppendContradictions(*object, notes);
        for (const Note& note : notes) warn(*object, note.text);
        Candidate candidate = Evaluate(*object);
        if (candidate.bakeable)
            pending.push_back({object, std::move(candidate)});
        else if (candidate.considered && candidate.severity == Severity::Warning)
            warn(*object, candidate.reason);
    }
    const char* setting = std::getenv("ENGINE_STATIC_BAKE");
    if (pending.empty() || (setting && setting[0] == '0')) return result;

    // Objects that some component names stay scene objects: a lookup by name
    // at runtime must keep finding them.
    std::unordered_set<const Object*> roots;
    for (const Pending& entry : pending) roots.insert(entry.object);
    const auto referenced = ReferencedStrings(scene, roots);
    std::map<std::string, std::vector<const Pending*>> byPrefab;
    for (const Pending& entry : pending)
    {
        if (!entry.object->name.empty() && referenced.count(entry.object->name))
        {
            warn(*entry.object, "not batched: a component in the scene refers to the name '" +
                entry.object->name + "'");
            continue;
        }
        byPrefab[entry.candidate.prefab].push_back(&entry);
    }

    Object* holder = nullptr;
    TerrainObjectLayer* layer = nullptr;
    std::vector<Object*> baked;
    for (const auto& [prefab, entries] : byPrefab)
    {
        // A single instance gains nothing from the instanced path.
        if (entries.size() < 2) continue;
        if (!holder)
        {
            holder = scene.AddObject("Static objects (baked)");
            if (!holder) return result;
            holder->runtimeOnly = true;
            holder->isStatic = true;
            layer = holder->AddComponent<TerrainObjectLayer>();
            layer->layerName = "Static objects";
            // Scene objects have no view distance: keep every level's reach
            // and let each prefab's own LOD distances pick the level.
            layer->viewDistance = 100000.f;
            layer->lod1Distance = 60.f;
            layer->lod2Distance = 220.f;
            layer->shadowDistance = 1000.f;
            layer->placementsChangeOnlyWhenMarked = true;
        }
        const Candidate& first = entries.front()->candidate;
        if (!first.lodDistances.empty() || first.cullAfterLastLod)
        {
            auto& lods = layer->objectLods[prefab];
            lods.distances = first.lodDistances;
            lods.cullAfterLastLod = first.cullAfterLastLod;
            lods.finalLodCullDistance = first.finalLodCullDistance;
        }
        for (const Pending* entry : entries)
        {
            TerrainObjectLayer::Placement placement = entry->candidate.placement;
            // The object's scene id, so a placement can be traced back to it.
            placement.id = scene.GetObjectId(entry->object);
            placement.asset = prefab;
            layer->placements.push_back(std::move(placement));
            baked.push_back(entry->object);
        }
        ++result.bakedPrefabs;
    }
    if (!layer) return result;
    layer->MarkConfigurationDirty();
    for (Object* object : baked) scene.RemoveObject(object);
    result.bakedObjects = baked.size();
    // Not a warning: stderr and the debugger only.
    char summary[160];
    std::snprintf(summary, sizeof(summary),
        "[Static] %zu static prefab instances of %zu prefabs drawn as instanced placements\n",
        result.bakedObjects, result.bakedPrefabs);
    std::fputs(summary, stderr);
#ifdef _WIN32
    OutputDebugStringA(summary);
#endif
    return result;
}

void MotionWatch::Begin(Engine::Scene::Scene& scene)
{
    m_entries.clear();
    m_next = 0;
    for (const auto& owned : scene.GetObjects())
    {
        const Object* object = owned.get();
        if (!object || object->runtimeOnly || !object->isStatic) continue;
        m_entries.push_back({scene.GetObjectId(object),
            object->transform.GetWorldRevision(), false});
    }
}

void MotionWatch::Check(Engine::Scene::Scene& scene, size_t objectsPerCall)
{
    if (m_entries.empty()) return;
    const size_t count = std::min(objectsPerCall, m_entries.size());
    for (size_t step = 0; step < count; ++step)
    {
        Entry& entry = m_entries[m_next];
        m_next = (m_next + 1) % m_entries.size();
        if (entry.reported) continue;
        const Object* object = scene.FindObjectById(entry.id);
        if (!object) { entry.reported = true; continue; }
        if (object->transform.GetWorldRevision() == entry.revision) continue;
        entry.reported = true;
        Warn(Label(*object) + ": it moved while playing. Something changes its transform "
            "(a script, an animation or a moving parent); clear the flag or stop moving it.");
    }
}
}
