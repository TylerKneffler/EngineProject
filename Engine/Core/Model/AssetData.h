#pragma once

#include <map>
#include <string>
#include <variant>

namespace Engine::Model
{
using AssetImportSetting = std::variant<bool, double, std::string>;
using AssetImportSettings = std::map<std::string, AssetImportSetting>;
}

