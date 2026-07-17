#include "PreferencesView.h"
#include "Engine/Editor/UI/IEditorUi.h"
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

bool DrawRendererCombo(IEditorUi& ui, const char* label, std::string& selectedApi)
{
    const auto options = RendererFactory::GetRendererOptions();
    std::vector<std::string> labels;
    std::vector<const char*> items;
    int selected = 0;
    for (size_t i = 0; i < options.size(); ++i)
    {
        labels.push_back(options[i].name + (options[i].available ? "" : " (Unavailable)"));
        if (selectedApi == options[i].name) selected = static_cast<int>(i);
    }
    for (auto& item : labels) items.push_back(item.c_str());
    if (!ui.Combo(label, &selected, items.data(), static_cast<int>(items.size()))) return false;
    if (selected < 0 || selected >= static_cast<int>(options.size()) || !options[selected].available) return false;
    selectedApi = options[selected].name;
    return true;
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
void PreferencesView::DrawWindow(IEditorUi& ui, bool& isOpen)
{
    if (!isOpen) return;
    UpdatePortableExport();

    ui.SetNextWindowRect(100, 50, 600, 700);
    
    if (ui.BeginWindow("Project Preferences", &isOpen))
    {
        if (ui.BeginTabBar("PreferencesTabs"))
        {
            if (ui.BeginTab("General"))
            {
                DrawMetadataSection(ui);
                ui.EndTab();
            }

            if (ui.BeginTab("Paths"))
            {
                DrawPathsSection(ui);
                ui.EndTab();
            }

            if (ui.BeginTab("Rendering"))
            {
                DrawRenderingSection(ui);
                ui.EndTab();
            }

            if (ui.BeginTab("Game"))
            {
                DrawAspectRatioSection(ui);
                ui.EndTab();
            }

            if (ui.BeginTab("Export"))
            {
                DrawExportSection(ui);
                ui.EndTab();
            }

            ui.EndTabBar();
        }

        ui.Separator();
        if (m_projFilePath.empty())
        {
            ui.DisabledLabel("Engine Sandbox settings are temporary and are not saved to a project file.");
            ui.BeginDisabled();
        }
        if (ui.Button("Save Project Settings"))
        {
            m_lastSaveSucceeded = SaveSettings();
            m_saveStatus = m_lastSaveSucceeded ? "Project settings saved." : "Failed to save project settings.";
        }
        if (m_projFilePath.empty())
            ui.EndDisabled();
        if (!m_saveStatus.empty())
        {
            ui.SameLine();
            ui.ColoredLabel(m_saveStatus.c_str(), m_lastSaveSucceeded ? EditorUiColor{.35f,.85f,.45f,1} : EditorUiColor{1,.35f,.35f,1});
        }
    }

    ui.EndWindow();
}

void PreferencesView::ConfigureEditorUiPackage(
    const std::string& activePackage,
    std::function<void(const std::string&)> switchPackage)
{
    m_activeEditorUiPackage = activePackage;
    m_switchEditorUiPackage = std::move(switchPackage);
}

void PreferencesView::DrawExportSection(IEditorUi& ui)
{
    ui.Label("Portable Game Export");
    ui.Separator();
    ui.Label("Builds a Release game and packages its executable, project settings, assets, and shaders into the project's Export folder.");
    ui.Spacing();
    if (!m_projFilePath.empty())
    { std::string output = "Output: " + (std::filesystem::path(m_projFilePath).parent_path() / "Export").string(); ui.DisabledLabel(output.c_str()); }
    else
        ui.DisabledLabel("Portable export requires a project file.");

    ui.Spacing();
    ui.BeginDisabled(m_exporting || m_projFilePath.empty());
    if (ui.Button("Build Portable Export", 190.f, 34.f))
        StartPortableExport();
    ui.EndDisabled();

    if (m_exporting)
    {
        ui.Spacing();
        ui.Label(ExportStageLabel(m_exportStage ? m_exportStage->load() : 0));
        ui.Progress(m_exportProgress ? m_exportProgress->load() : 0.f);
        ui.DisabledLabel("Build details: export-build.log");
    }
    else if (!m_exportStatus.empty())
    {
        ui.Spacing();
        ui.ColoredLabel(m_exportStatus.c_str(), m_exportSucceeded
            ? EditorUiColor{.35f,.85f,.45f,1.f} : EditorUiColor{1.f,.35f,.35f,1.f});
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
void PreferencesView::DrawMetadataSection(IEditorUi& ui)
{
    ui.Label("Project Information");
    ui.Separator();

    if (ui.InputText("##projectName", m_projectNameBuf, sizeof(m_projectNameBuf)))
    {
        m_settings.name = m_projectNameBuf;
        NotifyChanged();
    }
    ui.SameLine(); ui.DisabledLabel("Project Name");

    ui.ValueLabel("Version", m_settings.version.c_str());
    ui.ValueLabel("Description", m_settings.description.c_str());
}

// ---------------------------------------------------------------------------
// DrawPathsSection
// ---------------------------------------------------------------------------
void PreferencesView::DrawPathsSection(IEditorUi& ui)
{
    ui.Label("Project Paths"); ui.Separator();

    if (ui.InputText("##assetsPath", m_assetsPathBuf, sizeof(m_assetsPathBuf)))
    {
        m_settings.assetsDirectory = m_assetsPathBuf;
        NotifyChanged();
    }
    ui.SameLine(); ui.DisabledLabel("Assets Directory");

    ui.ValueLabel("Scene Directory", m_settings.sceneDirectory.c_str());
    ui.ValueLabel("Scripts Directory", m_settings.scriptsDirectory.c_str());
    ui.ValueLabel("Shaders Directory", m_settings.shadersDirectory.c_str());

    if (ui.InputText("##defaultScene", m_defaultSceneBuf, sizeof(m_defaultSceneBuf)))
    {
        m_settings.defaultScene = m_defaultSceneBuf;
        NotifyChanged();
    }
    ui.SameLine(); ui.DisabledLabel("Default Scene");
}

// ---------------------------------------------------------------------------
// DrawRenderingSection
// ---------------------------------------------------------------------------
void PreferencesView::DrawRenderingSection(IEditorUi& ui)
{
    ui.Label("Rendering Settings"); ui.Separator();

    const char* uiPackages[] = { "ImGui", "Nuklear" };
    const std::string displayedPackage = m_settings.editorUiPackage.empty()
        ? m_activeEditorUiPackage : m_settings.editorUiPackage;
    int currentUiPackage = displayedPackage == "Nuklear" ? 1 : 0;
    if (ui.Combo("Editor UI Package", &currentUiPackage, uiPackages, 2))
    {
        m_settings.editorUiPackage = uiPackages[currentUiPackage];
        if (m_switchEditorUiPackage)
            m_switchEditorUiPackage(m_settings.editorUiPackage);
        NotifyChanged();
    }
    ui.Tooltip("Applies immediately. Use Save Project Settings to persist it.");
    ui.Separator();

    if (DrawRendererCombo(ui, "Editor Rendering API", m_settings.editorRenderingAPI))
        NotifyChanged();
    ui.Tooltip("Changing the editor renderer takes effect after restarting the editor.");

    if (DrawRendererCombo(ui, "Game Rendering API", m_settings.gameRenderingAPI))
        NotifyChanged();

    for (const auto& option : RendererFactory::GetRendererOptions())
    {
        if (!option.available)
        { std::string reason=option.name+" unavailable: "+option.unavailableReason; ui.DisabledLabel(reason.c_str()); }
    }

    float clearColor[4] = { m_settings.clearColor.r, m_settings.clearColor.g, m_settings.clearColor.b, m_settings.clearColor.a };
    if (ui.ColorEdit4("Clear Color", clearColor))
    {
        m_settings.clearColor = glm::vec4(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        NotifyChanged();
    }

    if (ui.SliderInt("Target Framerate", reinterpret_cast<int*>(&m_settings.targetFramerate), 30, 240))
        NotifyChanged();

    if (ui.InputUInt("##editorViewportWidth", &m_settings.viewportWidth))
        NotifyChanged();
    ui.SameLine(); ui.DisabledLabel("Editor Viewport Width");

    if (ui.InputUInt("##editorViewportHeight", &m_settings.viewportHeight))
        NotifyChanged();
    ui.SameLine(); ui.DisabledLabel("Editor Viewport Height");
}

// ---------------------------------------------------------------------------
// DrawAspectRatioSection
// ---------------------------------------------------------------------------
void PreferencesView::DrawAspectRatioSection(IEditorUi& ui)
{
    ui.Label("Game Resolution & Aspect Ratio"); ui.Separator();

    // Aspect ratio mode
    const char* modes[] = { "Free", "Locked", "Hardcoded" };
    int currentMode = (int)m_settings.aspectRatioMode;
    if (ui.Combo("Aspect Ratio Mode", &currentMode, modes, 3))
    {
        m_settings.aspectRatioMode = (ProjectSettings::AspectRatioMode)currentMode;
        NotifyChanged();
    }

    ui.Separator();

    // Aspect ratio value (for Locked mode)
    if (ui.DragFloat("Game Aspect Ratio (W/H)", &m_settings.gameAspectRatio, 0.01f, 0.5f, 5.0f))
        NotifyChanged();

    // Game window size (for Hardcoded mode)
    if (ui.InputUInt("Game Window Width", &m_settings.gameWindowWidth))
        NotifyChanged();
    if (ui.InputUInt("Game Window Height", &m_settings.gameWindowHeight))
        NotifyChanged();

    // Letterbox color
    float letterboxColor[4] = { m_settings.letterboxColor.r, m_settings.letterboxColor.g, m_settings.letterboxColor.b, m_settings.letterboxColor.a };
    if (ui.ColorEdit4("Letterbox Color", letterboxColor))
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

            auto editorUiPackage = prop.child("EditorUIPackage");
            if (editorUiPackage)
                editorUiPackage.text().set(m_settings.editorUiPackage.c_str());
            else if (editorRenderingApi && !m_settings.editorUiPackage.empty())
                prop.append_child("EditorUIPackage").text().set(m_settings.editorUiPackage.c_str());

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
