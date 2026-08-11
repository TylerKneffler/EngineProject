#pragma once

#include "ImportedModel.h"
#include <string>

class ModelAssetWriter
{
public:
    static ModelImportResult Write(const ImportedModel& model,
        const std::string& sourcePath, const std::string& assetsDirectory);
};
