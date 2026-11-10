#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>
#include <cstddef>

namespace Engine::Core { class Component; class Object; }
namespace Engine::Scene { class Scene; }
namespace Engine::Graphics { class IGraphicsProvider; }
namespace Engine::Serialization { class JsonValue; }

namespace Engine::Serialization
{

// ---------------------------------------------------------------------------
// SceneSerializer — saves and loads Scene objects to/from scene XML files.
//
// Scene XML files use a generic element tree. Object arrays use <item>;
// numeric arrays use a single text node with encoded values; objects use their
// serialized field names; objects with a serialized "type" use that type as a
// namespace-qualified element without serializer-side type lists:
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
    using Component = Engine::Core::Component;
    using Object = Engine::Core::Object;
    using Scene = Engine::Scene::Scene;
    using IGraphicsProvider = Engine::Graphics::IGraphicsProvider;
    struct IncrementalLoadState
    {
        struct PendingNode
        {
            const JsonValue* node = nullptr;
            Object* parent = nullptr;
        };
        std::size_t nextTopLevelObject = 0;
        std::size_t completedTopLevelObjects = 0;
        bool topLevelObjectInProgress = false;
        std::vector<PendingNode> pendingNodes;
    };

    // Factory function: creates a default-constructed Component of the named type.
    using Factory = std::function<Component*()>;

    // Register a component type for deserialization by name.
    static void Register(const std::string& typeName, Factory factory);
    // Register a component factory using the type name reported by an instance.
    static void Register(Factory factory);

    // Create a default instance of a registered component type. The editor
    // uses this for component-script assets dropped onto an object.
    static Component* CreateRegisteredComponent(const std::string& typeName);
    // Sorted editor-addable names cached by the registration lifecycle.
    static const std::vector<std::string>& GetRegisteredComponentTypes();
    static std::vector<std::string> GetRegisteredScriptTypes();
    static void Unregister(const std::string& typeName);
    static Factory GetRegisteredFactory(const std::string& typeName);

    // Write the scene to a scene XML file.
    // Returns false if the destination file cannot be opened.
    static bool Save(const Scene& scene, const std::string& path);

    // Capture/restore a scene entirely in memory. Used by the editor to keep
    // play-mode mutations separate from the editable scene state.
    static std::string SaveToString(const Scene& scene);
    // Compact, lossless editor history/play snapshot. LoadFromString accepts
    // these bytes as well as the public XML/JSON scene formats.
    static std::string SaveSnapshotToString(const Scene& scene);
    // Capture the live graph on its owner; these value-only operations may run
    // on an IO worker after the owner releases the snapshot.
    static JsonValue CaptureSceneDocument(const Scene& scene);
    static std::string EncodeSceneSnapshot(const JsonValue& document);
    static std::string EncodeSceneFile(const JsonValue& document);
    static JsonValue ParseSceneText(const std::string& source);
    static JsonValue ReadSceneFile(const std::string& path);
    static bool LoadFromString(Scene& scene, const std::string& source,
        IGraphicsProvider* graphicsProvider = nullptr);
    // Restore a previously parsed snapshot without decoding its binary form.
    // The caller owns rollback if a component or prefab fails during restore.
    static bool LoadFromValue(Scene& scene, const JsonValue& snapshot,
        IGraphicsProvider* graphicsProvider = nullptr);
    // Editor-only incremental restore. Begin replaces scene contents and
    // applies settings; each step restores at most maxTopLevelObjects trees.
    // The caller must restore its own snapshot if a step returns false.
    static bool BeginIncrementalLoadFromValue(Scene& scene,
        const JsonValue& snapshot);
    static bool ContinueIncrementalLoadFromValue(Scene& scene,
        const JsonValue& snapshot, IncrementalLoadState& state,
        std::size_t maxObjectNodes, bool& complete);

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
    // Capture on the scene owner, then invoke the returned value-only job on
    // an IO worker. It owns the prefab and rig data needed by the save.
    static std::function<bool()> PreparePrefabSave(const Object& object,
        const std::string& path, bool preserveRootTransform = false);
    static std::string SavePrefabToString(const Object& object,
        bool includeRootTransform = true);
    static Object* InstantiatePrefab(Scene& scene, const std::string& path,
        IGraphicsProvider* graphicsProvider = nullptr);
    static JsonValue ReadPrefabFile(const std::string& path);
    // Release the owner-thread cache entry for a large prefab document.
    // The editor calls this at scene-session boundaries to cap retained memory.
    static void ClearLargePrefabDocumentCache();
    static Object* InstantiatePrefabFromValue(Scene& scene, const std::string& path,
        const JsonValue& document, IGraphicsProvider* graphicsProvider = nullptr);
    // Reload every scene instance that references path while preserving each
    // root placement transform. preservedInstance can identify the instance
    // already edited in-place; it will not be rebuilt. Returns false if the
    // prefab cannot be read.
    static bool RefreshPrefabInstances(Scene& scene, const std::string& path,
        IGraphicsProvider* graphicsProvider = nullptr,
        Object* preservedInstance = nullptr);
    static bool HasPrefabOverrides(const Object& instance,
        bool includeRootTransform = true);
    static bool RevertPrefabOverrides(Object& instance,
        IGraphicsProvider* graphicsProvider = nullptr);
    static bool ApplyPrefabOverridesToAsset(Object& instance,
        bool includeRootTransform,
        IGraphicsProvider* graphicsProvider = nullptr);

    // Called internally; exposed so Scene::Load can invoke it.
    static void EnsureBuiltinsRegistered();

private:
    static std::unordered_map<std::string, Factory>& GetRegistry();
};

// Helper for registering component types with the scene serializer registry.
template<typename T>
inline void RegisterComponentType()
{
    SceneSerializer::Register([]() -> Engine::Core::Component* { return new T(); });
}

// Backward-compatible overload for components that need a serialized alias.
template<typename T>
inline void RegisterComponentType(const std::string& typeName)
{
    SceneSerializer::Register(typeName, []() -> Engine::Core::Component* { return new T(); });
}
}
