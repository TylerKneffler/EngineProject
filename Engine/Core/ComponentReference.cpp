#include "Core/ComponentReference.h"

#include "Core/Component.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include <sstream>

namespace
{
std::string EncodePath(const Scene::ObjectPath& path)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < path.size(); ++index)
    {
        if (index) output << '.';
        output << path[index];
    }
    return output.str();
}

Scene::ObjectPath DecodePath(const std::string& encoded)
{
    Scene::ObjectPath path;
    std::istringstream input(encoded);
    std::string part;
    try
    {
        while (std::getline(input, part, '.'))
            if (!part.empty()) path.push_back(static_cast<std::size_t>(std::stoull(part)));
    }
    catch (...) { path.clear(); }
    return path;
}
}

ComponentReference CaptureComponentReference(const Component* component,
    const std::string& expectedType)
{
    ComponentReference result;
    result.expectedType = expectedType.empty() && component
        ? component->GetTypeName() : expectedType;
    if (!component || !component->Owner || !component->Owner->GetScene())
        return result;

    result.objectName = component->Owner->name;
    result.componentType = component->GetTypeName();
    Scene::ObjectPath path;
    if (component->Owner->GetScene()->TryGetObjectPath(component->Owner, path))
        result.objectPath = EncodePath(path);

    for (Component* candidate : component->Owner->Components)
    {
        if (candidate == component) break;
        if (candidate && candidate->GetTypeName() == result.componentType)
            ++result.componentIndex;
    }
    return result;
}

Component* ResolveComponentReferenceRaw(Object* context,
    const ComponentReference& reference)
{
    if (!context || !reference.IsAssigned() || !context->GetScene()) return nullptr;
    Scene* scene = context->GetScene();
    Object* target = scene->FindObjectByPath(DecodePath(reference.objectPath));
    if (!target || (!reference.objectName.empty() && target->name != reference.objectName))
    {
        Object* pathTarget = target;
        target = nullptr;
        for (const auto& candidate : scene->GetObjects())
            if (candidate && candidate->name == reference.objectName)
            {
                target = candidate.get();
                break;
            }
        // Object renames do not invalidate a still-correct hierarchy path.
        if (!target) target = pathTarget;
    }
    if (!target) return nullptr;

    int index = 0;
    for (Component* component : target->Components)
        if (component && component->GetTypeName() == reference.componentType)
        {
            if (index++ == reference.componentIndex) return component;
        }
    return nullptr;
}
