#pragma once
#include "pch.h"
#include "Core/Scene/Scene.h"
#include <filesystem>
#include <list>
#include <string>
#include <chrono>

class Project
{
    private:
    public:
    Project() = default;
    ~Project() = default;

    std::filesystem::path projectPath;
    // Project Settings (Loaded from project dir, settingsfile)
    std::string projectName;
    std::filesystem::path assetsPath;
    std::chrono::system_clock::time_point lastOpened;
    std::chrono::system_clock::time_point createdDate;

    /* 
     List of scenes that are used for the production build.
     First is the entry scene. Editor can have more scenes that are not included in the build, 
     but the build can only have scenes that are in the editor. 
    */
    std::list<Scene> buildScenes;

    //Skybox
    bool useSkyboxTexture;
    std::string skyboxColor;
    std::string skyboxTexturePath;
};