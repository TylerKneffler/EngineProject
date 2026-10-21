#pragma once

#include "Core/Model/ProjectSettings.h"

namespace Engine::Editor
{
    bool IsEngineDevelopmentDirectory();
    Model::ProjectSettings CreateEngineDevelopmentSettings();
}
