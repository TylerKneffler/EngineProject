#pragma once
#include <string>
#include <unordered_map>
#include <functional>

class Scene;
class Component;
struct ID3D12Device;

// ---------------------------------------------------------------------------
// SceneSerializer — saves and loads Scene objects to/from .scene files.
//
// .scene files are UTF-8 JSON with the following root structure:
//   {
//     "version":  1,
//     "settings": { ... SceneSettings fields ... },
//     "objects":  [ { "name", "transform", "components", "children" }, ... ]
//   }
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
//   scene.Load("Assets/Scenes/default.scene");
// ---------------------------------------------------------------------------
class SceneSerializer
{
public:
    // Factory function: creates a default-constructed Component of the named type.
    using Factory = std::function<Component*()>;

    // Register a component type for deserialization by name.
    static void Register(const std::string& typeName, Factory factory);

    // Write the scene to a .scene JSON file.
    // Returns false if the destination file cannot be opened.
    static bool Save(const Scene& scene, const std::string& path);

    // Read a .scene JSON file into scene, replacing all existing objects.
    // Pass a D3D12 device so that Mesh GPU buffers are created after load.
    // Returns false if the file cannot be read or has an unsupported format.
    static bool Load(Scene& scene, const std::string& path, ID3D12Device* device = nullptr);

    // Called internally; exposed so Scene::Load can invoke it.
    static void EnsureBuiltinsRegistered();

private:
    static std::unordered_map<std::string, Factory>& GetRegistry();
};
