#pragma once

#include "Core/ProjectLoader.h"
#include <string>

namespace Engine::Game
{
Engine::Model::ProjectSettings LoadGameProjectSettings();
std::string GetFallbackScenePath();
}
