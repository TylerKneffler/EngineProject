#pragma once

#include "Core/Model/ImportedModel.h"

using GltfImportResult = ModelImportResult;

// Converts glTF/GLB source data into an engine prefab plus engine-native
// mesh/material assets under Assets/ModelName/{Meshes,Materials,Textures}.
class GltfImporter
{
public:
    static GltfImportResult Import(
        const std::string& sourcePath,
        const std::string& assetsDirectory);
};
