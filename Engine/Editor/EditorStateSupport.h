#pragma once

#include <string>

namespace Engine::Editor::StateSupport
{
    bool HistoryDiffers(const std::string& left, const std::string& right);
    void LogStartupFailure(const std::string& message);
    std::string NormalizeAssetPath(const std::string& path);
    std::string AssetPathIdentity(const std::string& path);
}
