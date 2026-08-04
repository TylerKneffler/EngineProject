#pragma once

struct ProjectSettings;

namespace Engine::Game
{
// Resolves a saved renderer preference or asks the player using the native
// Windows TaskDialog. Returns false when startup was cancelled.
bool SelectStartupRenderer(ProjectSettings& settings);
}
