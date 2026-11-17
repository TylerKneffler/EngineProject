#pragma once

#include <cstdint>

namespace Engine::Core { class Object; }
namespace Engine::Components { class Mesh; }

namespace Engine::Rendering
{
// Optional per-view render input supplied by whoever hosts a scene view (the
// editor). The scene renderer reads it to outline a selection, show a pose or
// weight-paint overlay, or isolate a preview object. Hosts that supply none
// (game, server, exporter) render with these defaults.
struct SceneViewOverlays
{
    using Object = Engine::Core::Object;
    using Mesh = Engine::Components::Mesh;

    Object* selectedObject = nullptr;
    // A single instance of a placement layer, outlined like an object.
    uint64_t selectedPlacementId = 0;
    Object* selectedPlacementLayer = nullptr;
    Mesh* selectedMesh = nullptr;
    // Show the selected mesh in its editable bind/edit pose.
    bool meshEditPose = false;
    Mesh* weightPaintMesh = nullptr;
    int weightPaintBone = -1;
    // Rendered on its own (asset/prefab preview).
    Object* previewObject = nullptr;
};
}
