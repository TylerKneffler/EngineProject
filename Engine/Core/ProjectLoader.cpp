#include "ProjectLoader.h"
#include <filesystem>
#include <fstream>
#include <pugixml.hpp>
#include <regex>
#include <set>

// ---------------------------------------------------------------------------
// LoadProject
// ---------------------------------------------------------------------------
namespace Engine::Core
{
ProjectLoader::ProjectSettings ProjectLoader::LoadProject(const std::string& projFilePath)
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
    ParseDependencies(projectNode, settings, projFilePath);
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
        auto engineDir = prop.child("EngineDirectory");
        if (engineDir)
            settings.engineDirectory = engineDir.child_value();

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

        auto hierarchyDebug = prop.child("DebugHierarchyInteractions");
        if (hierarchyDebug)
        {
            std::string value = hierarchyDebug.child_value();
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            settings.debugHierarchyInteractions = value != "false" && value != "0" && value != "off";
        }

        auto historyLimit = prop.child("EditorHistoryLimit");
        if (historyLimit)
            settings.editorHistoryLimit = static_cast<uint32_t>(
                std::min<unsigned long>(std::stoul(historyLimit.child_value()), 1000ul));

        auto editorMode = prop.child("EditorMode");
        if (editorMode)
        {
            std::string value = editorMode.child_value();
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            settings.editorMode = value == "2d" || value == "two" || value == "twod"
                ? ProjectSettings::EditorMode::TwoD
                : ProjectSettings::EditorMode::ThreeD;
        }

        auto editorTheme = prop.child("EditorTheme");
        if (editorTheme && *editorTheme.child_value())
            settings.editorTheme = editorTheme.child_value();

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

        auto lightmapResolution = prop.child("BakedLightmapResolution");
        if (lightmapResolution)
            settings.bakedLighting.lightmapResolution =
                std::stoul(lightmapResolution.child_value());
        auto shadowBias = prop.child("BakedShadowBias");
        if (shadowBias)
            settings.bakedLighting.shadowBias = std::stof(shadowBias.child_value());
        auto dilationPasses = prop.child("BakedDilationPasses");
        if (dilationPasses)
            settings.bakedLighting.dilationPasses =
                std::stoul(dilationPasses.child_value());
        auto accumulate = prop.child("BakedPreserveSourceEmission");
        if (accumulate)
            settings.bakedLighting.accumulate =
                std::string(accumulate.child_value()) != "false";

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
void ProjectLoader::ParseDependencies(const pugi::xml_node& projectNode,
    ProjectSettings& settings, const std::string& projectFilePath)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot = fs::absolute(fs::path(projectFilePath)).parent_path();
    std::set<std::string> included;
    std::vector<std::pair<std::string, fs::path>> pending;

    const auto normalizedManifestPath = [&projectRoot](const fs::path& path)
    {
        std::string generic = path.generic_string();
        const std::string engineAssets = "Engine/Core/Assets/";
        const size_t engineAssetsOffset = generic.find(engineAssets);
        if (engineAssetsOffset != std::string::npos)
            return std::string("Assets/") + generic.substr(
                engineAssetsOffset + engineAssets.size());
        if (path.is_absolute())
        {
            std::error_code error;
            const fs::path relative = fs::relative(path, projectRoot, error);
            if (!error && !relative.empty() &&
                *relative.begin() != fs::path(".."))
                generic = relative.generic_string();
        }
        while (generic.rfind("./", 0) == 0)
            generic.erase(0, 2);
        return fs::path(generic).lexically_normal().generic_string();
    };

    const auto physicalPath = [&projectRoot](const std::string& manifestPath,
        const fs::path& referringFile)
    {
        fs::path reference(manifestPath);
        if (reference.is_absolute())
            return reference;
        const std::string generic = reference.generic_string();
        const fs::path projectRelative = projectRoot / reference;
        std::error_code existsError;
        if (fs::exists(projectRelative, existsError))
            return projectRelative;
        if (generic.rfind("Assets/", 0) == 0 ||
            generic.rfind("Engine/Core/Assets/", 0) == 0)
            return projectRelative;
        return referringFile.empty() ? projectRelative :
            referringFile.parent_path() / reference;
    };

    const auto enqueue = [&](const std::string& reference,
        const fs::path& referringFile)
    {
        if (reference.empty())
            return;
        const fs::path file = physicalPath(reference, referringFile)
            .lexically_normal();
        const std::string manifestPath = normalizedManifestPath(file);
        if (included.insert(manifestPath).second)
            pending.emplace_back(manifestPath, file);
    };

    const auto includeScene = [&](const pugi::xml_node& scene)
    {
        const std::string path = scene.attribute("Include").value();
        if (path.empty())
            return;
        const std::string manifestPath = normalizedManifestPath(path);
        if (std::find(settings.includedScenes.begin(),
                settings.includedScenes.end(), manifestPath) ==
            settings.includedScenes.end())
            settings.includedScenes.push_back(manifestPath);
        enqueue(path, {});
    };
    for (auto scene : projectNode.children("Scene"))
        includeScene(scene);
    for (auto itemGroup : projectNode.children("ItemGroup"))
        for (auto scene : itemGroup.children("Scene"))
            includeScene(scene);

    // Asset-bearing scene formats are deliberately text based. Extracting
    // path-shaped values keeps the manifest independent of component types and
    // also follows prefab, material, sprite-animation, and spritesheet chains.
    const std::regex assetReference(
        R"(([A-Za-z]:)?[A-Za-z0-9_./\\ -]+\.(scene|prefab|obj|fbx|gltf|glb|material|png|jpg|jpeg|dds|ktx|ktx2|hdr|exr|wav|ogg|mp3|ttf|otf|spriteanim|spritesheet|json|hlsl|glsl|vert|frag|comp))",
        std::regex::icase);
    for (size_t index = 0; index < pending.size(); ++index)
    {
        const fs::path file = pending[index].second;
        std::ifstream input(file, std::ios::binary);
        if (!input)
            continue;
        const std::string content((std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        for (std::sregex_iterator match(content.begin(), content.end(),
                 assetReference), end; match != end; ++match)
            enqueue((*match)[0].str(), file);
    }
    settings.includedAssets.assign(included.begin(), included.end());
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
}
