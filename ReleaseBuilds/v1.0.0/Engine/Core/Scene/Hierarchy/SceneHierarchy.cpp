#include "Core/Scene/Scene.h"
#include "Core/Audio/Audio.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Physics/Physics.h"
#include "Core/Renderers/UIRenderer.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_set>

namespace Engine::Scene
{
namespace
{
void DisconnectSpatialManipulators(Engine::Core::Object* object)
{
    if (!object)
        return;
    for (Engine::Core::Component* component : object->Components)
        if (auto* manipulator = dynamic_cast<
                Engine::Components::SpatialManipulator*>(component))
            manipulator->Disconnect();
}

bool ContainsObject(const Engine::Core::Object* root,
    const Engine::Core::Object* candidate)
{
    if (!root)
        return false;
    if (root == candidate)
        return true;
    for (const Engine::Core::Object* child : root->Children)
        if (ContainsObject(child, candidate))
            return true;
    return false;
}
}

Scene::Scene()
    : m_physics(std::make_unique<Engine::Physics::Physics>(*this))
    , m_audio(std::make_unique<Engine::Audio::Audio>(*this))
{
}

Scene::~Scene()
{
    m_audio->Reset();
    m_physics->Reset();
    for (const auto& object : m_objects)
        DisconnectSpatialManipulators(object.get());
    for (const auto& object : m_objects)
        if (object)
            object->OwnerScene = nullptr;
}

void Scene::Start()
{
    for (const auto& object : m_objects)
        object->Start();
    m_hasStarted = true;
}

void Scene::Update(float deltaTime)
{
    m_isUpdating = true;
    for (const auto& object : m_objects)
        object->Update();
    m_audio->Update(deltaTime);
    m_physics->Step(deltaTime);
    // Bullet has now published current-frame poses and overlap pairs. Portal
    // traversal deliberately runs here rather than in Component::Update so a
    // crossing is evaluated against the motion that just occurred.
    struct OrderedPortal
    {
        Engine::Components::SpatialManipulator* manipulator = nullptr;
        ObjectPath path;
    };
    std::vector<OrderedPortal> portals;
    for (const auto& object : m_objects)
    {
        if (!object || !object->IsEnabledInHierarchy())
            continue;
        if (auto* manipulator = object->GetComponent<
            Engine::Components::SpatialManipulator>())
        {
            ObjectPath path;
            TryGetObjectPath(object.get(), path);
            portals.push_back({ manipulator, std::move(path) });
        }
    }
    std::sort(portals.begin(), portals.end(),
        [](const OrderedPortal& first, const OrderedPortal& second)
        {
            if (first.manipulator->portalTraversalPriority !=
                second.manipulator->portalTraversalPriority)
            {
                return first.manipulator->portalTraversalPriority >
                    second.manipulator->portalTraversalPriority;
            }
            return first.path < second.path;
        });
    std::unordered_set<const Engine::Components::RigidBody*> claimedBodies;
    for (const OrderedPortal& portal : portals)
        portal.manipulator->PostPhysicsUpdate(&claimedBodies);
    m_isUpdating = false;
    FlushPendingObjectAdditions();
    FlushPendingObjectRemovals();
}

Engine::Core::Object* Scene::AddObject()
{
    auto obj = std::make_unique<Engine::Core::Object>();
    Engine::Core::Object* raw = obj.get();
    raw->OwnerScene = this;
    if (m_isUpdating)
        m_pendingObjectAdditions.push_back(std::move(obj));
    else
        m_objects.push_back(std::move(obj));
    return raw;
}

Engine::Core::Object* Scene::AddObject(const std::string& name)
{
    Engine::Core::Object* obj = AddObject();
    obj->name = name;
    obj->OwnerScene = this;
    return obj;
}

Engine::Core::Object* Scene::FindObjectByName(const std::string& name)
{
    const Scene* constScene = this;
    return const_cast<Engine::Core::Object*>(constScene->FindObjectByName(name));
}

const Engine::Core::Object* Scene::FindObjectByName(const std::string& name) const
{
    const auto findNamed = [&name](const auto& objects)
        -> const Engine::Core::Object*
    {
        for (const auto& object : objects)
        {
            if (object && object->name == name)
                return object.get();
        }
        return nullptr;
    };
    if (const Engine::Core::Object* object = findNamed(m_objects))
        return object;
    return findNamed(m_pendingObjectAdditions);
}

bool Scene::TryGetObjectPath(const Engine::Core::Object* object, ObjectPath& path) const
{
    path.clear();
    if (!object)
        return false;

    for (const Engine::Core::Object* current = object; current; current = current->Parent)
    {
        std::size_t index = 0;
        bool found = false;
        if (current->Parent)
        {
            const auto& siblings = current->Parent->Children;
            const auto position =
                std::find(siblings.begin(), siblings.end(), current);
            if (position != siblings.end())
            {
                index = static_cast<std::size_t>(
                    position - siblings.begin());
                found = true;
            }
        }
        else
        {
            for (const auto& candidate : m_objects)
            {
                if (candidate->Parent)
                    continue;
                if (candidate.get() == current)
                {
                    found = true;
                    break;
                }
                ++index;
            }
        }

        if (!found)
        {
            path.clear();
            return false;
        }
        path.push_back(index);
    }

    std::reverse(path.begin(), path.end());
    return true;
}

Engine::Core::Object* Scene::FindObjectByPath(const ObjectPath& path) const
{
    if (path.empty())
        return nullptr;

    Engine::Core::Object* current = nullptr;
    std::size_t rootIndex = 0;
    for (const auto& candidate : m_objects)
    {
        if (candidate->Parent)
            continue;
        if (rootIndex++ == path.front())
        {
            current = candidate.get();
            break;
        }
    }
    if (!current)
        return nullptr;

    for (std::size_t depth = 1; depth < path.size(); ++depth)
    {
        if (path[depth] >= current->Children.size())
            return nullptr;
        current = current->Children[path[depth]];
    }
    return current;
}

bool Scene::MoveObject(
    Engine::Core::Object* object,
    Engine::Core::Object* target,
    ObjectPlacement placement)
{
    if (!object || object == target)
        return false;

    Engine::Core::Object* newParent = placement == ObjectPlacement::AsChild
        ? target
        : (target ? target->Parent : nullptr);
    for (Engine::Core::Object* ancestor = newParent; ancestor; ancestor = ancestor->Parent)
        if (ancestor == object)
            return false;

    const glm::mat4 oldWorld = object->transform.GetWorldMatrix();
    if (object->Parent)
    {
        auto& oldSiblings = object->Parent->Children;
        oldSiblings.erase(
            std::remove(oldSiblings.begin(), oldSiblings.end(), object),
            oldSiblings.end());
    }
    object->Parent = newParent;

    if (newParent)
    {
        auto& siblings = newParent->Children;
        if (placement == ObjectPlacement::AsChild || !target)
            siblings.push_back(object);
        else
        {
            auto position = std::find(siblings.begin(), siblings.end(), target);
            if (position == siblings.end())
                siblings.push_back(object);
            else
                siblings.insert(
                    placement == ObjectPlacement::After
                        ? position + 1
                        : position,
                    object);
        }
    }
    else
    {
        auto moving = std::find_if(
            m_objects.begin(),
            m_objects.end(),
            [object](const auto& value) { return value.get() == object; });
        if (moving == m_objects.end())
            return false;

        std::unique_ptr<Engine::Core::Object> owned = std::move(*moving);
        m_objects.erase(moving);
        auto position = target
            ? std::find_if(
                m_objects.begin(),
                m_objects.end(),
                [target](const auto& value)
                {
                    return value.get() == target;
                })
            : m_objects.end();
        if (position == m_objects.end())
            m_objects.push_back(std::move(owned));
        else
            m_objects.insert(
                placement == ObjectPlacement::After
                    ? position + 1
                    : position,
                std::move(owned));
    }

    const glm::mat4 parentWorld =
        newParent ? newParent->transform.GetWorldMatrix() : glm::mat4(1.f);
    const glm::mat4 local = glm::inverse(parentWorld) * oldWorld;
    object->transform.position = glm::vec3(local[3]);
    object->transform.scale = {
        glm::length(glm::vec3(local[0])),
        glm::length(glm::vec3(local[1])),
        glm::length(glm::vec3(local[2]))
    };

    glm::mat3 rotation(1.f);
    for (int column = 0; column < 3; ++column)
    {
        const float scale = object->transform.scale[column];
        if (scale > 0.000001f)
            rotation[column] = glm::vec3(local[column]) / scale;
    }

    const float y = std::asin(std::clamp(-rotation[0][2], -1.f, 1.f));
    const float cosineY = std::cos(y);
    const float x = std::abs(cosineY) > 0.00001f
        ? std::atan2(rotation[1][2], rotation[2][2])
        : std::atan2(-rotation[2][1], rotation[1][1]);
    const float z = std::abs(cosineY) > 0.00001f
        ? std::atan2(rotation[0][1], rotation[0][0])
        : 0.f;
    object->transform.rotation = { x, y, z };
    return true;
}

void Scene::RemoveObject(Engine::Core::Object* obj)
{
    if (!obj)
        return;

    if (m_isUpdating)
    {
        RequestRemoveObject(obj);
        return;
    }

    std::vector<Engine::Core::Object*> objectsToRemove;
    std::function<void(Engine::Core::Object*)> collect = [&](Engine::Core::Object* current)
    {
        if (!current)
            return;
        objectsToRemove.push_back(current);
        for (Engine::Core::Object* child : current->Children)
            collect(child);
    };
    collect(obj);

    // Sever external links while every endpoint and hierarchy path is still
    // valid. Component destructors then only need to clear local state.
    for (Engine::Core::Object* object : objectsToRemove)
        DisconnectSpatialManipulators(object);
    for (Engine::Core::Object* object : objectsToRemove)
        if (object)
            object->OwnerScene = nullptr;

    if (obj->Parent)
    {
        auto& siblings = obj->Parent->Children;
        siblings.erase(
            std::remove(siblings.begin(), siblings.end(), obj),
            siblings.end());
        obj->Parent = nullptr;
    }
    if (m_selectedObject &&
        std::find(
            objectsToRemove.begin(),
            objectsToRemove.end(),
            m_selectedObject) != objectsToRemove.end())
    {
        m_selectedObject = nullptr;
    }
    if (m_previewObject &&
        std::find(objectsToRemove.begin(), objectsToRemove.end(),
            m_previewObject) != objectsToRemove.end())
    {
        m_previewObject = nullptr;
    }

    m_objects.erase(
        std::remove_if(
            m_objects.begin(),
            m_objects.end(),
            [&](const std::unique_ptr<Engine::Core::Object>& candidate)
            {
                return std::find(
                    objectsToRemove.begin(),
                    objectsToRemove.end(),
                    candidate.get()) != objectsToRemove.end();
            }),
        m_objects.end());
}

void Scene::RequestRemoveObject(Engine::Core::Object* obj)
{
    if (!obj || std::find(m_pendingObjectRemovals.begin(),
            m_pendingObjectRemovals.end(), obj) != m_pendingObjectRemovals.end())
    {
        return;
    }
    m_pendingObjectRemovals.push_back(obj);
}

void Scene::FlushPendingObjectAdditions()
{
    for (auto& object : m_pendingObjectAdditions)
    {
        if (!object)
            continue;
        // A runtime-spawned object cannot receive Start() until all of its
        // components have been attached by its creator. That is now true at
        // this update boundary.
        if (m_hasStarted)
            object->Start();
        m_objects.push_back(std::move(object));
    }
    m_pendingObjectAdditions.clear();
}

void Scene::FlushPendingObjectRemovals()
{
    std::vector<Engine::Core::Object*> pending;
    pending.swap(m_pendingObjectRemovals);
    for (Engine::Core::Object* object : pending)
    {
        // An earlier request can remove a parent. Do not dereference the
        // stale child pointer; compare it only against live hierarchy nodes.
        const bool isLive = std::any_of(m_objects.begin(), m_objects.end(),
            [object](const std::unique_ptr<Engine::Core::Object>& root)
            {
                return ContainsObject(root.get(), object);
            });
        if (isLive)
            RemoveObject(object);
    }
}

void Scene::ClearObjects()
{
    m_pendingObjectAdditions.clear();
    m_pendingObjectRemovals.clear();
    m_audio->Reset();
    m_physics->Reset();
    for (const auto& object : m_objects)
        DisconnectSpatialManipulators(object.get());
    for (const auto& object : m_objects)
        if (object)
            object->OwnerScene = nullptr;
    m_objects.clear();
    m_selectedObject = nullptr;
    m_previewObject = nullptr;
}

}
