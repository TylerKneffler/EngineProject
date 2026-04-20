#include "ProjectLoader.h"
#include <pugixml.hpp>

// ---------------------------------------------------------------------------
// LoadProject
// ---------------------------------------------------------------------------
ProjectSettings ProjectLoader::LoadProject(const std::string& projFilePath)
{
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(projFilePath.c_str());

    if (!result)
    {
        throw std::runtime_error(
            std::string("Failed to load project file: ") + projFilePath +
            " Error: " + result.description()
        );
    }

    ProjectSettings settings;

    auto projectNode = doc.child("Project");
    if (!projectNode)
    {
        throw std::runtime_error("Project file missing <Project> root element");
    }

    ParseMetadata(projectNode, settings);
    ParsePaths(projectNode, settings);
    ParseBuild(projectNode, settings);
    ParseEditor(projectNode, settings);
    ParseRendering(projectNode, settings);
    ParseAspectRatio(projectNode, settings);
    ParseDependencies(projectNode, settings);
    ParseComponents(projectNode, settings);

    return settings;
}

// ---------------------------------------------------------------------------
// ParseMetadata
// ---------------------------------------------------------------------------
void ProjectLoader::ParseMetadata(const pugi::xml_node& projectNode, ProjectSettings& settings)
{
    for (auto prop : projectNode.children("PropertyGroup"))
    {
        auto nameNode = prop.child("ProjectName");
        if (nameNode)
            settings.name = nameNode.child_value();

        auto versionNode = prop.child("ProjectVersion");
        if (versionNode)
            settings.version = versionNode.child_value();

        auto descNode = prop.child("ProjectDescription");
        if (descNode)
            settings.description = descNode.child_value();

        if (!settings.name.empty())
            break;
    }
}

// ---------------------------------------------------------------------------
// ParsePaths
// ---------------------------------------------------------------------------
void ProjectLoader::ParsePaths(const pugi::xml_node& projectNode, ProjectSettings& settings)
{
    for (auto prop : projectNode.children("PropertyGroup"))
    {
        auto assetsDir = prop.child("AssetsDirectory");
        if (assetsDir)
            settings.assetsDirectory = assetsDir.child_value();

        auto sceneDir = prop.child("SceneDirectory");
        if (sceneDir)
            settings.sceneDirectory = sceneDir.child_value();

        auto scriptsDir = prop.child("ScriptsDirectory");
        if (scriptsDir)
            settings.scriptsDirectory = scriptsDir.child_value();

        auto shadersDir = prop.child("ShadersDirectory");
        if (shadersDir)
            settings.shadersDirectory = shadersDir.child_value();

        auto buildDir = prop.child("BuildDirectory");
        if (buildDir)
            settings.buildDirectory = buildDir.child_value();

        if (!settings.assetsDirectory.empty())
            break;
    }
}

// ---------------------------------------------------------------------------
// ParseBuild
// ---------------------------------------------------------------------------
void ProjectLoader::ParseBuild(const pugi::xml_node& projectNode, ProjectSettings& settings)
{
    for (auto prop : projectNode.children("PropertyGroup"))
    {
        auto gen = prop.child("CMakeGenerator");
        if (gen)
            settings.cmakeGenerator = gen.child_value();

        auto plat = prop.child("Platform");
        if (plat)
            settings.platform = plat.child_value();

        if (!settings.cmakeGenerator.empty())
            break;
    }
}

// ---------------------------------------------------------------------------
// ParseEditor
// ---------------------------------------------------------------------------
void ProjectLoader::ParseEditor(const pugi::xml_node& projectNode, ProjectSettings& settings)
{
    for (auto prop : projectNode.children("PropertyGroup"))
    {
        auto defaultScene = prop.child("DefaultScene");
        if (defaultScene)
            settings.defaultScene = defaultScene.child_value();

        auto width = prop.child("ViewportWidth");
        if (width)
            settings.viewportWidth = std::stoul(width.child_value());

        auto height = prop.child("ViewportHeight");
        if (height)
            settings.viewportHeight = std::stoul(height.child_value());

        auto leftWidth = prop.child("LeftPanelWidth");
        if (leftWidth)
            settings.leftPanelWidth = std::stof(leftWidth.child_value());

        auto rightWidth = prop.child("RightPanelWidth");
        if (rightWidth)
            settings.rightPanelWidth = std::stof(rightWidth.child_value());

        if (!settings.defaultScene.empty())
            break;
    }

    // Parse panel tabs
    for (auto tab : projectNode.children("LeftPanelTab"))
    {
        const char* name = tab.attribute("Include").value();
        if (name && *name)
            settings.leftPanelTabs.push_back(name);
    }

    for (auto tab : projectNode.children("CenterPanelTab"))
    {
        const char* name = tab.attribute("Include").value();
        if (name && *name)
            settings.centerPanelTabs.push_back(name);
    }

    for (auto tab : projectNode.children("RightPanel"))
    {
        const char* name = tab.attribute("Include").value();
        if (name && *name)
            settings.rightPanelTabs.push_back(name);
    }
}

// ---------------------------------------------------------------------------
// ParseRendering
// ---------------------------------------------------------------------------
void ProjectLoader::ParseRendering(const pugi::xml_node& projectNode, ProjectSettings& settings)
{
    for (auto prop : projectNode.children("PropertyGroup"))
    {
        auto api = prop.child("RenderingAPI");
        if (api)
            settings.renderingAPI = api.child_value();

        auto editorApi = prop.child("EditorRenderingAPI");
        if (editorApi)
            settings.editorRenderingAPI = editorApi.child_value();

        auto gameApi = prop.child("GameRenderingAPI");
        if (gameApi)
            settings.gameRenderingAPI = gameApi.child_value();

        auto r = prop.child("ClearColorR");
        if (r)
            settings.clearColor.r = std::stof(r.child_value());

        auto g = prop.child("ClearColorG");
        if (g)
            settings.clearColor.g = std::stof(g.child_value());

        auto b = prop.child("ClearColorB");
        if (b)
            settings.clearColor.b = std::stof(b.child_value());

        auto a = prop.child("ClearColorA");
        if (a)
            settings.clearColor.a = std::stof(a.child_value());

        auto framerate = prop.child("TargetFramerate");
        if (framerate)
            settings.targetFramerate = std::stoul(framerate.child_value());

        if (!settings.renderingAPI.empty() || !settings.editorRenderingAPI.empty())
            break;
    }
    
    // Fallback to legacy renderingAPI field if separate APIs not specified
    if (settings.editorRenderingAPI.empty() && !settings.renderingAPI.empty())
        settings.editorRenderingAPI = settings.renderingAPI;
    if (settings.gameRenderingAPI.empty() && !settings.renderingAPI.empty())
        settings.gameRenderingAPI = settings.renderingAPI;
}

// ---------------------------------------------------------------------------
// ParseAspectRatio
// ---------------------------------------------------------------------------
void ProjectLoader::ParseAspectRatio(const pugi::xml_node& projectNode, ProjectSettings& settings)
{
    for (auto prop : projectNode.children("PropertyGroup"))
    {
        auto modeNode = prop.child("AspectRatioMode");
        if (modeNode)
        {
            std::string mode = modeNode.child_value();
            if (mode == "Free")
                settings.aspectRatioMode = ProjectSettings::AspectRatioMode::Free;
            else if (mode == "Locked")
                settings.aspectRatioMode = ProjectSettings::AspectRatioMode::Locked;
            else if (mode == "Hardcoded")
                settings.aspectRatioMode = ProjectSettings::AspectRatioMode::Hardcoded;
        }

        auto aspectNode = prop.child("GameAspectRatio");
        if (aspectNode)
            settings.gameAspectRatio = std::stof(aspectNode.child_value());

        auto widthNode = prop.child("GameWindowWidth");
        if (widthNode)
            settings.gameWindowWidth = std::stoul(widthNode.child_value());

        auto heightNode = prop.child("GameWindowHeight");
        if (heightNode)
            settings.gameWindowHeight = std::stoul(heightNode.child_value());

        auto lbR = prop.child("LetterboxColorR");
        if (lbR)
            settings.letterboxColor.r = std::stof(lbR.child_value());

        auto lbG = prop.child("LetterboxColorG");
        if (lbG)
            settings.letterboxColor.g = std::stof(lbG.child_value());

        auto lbB = prop.child("LetterboxColorB");
        if (lbB)
            settings.letterboxColor.b = std::stof(lbB.child_value());

        auto lbA = prop.child("LetterboxColorA");
        if (lbA)
            settings.letterboxColor.a = std::stof(lbA.child_value());

        if (modeNode)
            break;
    }
}

// ---------------------------------------------------------------------------
// ParseDependencies
// ---------------------------------------------------------------------------
void ProjectLoader::ParseDependencies(const pugi::xml_node& projectNode, ProjectSettings& settings)
{
    // Placeholder for dependency parsing
    // Currently not used in core loading logic
    (void)projectNode;
    (void)settings;
}

// ---------------------------------------------------------------------------
// ParseComponents
// ---------------------------------------------------------------------------
void ProjectLoader::ParseComponents(const pugi::xml_node& projectNode, ProjectSettings& settings)
{
    for (auto component : projectNode.children("BuiltInComponent"))
    {
        const char* name = component.attribute("Include").value();
        if (name && *name)
            settings.builtInComponents.push_back(name);
    }
}
