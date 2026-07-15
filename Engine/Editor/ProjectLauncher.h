#pragma once

#include <string>

class ProjectLauncher
{
public:
    // Returns an absolute .proj path, or an empty string when the launcher is closed.
    static std::string Run(HINSTANCE instance);
    static void RememberProject(const std::string& projectFile);
};
