#include "Game/Startup/GameProjectStartup.h"

#include <filesystem>

#ifndef PROJECT_FILE
#define PROJECT_FILE ""
#endif
#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

namespace Engine::Game
{
std::string GetFallbackScenePath()
{
    return std::string(ENGINE_ASSETS_PATH) + "Scenes/default.scene";
}

Engine::Model::ProjectSettings LoadGameProjectSettings()
{
    std::string projectFile = PROJECT_FILE;
    std::vector<std::string> localProjects;
    for (const auto& entry : std::filesystem::directory_iterator(
        std::filesystem::current_path()))
        if (entry.is_regular_file() && entry.path().extension() == ".proj")
            localProjects.push_back(entry.path().string());
    if (localProjects.size() == 1)
        projectFile = localProjects.front();

    try
    {
        return Engine::Core::ProjectLoader{}.LoadProject(projectFile);
    }
    catch (const std::exception&)
    {
        Engine::Model::ProjectSettings settings;
        settings.assetsDirectory = ENGINE_ASSETS_PATH;
        settings.defaultScene = GetFallbackScenePath();
        settings.viewportWidth = 1280;
        settings.viewportHeight = 720;
        settings.renderingAPI = "DirectX11";
        settings.editorRenderingAPI = "DirectX11";
        settings.gameRenderingAPI = "DirectX11";
        settings.clearColor = { 0.1f, 0.1f, 0.1f, 1.f };
        return settings;
    }
}
}
