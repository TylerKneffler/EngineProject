#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>

class Scene;
class Object;
class Component;
class IGraphicsProvider;

// ---------------------------------------------------------------------------
// SceneSerializer — saves and loads Scene objects to/from scene XML files.
//
// Scene XML files use a generic element tree. Arrays use <item>; objects use
// their serialized field names; objects with a serialized "type" use that type
// as a namespace-qualified element without serializer-side type lists:
//   <Scene xmlns:components="urn:engine-components">
//     <version>1</version>
//     <settings>...</settings>
//     <objects>
//       <item>
//         <components>
//           <components:Material>
//             <Material.diffuse>...</Material.diffuse>
//           </components:Material>
//         </components>
//       </item>
//     </objects>
//   </Scene>
//
// Prefab instances are stored as compact references:
//   { "prefab": "Assets/Model/Model.prefab", "transform": { ... } }
// Names, components, children, meshes, materials, and other prefab-owned
// properties are rebuilt from that file. The root transform remains
// scene-owned placement data.
//
// Only top-level objects (those with no parent) appear in "objects"; their
// children are nested inside "children" arrays recursively.  On load, ALL
// objects (including children) are added to the scene's flat object list so
// they participate in Update/Render loops, while the parent-child hierarchy
// is also reconstructed.
//
// Built-in component types are registered automatically. User component types
// must be registered before the first Load() call, typically at application
// startup. The serialized name is obtained from the component itself:
//
//   RegisterComponentType<Rotate>();
//   scene.Load("Assets/Scenes/level1.scene");
// ---------------------------------------------------------------------------
class SceneSerializer
{
public:
    // Factory function: creates a default-constructed Component of the named type.
    using Factory = std::function<Component*()>;

    // Register a component type for deserialization by name.
    static void Register(const std::string& typeName, Factory factory);
    // Register a component factory using the type name reported by an instance.
    static void Register(Factory factory);

    // Create a default instance of a registered component type. The editor
    // uses this for component-script assets dropped onto an object.
    static Component* CreateRegisteredComponent(const std::string& typeName);
    static std::vector<std::string> GetRegisteredComponentTypes();
    static std::vector<std::string> GetRegisteredScriptTypes();
    static void Unregister(const std::string& typeName);
    static Factory GetRegisteredFactory(const std::string& typeName);

    // Write the scene to a scene XML file.
    // Returns false if the destination file cannot be opened.
    static bool Save(const Scene& scene, const std::string& path);

    // Capture/restore a scene entirely in memory. Used by the editor to keep
    // play-mode mutations separate from the editable scene state.
    static std::string SaveToString(const Scene& scene);
    static bool LoadFromString(Scene& scene, const std::string& source,
        IGraphicsProvider* graphicsProvider = nullptr);

    // Capture and recreate one object plus its complete child hierarchy.
    // Used by editor copy/paste without touching the filesystem.
    static std::string SaveObjectToString(const Object& object);
    static Object* InstantiateObjectFromString(Scene& scene, const std::string& source,
        IGraphicsProvider* graphicsProvider = nullptr);

    // Read a scene XML file into scene, replacing all existing objects.
    // Pass a graphics provider so that Mesh GPU buffers are created after load.
    // Returns false if the file cannot be read or has an unsupported format.
    static bool Load(Scene& scene, const std::string& path, IGraphicsProvider* graphicsProvider = nullptr);

    static bool SavePrefab(const Object& object, const std::string& path,
        bool preserveRootTransform = false);
    static std::string SavePrefabToString(const Object& object,
        bool includeRootTransform = true);
    static Object* InstantiatePrefab(Scene& scene, const std::string& path,
        IGraphicsProvider* graphicsProvider = nullptr);
    // Reload every scene instance that references path while preserving each
    // root placement transform. preservedInstance can identify the instance
    // already edited in-place; it will not be rebuilt. Returns false if the
    // prefab cannot be read.
    static bool RefreshPrefabInstances(Scene& scene, const std::string& path,
        IGraphicsProvider* graphicsProvider = nullptr,
        Object* preservedInstance = nullptr);

    // Called internally; exposed so Scene::Load can invoke it.
    static void EnsureBuiltinsRegistered();

private:
    static std::unordered_map<std::string, Factory>& GetRegistry();
};

// Helper for registering component types with the scene serializer registry.
template<typename T>
inline void RegisterComponentType()
{
    SceneSerializer::Register([]() -> Component* { return new T(); });
}

// Backward-compatible overload for components that need a serialized alias.
template<typename T>
inline void RegisterComponentType(const std::string& typeName)
{
    SceneSerializer::Register(typeName, []() -> Component* { return new T(); });
}
