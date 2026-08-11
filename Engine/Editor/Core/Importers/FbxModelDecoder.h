#pragma once

#include "ImportedModel.h"
#include <string>

class FbxModelDecoder
{
public:
    static bool Decode(const std::string& sourcePath, ImportedModel& model,
        std::string& error);
};
