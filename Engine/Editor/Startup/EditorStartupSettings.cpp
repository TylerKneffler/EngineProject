#include "Engine/Editor/Startup/EditorStartupSettings.h"

#include <filesystem>

#ifndef ENGINE_ROOT_PATH
#define ENGINE_ROOT_PATH "."
#endif
#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif
#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif
#ifndef ENGINE_BUILD_DIR
#define ENGINE_BUILD_DIR "build/Debug"
#endif

namespace Engine::Editor
{
    bool IsEngineDevelopmentDirectory()
    {
        std::error_code error;
        const auto current = std::filesystem::weakly_canonical(
            std::filesystem::current_path(), error);
        if (error) return false;
        const auto engineRoot = std::filesystem::weakly_canonical(
            std::filesystem::path(ENGINE_ROOT_PATH), error);
        return !error && current == engineRoot &&
            std::filesystem::is_directory(engineRoot / "Engine" / "Core" / "Assets") &&
            std::filesystem::is_regular_file(engineRoot / "CMakeLists.txt");
    }

    Model::ProjectSettings CreateEngineDevelopmentSettings()
    {
        Model::ProjectSettings settings{};
        settings.name = "Engine Sandbox";
        settings.version = "Development";
        settings.description = "Built-in project-free engine development environment";
        settings.engineDirectory = std::filesystem::path(ENGINE_ROOT_PATH).string();
        settings.assetsDirectory = std::filesystem::path(ENGINE_ASSETS_PATH).string();
        settings.sceneDirectory =
            (std::filesystem::path(ENGINE_ASSETS_PATH) / "Scenes").string();
        settings.scriptsDirectory =
            (std::filesystem::path(ENGINE_ASSETS_PATH) / "Scripts").string();
        settings.shadersDirectory = std::filesystem::path(ENGINE_SHADERS_PATH).string();
        settings.buildDirectory = std::filesystem::path(ENGINE_BUILD_DIR).string();
        settings.defaultScene =
            (std::filesystem::path(ENGINE_ASSETS_PATH) / "Scenes" / "Basics" / "default.scene").string();
        settings.viewportWidth = 1280;
        settings.viewportHeight = 720;
        settings.leftPanelWidth = 0.20f;
        settings.rightPanelWidth = 0.31f;
        settings.leftPanelTabs = { "HierarchyView" };
        settings.centerPanelTabs = { "Scene", "Game" };
        settings.rightPanelTabs = { "Properties" };
        settings.renderingAPI = "DirectX11";
        settings.editorRenderingAPI = "DirectX11";
        settings.gameRenderingAPI = "DirectX11";
        settings.clearColor = { 0.18f, 0.18f, 0.18f, 1.f };
        settings.targetFramerate = 60;
        settings.aspectRatioMode = Model::ProjectSettings::AspectRatioMode::Free;
        return settings;
    }
}
