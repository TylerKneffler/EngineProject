#include "PreferencesView.h"
#include "Core/Renderers/RendererFactory.h"
#include <pugixml.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>

#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif

namespace
{
namespace fs = std::filesystem;

enum ExportStage
{
    ExportConfiguring,
    ExportBuilding,
    ExportPackaging
};

const char* ExportStageLabel(int stage)
{
    switch (stage)
    {
    case ExportConfiguring: return "Configuring portable Release build...";
    case ExportBuilding: return "Building the standalone game...";
    case ExportPackaging: return "Packaging executable, assets, and shaders...";
    default: return "Preparing export...";
    }
}

bool RunExportCommand(const std::wstring& command, const fs::path& workingDirectory,
    const fs::path& logFile)
{
    HANDLE log = CreateFileW(logFile.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE)
        return false;
    SetHandleInformation(log, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log;
    startup.hStdError = log;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, workingDirectory.c_str(), &startup, &process);
    CloseHandle(log);
    if (!created)
        return false;

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exitCode == 0;
}

void CopyRuntimeAssets(const fs::path& source, const fs::path& destination)
{
    for (const auto& entry : fs::recursive_directory_iterator(source))
    {
        const fs::path relative = fs::relative(entry.path(), source);
        const fs::path output = destination / relative;
        if (entry.is_directory())
        {
            fs::create_directories(output);
            continue;
        }
        if (!entry.is_regular_file())
            continue;

        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        if (extension == ".cpp" || extension == ".c" || extension == ".h" ||
            extension == ".hpp" || extension == ".inl")
            continue;
        fs::create_directories(output.parent_path());
        fs::copy_file(entry.path(), output, fs::copy_options::overwrite_existing);
    }
}

void MakePackagedProjectPortable(const fs::path& projectFile)
{
    pugi::xml_document document;
    if (!document.load_file(projectFile.c_str()))
        throw std::runtime_error("Could not update the packaged project settings");
    const auto project = document.child("Project");
    for (auto group : project.children("PropertyGroup"))
    {
        if (auto engine = group.child("EngineDirectory"))
            engine.text().set(".");
        if (auto shaders = group.child("ShadersDirectory"))
            shaders.text().set("Engine/Shaders/");
        if (auto build = group.child("BuildDirectory"))
            build.text().set("");
    }
    if (!document.save_file(projectFile.c_str(), "  "))
        throw std::runtime_error("Could not save the packaged project settings");
}

std::pair<bool, std::string> BuildPortableExport(const std::string& projectFile,
    const std::shared_ptr<std::atomic<float>>& progress,
    const std::shared_ptr<std::atomic<int>>& stage)
{
    try
    {
        const fs::path projectPath = fs::weakly_canonical(projectFile);
        const fs::path projectRoot = projectPath.parent_path();
        const fs::path buildDirectory = projectRoot / "build" / "Export";
        const fs::path exportDirectory = projectRoot / "Export";
        const fs::path logFile = projectRoot / "export-build.log";
        std::ofstream(logFile, std::ios::trunc)
            << "Building portable export for " << projectPath.string() << '\n';

        stage->store(ExportConfiguring);
        progress->store(0.10f);
        const bool hasPresets = fs::is_regular_file(projectRoot / "CMakePresets.json");
        const std::wstring configure = hasPresets
            ? L"cmake --preset export"
            : L"cmake -S \"" + projectRoot.wstring() + L"\" -B \"" +
                buildDirectory.wstring() +
                L"\" -G \"Visual Studio 17 2022\" -A x64 -DENGINE_PORTABLE_EXPORT=ON";
        if (!RunExportCommand(configure, projectRoot, logFile))
            return { false, "Export configuration failed. See " + logFile.string() };

        stage->store(ExportBuilding);
        progress->store(0.35f);
        const std::wstring build = hasPresets
            ? L"cmake --build --preset export --target Game --parallel"
            : L"cmake --build \"" + buildDirectory.wstring() +
                L"\" --config Release --target Game --parallel";
        if (!RunExportCommand(build, projectRoot, logFile))
            return { false, "Export build failed. See " + logFile.string() };

        const fs::path gameExecutable = buildDirectory / "Engine" / "Release" / "Game.exe";
        if (!fs::is_regular_file(gameExecutable))
            return { false, "Game.exe was not produced. See " + logFile.string() };

        stage->store(ExportPackaging);
        progress->store(0.82f);
        std::error_code error;
        fs::remove_all(exportDirectory, error);
        if (error)
            return { false, "Could not replace the Export folder: " + error.message() };
        fs::create_directories(exportDirectory / "Engine");

        const std::string gameName = projectPath.stem().string();
        fs::copy_file(gameExecutable, exportDirectory / (gameName + ".exe"),
            fs::copy_options::overwrite_existing);
        const fs::path packagedProject = exportDirectory / projectPath.filename();
        fs::copy_file(projectPath, packagedProject,
            fs::copy_options::overwrite_existing);
        MakePackagedProjectPortable(packagedProject);
        CopyRuntimeAssets(projectRoot / "Assets", exportDirectory / "Assets");
        fs::copy(fs::path(ENGINE_SHADERS_PATH), exportDirectory / "Engine" / "Shaders",
            fs::copy_options::recursive | fs::copy_options::overwrite_existing);

        const fs::path vulkanShaders = buildDirectory / "VulkanShaders";
        if (fs::is_directory(vulkanShaders))
            fs::copy(vulkanShaders, exportDirectory / "VulkanShaders",
                fs::copy_options::recursive | fs::copy_options::overwrite_existing);

        std::ofstream(exportDirectory / "README.txt", std::ios::trunc)
            << "Run " << gameName << ".exe from this folder.\n";
        progress->store(1.f);
        return { true, "Portable export ready: " + exportDirectory.string() };
    }
    catch (const std::exception& error)
    {
        return { false, std::string("Export failed: ") + error.what() };
    }
}

bool DrawRendererCombo(const char* label, std::string& selectedApi)
{
    bool changed = false;
    if (ImGui::BeginCombo(label, selectedApi.empty() ? "Not selected" : selectedApi.c_str()))
    {
        for (const auto& option : RendererFactory::GetRendererOptions())
        {
            const bool selected = selectedApi == option.name;
            std::string displayName = option.name;
            if (!option.available) displayName += " (Unavailable)";

            if (!option.available) ImGui::BeginDisabled();
            if (ImGui::Selectable(displayName.c_str(), selected) && option.available)
            {
                selectedApi = option.name;
                changed = true;
            }
            if (!option.available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", option.unavailableReason.c_str());
            if (!option.available) ImGui::EndDisabled();
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void PreferencesView::Init(const ProjectSettings& settings, const std::string& projFilePath)
{
    m_settings = settings;
    m_projFilePath = projFilePath;

    // Initialize string buffers
    strncpy_s(m_projectNameBuf, m_settings.name.c_str(), sizeof(m_projectNameBuf) - 1);
    strncpy_s(m_assetsPathBuf, m_settings.assetsDirectory.c_str(), sizeof(m_assetsPathBuf) - 1);
    strncpy_s(m_defaultSceneBuf, m_settings.defaultScene.c_str(), sizeof(m_defaultSceneBuf) - 1);
}

// ---------------------------------------------------------------------------
// DrawWindow
// ---------------------------------------------------------------------------
void PreferencesView::DrawWindow(bool& isOpen)
{
    if (!isOpen) return;
    UpdatePortableExport();

    ImGui::SetNextWindowSize(ImVec2(600, 700), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("Project Preferences", &isOpen))
    {
        if (ImGui::BeginTabBar("PreferencesTabs"))
        {
            if (ImGui::BeginTabItem("General"))
            {
                DrawMetadataSection();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Paths"))
            {
                DrawPathsSection();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Rendering"))
            {
                DrawRenderingSection();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Game"))
            {
                DrawAspectRatioSection();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Export"))
            {
                DrawExportSection();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Separator();
        if (m_projFilePath.empty())
        {
            ImGui::TextDisabled("Engine Sandbox settings are temporary and are not saved to a project file.");
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Save Project Settings"))
        {
            m_lastSaveSucceeded = SaveSettings();
            m_saveStatus = m_lastSaveSucceeded ? "Project settings saved." : "Failed to save project settings.";
        }
        if (m_projFilePath.empty())
            ImGui::EndDisabled();
        if (!m_saveStatus.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(m_lastSaveSucceeded ? ImVec4(0.35f, 0.85f, 0.45f, 1.0f)
                                                    : ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "%s", m_saveStatus.c_str());
        }
    }

    ImGui::End();
}

void PreferencesView::DrawExportSection()
{
    ImGui::Text("Portable Game Export");
    ImGui::Separator();
    ImGui::TextWrapped("Builds a Release game and packages its executable, project settings, assets, and shaders into the project's Export folder.");
    ImGui::Spacing();
    if (!m_projFilePath.empty())
        ImGui::TextDisabled("Output: %s", (std::filesystem::path(m_projFilePath).parent_path() / "Export").string().c_str());
    else
        ImGui::TextDisabled("Portable export requires a project file.");

    ImGui::Spacing();
    ImGui::BeginDisabled(m_exporting || m_projFilePath.empty());
    if (ImGui::Button("Build Portable Export", { 190.f, 34.f }))
        StartPortableExport();
    ImGui::EndDisabled();

    if (m_exporting)
    {
        ImGui::Spacing();
        ImGui::Text("%s", ExportStageLabel(m_exportStage ? m_exportStage->load() : 0));
        ImGui::ProgressBar(m_exportProgress ? m_exportProgress->load() : 0.f,
            { -1.f, 22.f });
        ImGui::TextDisabled("Build details: export-build.log");
    }
    else if (!m_exportStatus.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(m_exportSucceeded
            ? ImVec4(0.35f, 0.85f, 0.45f, 1.f)
            : ImVec4(1.f, 0.35f, 0.35f, 1.f), "%s", m_exportStatus.c_str());
    }
}

void PreferencesView::StartPortableExport()
{
    if (m_exporting || m_projFilePath.empty())
        return;
    if (!SaveSettings())
    {
        m_exportSucceeded = false;
        m_exportStatus = "Save the project settings before exporting.";
        return;
    }

    m_exporting = true;
    m_exportSucceeded = false;
    m_exportStatus.clear();
    m_exportProgress = std::make_shared<std::atomic<float>>(0.f);
    m_exportStage = std::make_shared<std::atomic<int>>(ExportConfiguring);
    const std::string projectFile = m_projFilePath;
    const auto progress = m_exportProgress;
    const auto stage = m_exportStage;
    m_exportFuture = std::async(std::launch::async,
        [projectFile, progress, stage]()
        {
            return BuildPortableExport(projectFile, progress, stage);
        });
}

void PreferencesView::UpdatePortableExport()
{
    if (!m_exporting || !m_exportFuture.valid() ||
        m_exportFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    const auto result = m_exportFuture.get();
    m_exporting = false;
    m_exportSucceeded = result.first;
    m_exportStatus = result.second;
}

// ---------------------------------------------------------------------------
// DrawMetadataSection
// ---------------------------------------------------------------------------
void PreferencesView::DrawMetadataSection()
{
    ImGui::Text("Project Information");
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputText("##projectName", m_projectNameBuf, sizeof(m_projectNameBuf)))
    {
        m_settings.name = m_projectNameBuf;
        NotifyChanged();
    }
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled("Project Name");

    ImGui::LabelText("Version", "%s", m_settings.version.c_str());
    ImGui::LabelText("Description", "%s", m_settings.description.c_str());
}

// ---------------------------------------------------------------------------
// DrawPathsSection
// ---------------------------------------------------------------------------
void PreferencesView::DrawPathsSection()
{
    ImGui::Text("Project Paths");
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputText("##assetsPath", m_assetsPathBuf, sizeof(m_assetsPathBuf)))
    {
        m_settings.assetsDirectory = m_assetsPathBuf;
        NotifyChanged();
    }
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled("Assets Directory");

    ImGui::LabelText("Scene Directory",   "%s", m_settings.sceneDirectory.c_str());
    ImGui::LabelText("Scripts Directory", "%s", m_settings.scriptsDirectory.c_str());
    ImGui::LabelText("Shaders Directory", "%s", m_settings.shadersDirectory.c_str());

    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputText("##defaultScene", m_defaultSceneBuf, sizeof(m_defaultSceneBuf)))
    {
        m_settings.defaultScene = m_defaultSceneBuf;
        NotifyChanged();
    }
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled("Default Scene");
}

// ---------------------------------------------------------------------------
// DrawRenderingSection
// ---------------------------------------------------------------------------
void PreferencesView::DrawRenderingSection()
{
    ImGui::Text("Rendering Settings");
    ImGui::Separator();

    if (DrawRendererCombo("Editor Rendering API", m_settings.editorRenderingAPI))
        NotifyChanged();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Changing the editor renderer takes effect after restarting the editor.");

    if (DrawRendererCombo("Game Rendering API", m_settings.gameRenderingAPI))
        NotifyChanged();

    for (const auto& option : RendererFactory::GetRendererOptions())
    {
        if (!option.available)
            ImGui::TextDisabled("%s unavailable: %s", option.name.c_str(), option.unavailableReason.c_str());
    }

    float clearColor[4] = { m_settings.clearColor.r, m_settings.clearColor.g, m_settings.clearColor.b, m_settings.clearColor.a };
    if (ImGui::ColorEdit4("Clear Color", clearColor))
    {
        m_settings.clearColor = glm::vec4(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        NotifyChanged();
    }

    if (ImGui::SliderInt("Target Framerate", (int*)&m_settings.targetFramerate, 30, 240))
        NotifyChanged();

    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputScalar("##editorViewportWidth", ImGuiDataType_U32, &m_settings.viewportWidth))
        NotifyChanged();
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled("Editor Viewport Width");

    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputScalar("##editorViewportHeight", ImGuiDataType_U32, &m_settings.viewportHeight))
        NotifyChanged();
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled("Editor Viewport Height");
}

// ---------------------------------------------------------------------------
// DrawAspectRatioSection
// ---------------------------------------------------------------------------
void PreferencesView::DrawAspectRatioSection()
{
    ImGui::Text("Game Resolution & Aspect Ratio");
    ImGui::Separator();

    // Aspect ratio mode
    const char* modes[] = { "Free", "Locked", "Hardcoded" };
    int currentMode = (int)m_settings.aspectRatioMode;
    if (ImGui::Combo("Aspect Ratio Mode", &currentMode, modes, 3))
    {
        m_settings.aspectRatioMode = (ProjectSettings::AspectRatioMode)currentMode;
        NotifyChanged();
    }

    ImGui::Separator();

    // Aspect ratio value (for Locked mode)
    if (ImGui::DragFloat("Game Aspect Ratio (W/H)", &m_settings.gameAspectRatio, 0.01f, 0.5f, 5.0f, "%.3f"))
        NotifyChanged();

    // Game window size (for Hardcoded mode)
    if (ImGui::InputScalar("Game Window Width", ImGuiDataType_U32, &m_settings.gameWindowWidth))
        NotifyChanged();
    if (ImGui::InputScalar("Game Window Height", ImGuiDataType_U32, &m_settings.gameWindowHeight))
        NotifyChanged();

    // Letterbox color
    float letterboxColor[4] = { m_settings.letterboxColor.r, m_settings.letterboxColor.g, m_settings.letterboxColor.b, m_settings.letterboxColor.a };
    if (ImGui::ColorEdit4("Letterbox Color", letterboxColor))
    {
        m_settings.letterboxColor = glm::vec4(letterboxColor[0], letterboxColor[1], letterboxColor[2], letterboxColor[3]);
        NotifyChanged();
    }
}

// ---------------------------------------------------------------------------
// SaveSettings
// ---------------------------------------------------------------------------
bool PreferencesView::SaveSettings()
{
    try
    {
        pugi::xml_document doc;
        
        // Load existing XML
        if (!doc.load_file(m_projFilePath.c_str()))
        {
            std::cerr << "Failed to load project file for saving" << std::endl;
            return false;
        }

        auto projectNode = doc.child("Project");
        if (!projectNode)
            return false;

        // Update metadata
        for (auto prop : projectNode.children("PropertyGroup"))
        {
            auto nameNode = prop.child("ProjectName");
            if (nameNode)
                nameNode.text().set(m_settings.name.c_str());

            auto assetsNode = prop.child("AssetsDirectory");
            if (assetsNode)
                assetsNode.text().set(m_settings.assetsDirectory.c_str());

            auto defaultSceneNode = prop.child("DefaultScene");
            if (defaultSceneNode)
                defaultSceneNode.text().set(m_settings.defaultScene.c_str());

            auto renderingApi = prop.child("RenderingAPI");
            if (renderingApi)
                renderingApi.text().set(m_settings.gameRenderingAPI.c_str());

            auto editorRenderingApi = prop.child("EditorRenderingAPI");
            if (editorRenderingApi)
                editorRenderingApi.text().set(m_settings.editorRenderingAPI.c_str());

            auto gameRenderingApi = prop.child("GameRenderingAPI");
            if (gameRenderingApi)
                gameRenderingApi.text().set(m_settings.gameRenderingAPI.c_str());

            auto clearColorR = prop.child("ClearColorR");
            if (clearColorR)
                clearColorR.text().set(std::to_string(m_settings.clearColor.r).c_str());

            auto clearColorG = prop.child("ClearColorG");
            if (clearColorG)
                clearColorG.text().set(std::to_string(m_settings.clearColor.g).c_str());

            auto clearColorB = prop.child("ClearColorB");
            if (clearColorB)
                clearColorB.text().set(std::to_string(m_settings.clearColor.b).c_str());

            auto framerate = prop.child("TargetFramerate");
            if (framerate)
                framerate.text().set(std::to_string(m_settings.targetFramerate).c_str());

            auto modeNode = prop.child("AspectRatioMode");
            if (modeNode)
            {
                const char* modeStr = "";
                switch (m_settings.aspectRatioMode)
                {
                    case ProjectSettings::AspectRatioMode::Free: modeStr = "Free"; break;
                    case ProjectSettings::AspectRatioMode::Locked: modeStr = "Locked"; break;
                    case ProjectSettings::AspectRatioMode::Hardcoded: modeStr = "Hardcoded"; break;
                }
                modeNode.text().set(modeStr);
            }

            auto aspectNode = prop.child("GameAspectRatio");
            if (aspectNode)
                aspectNode.text().set(std::to_string(m_settings.gameAspectRatio).c_str());

            auto widthNode = prop.child("GameWindowWidth");
            if (widthNode)
                widthNode.text().set(std::to_string(m_settings.gameWindowWidth).c_str());

            auto heightNode = prop.child("GameWindowHeight");
            if (heightNode)
                heightNode.text().set(std::to_string(m_settings.gameWindowHeight).c_str());

            auto lbR = prop.child("LetterboxColorR");
            if (lbR)
                lbR.text().set(std::to_string(m_settings.letterboxColor.r).c_str());

            auto lbG = prop.child("LetterboxColorG");
            if (lbG)
                lbG.text().set(std::to_string(m_settings.letterboxColor.g).c_str());

            auto lbB = prop.child("LetterboxColorB");
            if (lbB)
                lbB.text().set(std::to_string(m_settings.letterboxColor.b).c_str());

            auto lbA = prop.child("LetterboxColorA");
            if (lbA)
                lbA.text().set(std::to_string(m_settings.letterboxColor.a).c_str());
        }

        // Save to file
        if (!doc.save_file(m_projFilePath.c_str()))
        {
            std::cerr << "Failed to save project file" << std::endl;
            return false;
        }

        std::cout << "Project settings saved successfully" << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error saving project settings: " << e.what() << std::endl;
        return false;
    }
}
