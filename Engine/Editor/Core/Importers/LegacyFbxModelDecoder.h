#pragma once

#include "Core/Model/ImportedModel.h"
#include <string>

namespace Engine::Editor
{
class LegacyFbxModelDecoder
{
public:
    static bool Decode(const std::string& sourcePath,
        Engine::Model::ImportedModel& model, std::string& error);
};
}
