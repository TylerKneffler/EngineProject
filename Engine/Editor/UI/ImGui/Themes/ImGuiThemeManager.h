#pragma once

#include <string>
#include <vector>

namespace Engine::Editor
{
// Discovers and applies validated .imguitheme files from the editor's Themes directory.
class ImGuiThemeManager
{
public:
    static bool Refresh(std::string* error = nullptr);
    static const std::vector<std::string>& AvailableThemes();
    static bool Apply(const std::string& name, std::string* error = nullptr);
    static std::string ThemeDirectory();
};
}
