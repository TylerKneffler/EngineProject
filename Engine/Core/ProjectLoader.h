#pragma once
#include "pch.h"
#include "Core/Model/ProjectSettings.h"
#include <pugixml.hpp>

// ---------------------------------------------------------------------------
// ProjectLoader
//
// Loads and parses project configuration from .proj XML files.
// Provides settings to configure the engine and editor at startup.
//
// Usage:
//   ProjectLoader loader;
//   ProjectSettings settings = loader.LoadProject("MyGame.proj");
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
