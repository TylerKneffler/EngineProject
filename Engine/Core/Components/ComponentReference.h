#pragma once

#include <string>

// Serializable, editor-assignable reference to a component in the same scene.
// An empty componentType means "use the owning component's conventional default".
namespace Engine::Core
{
class Component;
class Object;

struct ComponentReference
{
    std::string expectedType;
    std::string objectPath;
    std::string objectName;
    std::string componentType;
    int componentIndex = 0;

    ComponentReference() = default;
    explicit ComponentReference(const char* expected)
        : expectedType(expected ? expected : "") {}

    bool IsAssigned() const { return !componentType.empty(); }
    void Clear()
    {
        objectPath.clear();
        objectName.clear();
        componentType.clear();
        componentIndex = 0;
    }
};

ComponentReference CaptureComponentReference(const Component* component,
    const std::string& expectedType = {});
Component* ResolveComponentReferenceRaw(Object* context,
    const ComponentReference& reference);

template<typename T>
T* ResolveComponentReference(Object* context, const ComponentReference& reference)
{
    return dynamic_cast<T*>(ResolveComponentReferenceRaw(context, reference));
}
}
