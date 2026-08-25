#pragma once

#include "Core/Model/ImportedModel.h"
#include <string>

namespace Engine::Editor
{
class ModelAssetWriter
{
public:
    static Engine::Model::ModelImportResult Write(const Engine::Model::ImportedModel& model,
        const std::string& sourcePath, const std::string& assetsDirectory);
};
}
