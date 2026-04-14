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
    std::string renderingAPI;
    glm::vec4 clearColor;
    uint32_t targetFramerate;

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
    void ParseDependencies(const pugi::xml_node& projectNode, ProjectSettings& settings);
    void ParseComponents(const pugi::xml_node& projectNode, ProjectSettings& settings);
};
