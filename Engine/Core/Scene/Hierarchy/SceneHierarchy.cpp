#include "Core/Scene/Scene.h"
#include "Core/Audio/Audio.h"
#include "Core/Physics/Physics.h"
#include "Core/Renderers/UIRenderer.h"
#include <algorithm>
#include <cmath>
#include <functional>

Scene::Scene()
    : m_physics(std::make_unique<Physics>(*this))
    , m_audio(std::make_unique<Audio>(*this))
{
}

Scene::~Scene()
{
    m_audio->Reset();
    m_physics->Reset();
}

void Scene::Start()
{
    for (const auto& object : m_objects)
        object->Start();
}

void Scene::Update(float deltaTime)
{
    for (const auto& object : m_objects)
        object->Update();
    m_audio->Update(deltaTime);
    m_physics->Step(deltaTime);
}

Object* Scene::AddObject()
{
    auto obj = std::make_unique<Object>();
    Object* raw = obj.get();
    raw->OwnerScene = this;
    m_objects.push_back(std::move(obj));
    return raw;
}

Object* Scene::AddObject(const std::string& name)
{
    Object* obj = AddObject();
    obj->name = name;
    obj->OwnerScene = this;
    return obj;
}

bool Scene::TryGetObjectPath(const Object* object, ObjectPath& path) const
{
    path.clear();
    if (!object)
        return false;

    for (const Object* current = object; current; current = current->Parent)
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

Object* Scene::FindObjectByPath(const ObjectPath& path) const
{
    if (path.empty())
        return nullptr;

    Object* current = nullptr;
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
    Object* object,
    Object* target,
    ObjectPlacement placement)
{
    if (!object || object == target)
        return false;

    Object* newParent = placement == ObjectPlacement::AsChild
        ? target
        : (target ? target->Parent : nullptr);
    for (Object* ancestor = newParent; ancestor; ancestor = ancestor->Parent)
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

        std::unique_ptr<Object> owned = std::move(*moving);
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

void Scene::RemoveObject(Object* obj)
{
    if (!obj)
        return;

    std::vector<Object*> objectsToRemove;
    std::function<void(Object*)> collect = [&](Object* current)
    {
        if (!current)
            return;
        objectsToRemove.push_back(current);
        for (Object* child : current->Children)
            collect(child);
    };
    collect(obj);

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
            [&](const std::unique_ptr<Object>& candidate)
            {
                return std::find(
                    objectsToRemove.begin(),
                    objectsToRemove.end(),
                    candidate.get()) != objectsToRemove.end();
            }),
        m_objects.end());
}

void Scene::ClearObjects()
{
    m_audio->Reset();
    m_physics->Reset();
    m_objects.clear();
    m_selectedObject = nullptr;
    m_previewObject = nullptr;
}
