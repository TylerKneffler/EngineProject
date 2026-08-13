#pragma once

#include "Core/Model/ImportedModel.h"
#include <string>

class ModelImporter
{
public:
    static bool SupportsExtension(const std::string& extension);
    static ModelImportResult Import(const std::string& sourcePath,
        const std::string& assetsDirectory);
};
