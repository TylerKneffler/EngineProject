#pragma once

#include "Core/Model/ImportedModel.h"
#include "PmxModelDecoder.h"
#include <string>

namespace Engine::Editor
{
class ModelImporter
{
public:
    static bool SupportsExtension(const std::string& extension);
    static Engine::Model::ModelImportResult Import(const std::string& sourcePath,
        const std::string& assetsDirectory, float pmxUnitScaleToMeters = 1.f,
        PmxImportOptions pmxOptions = {});
};
}
