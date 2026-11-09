#pragma once

#include <deque>
#include <string>

namespace Engine::Components { class Mesh; }

namespace Engine::Editor
{
std::string CaptureMeshSnapshot(const Engine::Components::Mesh& mesh);
bool RestoreMeshSnapshot(Engine::Components::Mesh& mesh,
    const std::string& snapshot);
// Applies one editor mesh or weight-paint undo/redo entry only after the
// authored mesh accepts the snapshot.
bool StepMeshSnapshotHistory(Engine::Components::Mesh& mesh, bool redo,
    std::deque<std::string>& undo, std::deque<std::string>& redoEntries,
    std::string& baseline);
}
