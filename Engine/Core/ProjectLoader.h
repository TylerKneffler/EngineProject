#pragma once
#include "pch.h"
#include <pugixml.hpp>

// ---------------------------------------------------------------------------
// ProjectSettings
//
// Contains all project configuration loaded from the .proj file.
// ---------------------------------------------------------------------------
struct ProjectSettings
{
    // Project metadata
    std::string name;
    std::string version;
    std::string description;

    // Paths
    std::string assetsDirectory;
    std::string sceneDirectory;
    std::string scriptsDirectory;
    std::string shadersDirectory;
    std::string buildDirectory;

    // Build
    std::string cmakeGenerator;
    std::string platform;

    // Editor
    std::string defaultScene;
    uint32_t viewportWidth;
    uint32_t viewportHeight;
    float leftPanelWidth;
    float rightPanelWidth;

    std::vector<std::string> leftPanelTabs;
    std::vector<std::string> centerPanelTabs;
    std::vector<std::string> rightPanelTabs;

    // Rendering
    std::string renderingAPI;              // Deprecated: use editorRenderingAPI and gameRenderingAPI
    std::string editorRenderingAPI;        // Rendering API for editor views (DirectX12, Vulkan)
    std::string gameRenderingAPI;          // Rendering API for game window (DirectX12, Vulkan)
    glm::vec4 clearColor;
    uint32_t targetFramerate;

    // Game Resolution / Aspect Ratio
    enum class AspectRatioMode { Free, Locked, Hardcoded };
    AspectRatioMode aspectRatioMode = AspectRatioMode::Locked;
    float gameAspectRatio = 1.777f;  // 16:9
    uint32_t gameWindowWidth = 1920;
    uint32_t gameWindowHeight = 1080;
    glm::vec4 letterboxColor = glm::vec4(0.f, 0.f, 0.f, 1.f);  // black

    // Components
    std::vector<std::string> builtInComponents;
};

// ---------------------------------------------------------------------------
// ProjectLoader
//
// Loads and parses project configuration from .proj XML files.
// Provides settings to configure the engine and editor at startup.
//
// Usage:
//   ProjectLoader loader;
//   ProjectSettings settings = loader.LoadProject("Example_Proj.proj");
//   renderer.SetClearColor(settings.clearColor);
//   assetsExplorer.Init(settings.assetsDirectory);
// ---------------------------------------------------------------------------
class ProjectLoader
{
public:
    ProjectLoader()  = default;
    ~ProjectLoader() = default;

    // Loads and parses the project configuration file
    // Throws std::runtime_error if file cannot be read or parsed
    ProjectSettings LoadProject(const std::string& projFilePath);

private:
    // Helper methods for parsing XML elements
    void ParseMetadata(const pugi::xml_node& projectNode, ProjectSettings& settings);
    void ParsePaths(const pugi::xml_node& projectNode, ProjectSettings& settings);
    void ParseBuild(const pugi::xml_node& projectNode, ProjectSettings& settings);
    void ParseEditor(const pugi::xml_node& projectNode, ProjectSettings& settings);
    void ParseRendering(const pugi::xml_node& projectNode, ProjectSettings& settings);
    void ParseAspectRatio(const pugi::xml_node& projectNode, ProjectSettings& settings);
    void ParseDependencies(const pugi::xml_node& projectNode, ProjectSettings& settings);
    void ParseComponents(const pugi::xml_node& projectNode, ProjectSettings& settings);
};
