#pragma once
#include "Core/Model/ProjectSettings.h"


namespace Engine::Game
{
// Resolves a saved renderer preference or asks the player using the native
// Windows TaskDialog. Returns false when startup was cancelled.
bool SelectStartupRenderer(Engine::Model::ProjectSettings& settings);
}
