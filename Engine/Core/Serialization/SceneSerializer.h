#pragma once
#include <string>
#include <unordered_map>
#include <functional>

class Scene;
class Object;
class Component;
class IGraphicsProvider;

// ---------------------------------------------------------------------------
// SceneSerializer — saves and loads Scene objects to/from scene XML files.
//
// Scene XML files use an explicit element tree with the following root structure:
//   <Scene>
//     <version>1</version>
//     <settings>...</settings>
//     <objects>
//       <Object>
//         <components>
//           <Component>
//             <type>Mesh</type>
//             ...
//           </Component>
//         </components>
//       </Object>
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
// Built-in component types (Mesh, Material, Camera) are registered
// automatically.  User script types must be registered before the first
// Load() call, typically at application startup:
//
//   SceneSerializer::Register("Rotate", []() -> Component* { return new Rotate(); });
//   scene.Load("Assets/Scenes/level1.scene");
// ---------------------------------------------------------------------------
class SceneSerializer
{
public:
    // Factory function: creates a default-constructed Component of the named type.
    using Factory = std::function<Component*()>;

    // Register a component type for deserialization by name.
    static void Register(const std::string& typeName, Factory factory);

    // Write the scene to a scene XML file.
    // Returns false if the destination file cannot be opened.
    static bool Save(const Scene& scene, const std::string& path);

    // Capture/restore a scene entirely in memory. Used by the editor to keep
    // play-mode mutations separate from the editable scene state.
    static std::string SaveToString(const Scene& scene);
    static bool LoadFromString(Scene& scene, const std::string& source,
        IGraphicsProvider* graphicsProvider = nullptr);

    // Read a scene XML file into scene, replacing all existing objects.
    // Pass a graphics provider so that Mesh GPU buffers are created after load.
    // Returns false if the file cannot be read or has an unsupported format.
    static bool Load(Scene& scene, const std::string& path, IGraphicsProvider* graphicsProvider = nullptr);

    static bool SavePrefab(const Object& object, const std::string& path);
    static Object* InstantiatePrefab(Scene& scene, const std::string& path,
        IGraphicsProvider* graphicsProvider = nullptr);
    // Reload every scene instance that references path while preserving each
    // root placement transform. Returns false if the prefab cannot be read.
    static bool RefreshPrefabInstances(Scene& scene, const std::string& path,
        IGraphicsProvider* graphicsProvider = nullptr);

    // Called internally; exposed so Scene::Load can invoke it.
    static void EnsureBuiltinsRegistered();

private:
    static std::unordered_map<std::string, Factory>& GetRegistry();
};

// Helper for registering component types with the scene serializer registry.
template<typename T>
inline void RegisterComponentType(const std::string& typeName)
{
    SceneSerializer::Register(typeName, []() -> Component* { return new T(); });
}
