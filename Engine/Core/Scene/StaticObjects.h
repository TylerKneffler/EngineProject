#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Engine::Core { class Object; }
namespace Engine::Scene { class Scene; }

// Static objects (Object::isStatic): the author's promise that an object does
// not move while the game runs.
//
// What the flag does today:
//  * Baking. When play starts, static prefab instances that are plain
//    geometry are taken out of the scene and drawn as instanced placements
//    (a runtime-only TerrainObjectLayer, the path forests use), so they cost
//    no per-object update, transform or draw preparation. The editor is not
//    affected: it only bakes inside play mode, whose scene is restored on stop.
//  * Checking. Anything that contradicts the promise is reported: at play
//    start for what can be seen in the scene, and while playing if a static
//    object's transform changes anyway.
//
// A static object is baked when all of this holds:
//  * it is the root of a prefab instance with no overrides besides its
//    transform, at least two static instances of that prefab qualify, and it
//    is enabled;
//  * the prefab holds only Mesh, Material, PrimitiveObjectCollider and fixed
//    ("Static") RigidBody components, and no blended (transparent) material;
//  * its world transform is a rotation, a uniform positive scale and a
//    translation;
//  * no component elsewhere in the scene refers to it by name.
// Static objects that are not geometry (lights, cameras, terrain, emitters)
// simply stay scene objects; that is not a problem and is not reported.
namespace Engine::Scene::StaticObjects
{
enum class Severity
{
    // The object will be (or was) baked, or is static without being geometry.
    Info,
    // The flag contradicts the object, or a prefab instance that looks
    // bakeable is kept as a scene object for a reason the author can fix.
    Warning
};

struct Note
{
    Severity severity = Severity::Info;
    std::string text;
};

// What the static flag means for one object right now: contradictions,
// whether it would be baked, and why not. Empty for objects that are not
// static in their hierarchy. Cheap enough for the inspector to call per
// frame for the selection (prefab override checks are cached on the object).
std::vector<Note> Describe(const Engine::Core::Object& object);

struct BakeResult
{
    size_t bakedObjects = 0;      // prefab instances turned into placements
    size_t bakedPrefabs = 0;      // distinct prefabs among them
    size_t staticObjects = 0;     // objects marked static (before baking)
    std::vector<std::string> warnings;
};

// Called by Game::Start before objects start. Reports contradictions through
// the warning sink and replaces bakeable static prefab instances with
// placements on one runtime-only object. ENGINE_STATIC_BAKE=0 keeps every
// object (warnings are still reported).
BakeResult BakeForPlay(Engine::Scene::Scene& scene);

// Catches static objects that move after all: remembers the world transform
// revision of every remaining static object at play start and re-checks a few
// each frame, warning once per object.
class MotionWatch
{
public:
    void Begin(Engine::Scene::Scene& scene);
    void Check(Engine::Scene::Scene& scene, size_t objectsPerCall = 32);

private:
    struct Entry { uint64_t id = 0; uint64_t revision = 0; bool reported = false; };
    std::vector<Entry> m_entries;
    size_t m_next = 0;
};

// Where warnings go. The default writes to stderr and the debugger output;
// the editor routes them to its Console. Pass an empty function to restore
// the default.
void SetWarningSink(std::function<void(const std::string&)> sink);
void Warn(const std::string& message);
}
