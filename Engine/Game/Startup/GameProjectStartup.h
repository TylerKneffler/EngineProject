#pragma once

#include "Core/ProjectLoader.h"
#include <string>

namespace Engine::Game
{
ProjectSettings LoadGameProjectSettings();
std::string GetFallbackScenePath();
}
