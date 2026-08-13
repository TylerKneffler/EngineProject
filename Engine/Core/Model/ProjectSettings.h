#pragma once

#include "Core/Model/LightingData.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

struct ProjectSettings
{
    enum class EditorMode { ThreeD, TwoD };
    std::string name;
    std::string version;
    std::string description;
    std::string engineDirectory;
    std::string assetsDirectory;
    std::string sceneDirectory;
    std::string scriptsDirectory;
    std::string shadersDirectory;
    std::string buildDirectory;
    std::string cmakeGenerator;
    std::string platform;
    std::string defaultScene;
    uint32_t viewportWidth;
    uint32_t viewportHeight;
    float leftPanelWidth;
    float rightPanelWidth;
    bool debugHierarchyInteractions = true;
    uint32_t editorHistoryLimit = 100;
    EditorMode editorMode = EditorMode::ThreeD;
    std::vector<std::string> leftPanelTabs;
    std::vector<std::string> centerPanelTabs;
    std::vector<std::string> rightPanelTabs;
    std::string renderingAPI;
    std::string editorRenderingAPI;
    std::string gameRenderingAPI;
    glm::vec4 clearColor;
    uint32_t targetFramerate;
    Engine::Rendering::Lighting::BakedLightingSettings bakedLighting;
    enum class AspectRatioMode { Free, Locked, Hardcoded };
    AspectRatioMode aspectRatioMode = AspectRatioMode::Locked;
    float gameAspectRatio = 1.777f;
    uint32_t gameWindowWidth = 1920;
    uint32_t gameWindowHeight = 1080;
    glm::vec4 letterboxColor = glm::vec4(0.f, 0.f, 0.f, 1.f);
    std::vector<std::string> builtInComponents;
};
