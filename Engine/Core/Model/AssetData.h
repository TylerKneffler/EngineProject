#pragma once

#include <map>
#include <string>
#include <variant>

using AssetImportSetting = std::variant<bool, double, std::string>;
using AssetImportSettings = std::map<std::string, AssetImportSetting>;
