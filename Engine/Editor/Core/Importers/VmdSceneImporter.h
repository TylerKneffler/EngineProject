#pragma once

#include "Core/Model/ImportedModel.h"
#include <string>

namespace Engine::Editor
{
class VmdSceneImporter
{
public:
    static Engine::Model::ModelImportResult Import(const std::string& sourcePath,
        const std::string& assetsDirectory, bool createScenePrefab = false);
};
}
