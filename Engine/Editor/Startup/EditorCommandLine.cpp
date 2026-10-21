#include "Engine/Editor/Startup/EditorCommandLine.h"

#include "Engine/Editor/Core/Importers/ModelImporter.h"
#include "Engine/Editor/Core/Importers/MmdDanceAssetBinder.h"
#include "Engine/Editor/ProjectLauncher.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <shellapi.h>

#ifndef PROJECT_FILE
#define PROJECT_FILE ""
#endif

namespace Engine::Editor
{
    namespace
    {
        void LogImportResult(const std::string& message)
        {
            std::ofstream log("editor-startup.log", std::ios::app);
            if (log) log << message << '\n';
        }
    }

    EditorCommandLine::EditorCommandLine()
    {
        int count = 0;
        LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!values) return;
        arguments_.assign(values, values + count);
        LocalFree(values);
    }

    std::optional<int> EditorCommandLine::RunImportCommand() const
    {
        if (arguments_.size() < 4) return std::nullopt;

        if (arguments_[1] == L"--bind-vmd")
        {
            std::string error;
            const bool bound = MmdDanceAssetBinder::BindFromPrefab(
                std::filesystem::path(arguments_[2]).string(),
                std::filesystem::path(arguments_[3]).string(), error);
            LogImportResult(bound ? "VMD bound to imported prefab" :
                "VMD binding failed: " + error);
            return bound ? 0 : 1;
        }
        if (arguments_[1] != L"--import-model" && arguments_[1] != L"--import-gltf")
            return std::nullopt;

        PmxImportOptions options;
        for (size_t index = 4; index < arguments_.size(); ++index)
        {
            const auto& argument = arguments_[index];
            if (argument == L"--pmx-dynamic-physics" || argument == L"--pmx-physics-high")
            {
                options.dynamicPhysics = true;
                options.physicsQuality = PmxPhysicsQuality::High;
            }
            else if (argument == L"--pmx-physics-medium")
            {
                options.dynamicPhysics = true;
                options.physicsQuality = PmxPhysicsQuality::Medium;
            }
            else if (argument == L"--pmx-physics-low")
            {
                options.dynamicPhysics = true;
                options.physicsQuality = PmxPhysicsQuality::Low;
            }
            else if (argument == L"--pmx-weight-fitted-colliders" ||
                argument == L"--pmx-collider-hull")
            {
                options.weightFittedColliders = true;
                options.colliderMode = PmxColliderMode::Hull;
            }
            else if (argument == L"--pmx-collider-primitive")
            {
                options.weightFittedColliders = false;
                options.colliderMode = PmxColliderMode::Primitive;
            }
            else if (argument == L"--pmx-collider-none")
            {
                options.weightFittedColliders = false;
                options.colliderMode = PmxColliderMode::None;
            }
        }
        const Model::ModelImportResult result = ModelImporter::Import(
            std::filesystem::path(arguments_[2]).string(),
            std::filesystem::path(arguments_[3]).string(), 1.f, options);
        LogImportResult(result.success
            ? "Asset imported: " + (result.primaryAssetPath.empty()
                ? result.prefabPath : result.primaryAssetPath)
            : "Model import failed: " + result.message);
        return result.success ? 0 : 1;
    }

    std::string EditorCommandLine::ResolveProject() const
    {
        if (arguments_.size() > 1 && std::filesystem::exists(arguments_[1]))
            return std::filesystem::absolute(arguments_[1]).string();
        if (std::string(PROJECT_FILE).size() > 0 && std::filesystem::exists(PROJECT_FILE))
            return std::filesystem::absolute(PROJECT_FILE).string();

        std::vector<std::string> projects;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path()))
            if (entry.is_regular_file() && entry.path().extension() == ".proj")
                projects.push_back(std::filesystem::absolute(entry.path()).string());
        if (projects.size() == 1)
            return projects.front();
        for (const auto& project : projects)
            ProjectLauncher::RememberProject(project);
        return {};
    }

    bool EditorCommandLine::HasArgument(const wchar_t* argument) const
    {
        if (arguments_.size() <= 1) return false;
        return std::find(arguments_.begin() + 1, arguments_.end(), argument) !=
            arguments_.end();
    }

    std::string EditorCommandLine::RendererOverride() const
    {
        for (size_t index = 1; index + 1 < arguments_.size(); ++index)
            if (arguments_[index] == L"--editor-renderer")
                return std::filesystem::path(arguments_[index + 1]).string();
        return {};
    }
}
