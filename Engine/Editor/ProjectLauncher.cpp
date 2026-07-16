#include "pch.h"
#include "Engine/Editor/ProjectLauncher.h"
#include "Engine/Editor/UI/ImGui/ImGuiUiBackend.h"
#include "Core/Window.h"
#include "Core/ProjectLoader.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "imgui.h"
#include <commdlg.h>
#include <shobjidl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iterator>
#include <unordered_set>

#ifndef ENGINE_ROOT_PATH
#define ENGINE_ROOT_PATH "."
#endif
#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fs = std::filesystem;

namespace
{
enum class ProjectSetupStage
{
    Validating,
    CreatingFolders,
    CopyingAssets,
    WritingProjectFiles,
    CreatingShortcut,
    Configuring,
    Building,
    Launching
};

struct ProjectSetupProgress
{
    std::atomic<float> value{ 0.f };
    std::atomic<ProjectSetupStage> stage{ ProjectSetupStage::Validating };

    void Set(float newValue, ProjectSetupStage newStage)
    {
        stage.store(newStage);
        value.store(newValue);
    }
};

const char* SetupStageLabel(ProjectSetupStage stage)
{
    switch (stage)
    {
    case ProjectSetupStage::Validating: return "Validating project settings...";
    case ProjectSetupStage::CreatingFolders: return "Creating project folders...";
    case ProjectSetupStage::CopyingAssets: return "Copying starter assets...";
    case ProjectSetupStage::WritingProjectFiles: return "Writing project and Visual Studio files...";
    case ProjectSetupStage::CreatingShortcut: return "Creating the Editor shortcut...";
    case ProjectSetupStage::Configuring: return "Configuring the project with CMake...";
    case ProjectSetupStage::Building: return "Building the project Editor...";
    case ProjectSetupStage::Launching: return "Starting the project Editor...";
    }
    return "Preparing project...";
}

struct ProjectBuildResult
{
    bool success = false;
    std::string projectFile;
    std::string editorExecutable;
    std::string message;
};

std::string EnvironmentVariable(const char* name)
{
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || !value)
        return {};
    std::string result(value);
    free(value);
    return result;
}

fs::path RecentProjectsFile()
{
    const std::string localAppData = EnvironmentVariable("LOCALAPPDATA");
    fs::path directory = !localAppData.empty() ? fs::path(localAppData) / "EngineProject"
                                              : fs::temp_directory_path() / "EngineProject";
    std::error_code error;
    fs::create_directories(directory, error);
    return directory / "recent-projects.txt";
}

std::vector<std::string> LoadRecentProjects()
{
    std::vector<std::string> projects;
    std::ifstream input(RecentProjectsFile());
    std::string line;
    while (std::getline(input, line))
        if (!line.empty())
            projects.push_back(line);
    return projects;
}

void SaveRecentProjects(const std::vector<std::string>& projects)
{
    std::ofstream output(RecentProjectsFile(), std::ios::trunc);
    for (const auto& project : projects)
        output << project << '\n';
}

std::string AbsoluteProjectPath(const fs::path& path)
{
    std::error_code error;
    const fs::path absolute = fs::weakly_canonical(fs::absolute(path), error);
    return (error ? fs::absolute(path) : absolute).string();
}

std::string ReadText(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not read template: " + path.string());
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

void ReplaceAll(std::string& text, const std::string& from, const std::string& to)
{
    size_t offset = 0;
    while ((offset = text.find(from, offset)) != std::string::npos)
    {
        text.replace(offset, from.size(), to);
        offset += to.size();
    }
}

void WriteTemplate(const fs::path& source, const fs::path& destination,
    const std::string& projectName)
{
    std::string content = ReadText(source);
    std::string engineRoot = fs::path(ENGINE_ROOT_PATH).generic_string();
    ReplaceAll(content, "{{PROJECT_NAME}}", projectName);
    ReplaceAll(content, "{{ENGINE_ROOT}}", engineRoot);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("Could not create: " + destination.string());
    output << content;
}

void CreateEditorShortcut(const fs::path& projectRoot, const fs::path& projectFile,
    const std::string& projectName)
{
    const fs::path editorExecutable =
        projectRoot / "build" / "Debug" / "Engine" / "Debug" / "Editor.exe";
    const fs::path shortcutPath = projectRoot / ("Open " + projectName + " Editor.lnk");

    Microsoft::WRL::ComPtr<IShellLinkW> shortcut;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&shortcut));
    if (FAILED(result))
        throw std::runtime_error("Could not create the project editor shortcut.");

    const std::wstring arguments = L"\"" + projectFile.wstring() + L"\"";
    const std::wstring description = L"Open " + fs::path(projectName).wstring() +
        L" in EngineProject Editor";
    if (FAILED(shortcut->SetPath(editorExecutable.c_str())) ||
        FAILED(shortcut->SetArguments(arguments.c_str())) ||
        FAILED(shortcut->SetWorkingDirectory(projectRoot.c_str())) ||
        FAILED(shortcut->SetDescription(description.c_str())) ||
        FAILED(shortcut->SetIconLocation(editorExecutable.c_str(), 0)))
    {
        throw std::runtime_error("Could not configure the project editor shortcut.");
    }

    Microsoft::WRL::ComPtr<IPersistFile> persistFile;
    if (FAILED(shortcut.As(&persistFile)) ||
        FAILED(persistFile->Save(shortcutPath.c_str(), TRUE)))
    {
        throw std::runtime_error("Could not save the project editor shortcut: " +
            shortcutPath.string());
    }
}

std::string CreateProject(const std::string& name, const std::string& location,
    const std::shared_ptr<ProjectSetupProgress>& progress = {})
{
    if (progress) progress->Set(0.03f, ProjectSetupStage::Validating);
    if (name.empty())
        throw std::runtime_error("Enter a project name.");
    if (!std::isalpha(static_cast<unsigned char>(name.front())))
        throw std::runtime_error("Project names must begin with a letter.");
    for (char character : name)
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_' && character != '-')
            throw std::runtime_error("Project names may contain only letters, numbers, '_' and '-'.");
    if (location.empty())
        throw std::runtime_error("Choose a parent directory.");

    const fs::path projectRoot = fs::path(location) / name;
    if (fs::exists(projectRoot))
        throw std::runtime_error("The project directory already exists: " + projectRoot.string());

    const fs::path templateRoot = fs::path(ENGINE_ROOT_PATH) / "ProjectTemplate";
    try
    {
        if (progress) progress->Set(0.10f, ProjectSetupStage::CreatingFolders);
        fs::create_directories(projectRoot / ".vscode");
        if (progress) progress->Set(0.18f, ProjectSetupStage::CopyingAssets);
        fs::copy(fs::path(ENGINE_ASSETS_PATH), projectRoot / "Assets",
            fs::copy_options::recursive);
        if (progress) progress->Set(0.35f, ProjectSetupStage::WritingProjectFiles);
        WriteTemplate(templateRoot / "CMakeLists.txt.in", projectRoot / "CMakeLists.txt", name);
        WriteTemplate(templateRoot / "CMakePresets.json.in", projectRoot / "CMakePresets.json", name);
        WriteTemplate(templateRoot / "Project.proj.in", projectRoot / (name + ".proj"), name);
        WriteTemplate(templateRoot / "tasks.json.in", projectRoot / ".vscode" / "tasks.json", name);
        WriteTemplate(templateRoot / "gitignore.in", projectRoot / ".gitignore", name);
        if (progress) progress->Set(0.48f, ProjectSetupStage::CreatingShortcut);
        CreateEditorShortcut(projectRoot, projectRoot / (name + ".proj"), name);
    }
    catch (...)
    {
        std::error_code ignored;
        fs::remove_all(projectRoot, ignored);
        throw;
    }
    return AbsoluteProjectPath(projectRoot / (name + ".proj"));
}

std::string BrowseForProject(HWND owner)
{
    wchar_t fileName[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"Engine Projects (*.proj)\0*.proj\0All Files (*.*)\0*.*\0";
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog))
        return {};
    return AbsoluteProjectPath(fs::path(fileName));
}

std::string BrowseForFolder(HWND owner)
{
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog))))
        return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (FAILED(dialog->Show(owner)))
        return {};
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)))
        return {};
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return {};
    std::string result = fs::path(path).string();
    CoTaskMemFree(path);
    return result;
}

std::string ChooseLauncherRenderer()
{
    for (const auto& option : RendererFactory::GetRendererOptions())
        if (option.available && option.name == "DirectX11")
            return option.name;
    for (const auto& option : RendererFactory::GetRendererOptions())
        if (option.available)
            return option.name;
    return {};
}

bool RunCommand(const std::wstring& command, const fs::path& workingDirectory,
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

ProjectBuildResult BuildProjectEditor(const std::string& projectFile,
    const std::shared_ptr<ProjectSetupProgress>& progress = {})
{
    ProjectBuildResult result{};
    result.projectFile = projectFile;
    const fs::path projectRoot = fs::path(projectFile).parent_path();
    const fs::path buildDirectory = projectRoot / "build" / "Debug";
    const fs::path logFile = projectRoot / "project-build.log";
    result.editorExecutable = (buildDirectory / "Engine" / "Debug" / "Editor.exe").string();

    std::error_code error;
    fs::create_directories(buildDirectory, error);
    std::ofstream(logFile, std::ios::trunc)
        << "Configuring and building the EngineProject editor for " << projectRoot.string() << "\n";

    if (progress) progress->Set(0.55f, ProjectSetupStage::Configuring);
    const std::wstring configure = L"cmake --preset debug";
    if (!RunCommand(configure, projectRoot, logFile))
    {
        result.message = "Project configuration failed. See " + logFile.string();
        return result;
    }

    if (progress) progress->Set(0.70f, ProjectSetupStage::Building);
    const std::wstring build = L"cmake --build --preset debug --target Editor --parallel";
    if (!RunCommand(build, projectRoot, logFile) || !fs::exists(result.editorExecutable))
    {
        result.message = "Editor build failed. See " + logFile.string();
        return result;
    }

    result.success = true;
    result.message = "Editor build completed.";
    if (progress) progress->Set(0.95f, ProjectSetupStage::Launching);
    return result;
}

bool LaunchProjectEditor(const std::string& executable, const std::string& projectFile)
{
    const fs::path projectRoot = fs::path(projectFile).parent_path();
    auto launch = [&](const std::wstring& extraArguments) -> bool
    {
        std::wstring command = L"\"" + fs::path(executable).wstring() + L"\" \"" +
            fs::path(projectFile).wstring() + L"\"" + extraArguments;
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(fs::path(executable).c_str(), mutableCommand.data(),
            nullptr, nullptr, FALSE, 0, nullptr, projectRoot.c_str(), &startup, &process);
        if (!created)
            return false;

        // A process can remain alive while a renderer error dialog is open. Only
        // close the hub after the real editor window has finished initializing
        // and become visible.
        bool editorReady = false;
        for (int attempt = 0; attempt < 50 && !editorReady; ++attempt)
        {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
                break;

            struct WindowSearch
            {
                DWORD processId;
                bool found;
            } search{ process.dwProcessId, false };
            EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL
            {
                auto* search = reinterpret_cast<WindowSearch*>(parameter);
                DWORD windowProcessId = 0;
                GetWindowThreadProcessId(hwnd, &windowProcessId);
                if (windowProcessId != search->processId || !IsWindowVisible(hwnd))
                    return TRUE;

                wchar_t title[128]{};
                GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
                if (std::wstring(title).rfind(L"Engine Editor", 0) == 0)
                {
                    search->found = true;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&search));
            editorReady = search.found;
            if (!editorReady)
                Sleep(100);
        }

        if (!editorReady && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
        {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return editorReady;
    };

    if (launch(L""))
        return true;

    // A fresh process avoids reusing partially initialized graphics state.
    return launch(L" --editor-renderer DirectX11");
}

bool CanDeleteProjectDirectory(const std::string& projectFile)
{
    std::error_code error;
    const fs::path file = fs::weakly_canonical(projectFile, error);
    if (error || file.extension() != ".proj" || !fs::is_regular_file(file))
        return false;

    const fs::path root = file.parent_path();
    const fs::path engineRoot = fs::weakly_canonical(fs::path(ENGINE_ROOT_PATH), error);
    if (error || root.empty() || root == root.root_path() || root == engineRoot)
        return false;

    // Generated projects use a dedicated folder matching the .proj name. This
    // prevents a misplaced .proj file from offering to delete a broad parent
    // directory such as Documents or Downloads.
    return root.filename() == file.stem() &&
        fs::is_regular_file(root / "CMakeLists.txt") &&
        fs::is_directory(root / "Assets");
}
}

void ProjectLauncher::RememberProject(const std::string& projectFile)
{
    if (projectFile.empty()) return;
    const std::string absolute = AbsoluteProjectPath(projectFile);
    auto projects = LoadRecentProjects();
    projects.erase(std::remove(projects.begin(), projects.end(), absolute), projects.end());
    projects.insert(projects.begin(), absolute);
    if (projects.size() > 12) projects.resize(12);
    SaveRecentProjects(projects);
}

std::string ProjectLauncher::Run(HINSTANCE instance)
{
    const std::string api = ChooseLauncherRenderer();
    if (api.empty())
    {
        MessageBoxA(nullptr, "No supported renderer is available for the project launcher.",
            "Engine Project Launcher", MB_OK | MB_ICONERROR);
        return {};
    }

    ProjectSettings settings{};
    settings.editorRenderingAPI = api;
    auto window = std::make_unique<Window>(instance, L"Engine Project Launcher", 960, 600);
    auto renderer = RendererFactory::CreateEditorRenderer(settings);
    if (!renderer || !renderer->Init(window->GetHWND(), window->GetWidth(), window->GetHeight()))
        return {};
    auto uiBackend = std::make_unique<ImGuiUiBackend>();
    if (!uiBackend->Initialize(window->GetHWND(), *renderer))
        return {};
    renderer->SetUiRenderHooks({
        [&]() { uiBackend->BeginFrame(); },
        [&](void* commands) { uiBackend->Render(commands); },
        [&]() { uiBackend->EndFrame(); }
    });

    std::vector<std::string> projects = LoadRecentProjects();
    int selected = projects.empty() ? -1 : 0;
    char projectName[128] = "NewGame";
    char location[MAX_PATH]{};
    const std::string userProfile = EnvironmentVariable("USERPROFILE");
    if (!userProfile.empty())
        strncpy_s(location, (fs::path(userProfile) / "Documents").string().c_str(), _TRUNCATE);
    std::string status;
    std::string result;
    std::string pendingDeleteProject;
    bool building = false;
    bool creatingProject = false;
    bool openSetupPopup = false;
    std::shared_ptr<ProjectSetupProgress> setupProgress;
    std::future<ProjectBuildResult> buildFuture;

    auto openProject = [&](const std::string& projectFile)
    {
        if (building || projectFile.empty()) return;
        RememberProject(projectFile);
        const fs::path root = fs::path(projectFile).parent_path();
        const fs::path executable = root / "build" / "Debug" / "Engine" / "Debug" / "Editor.exe";
        if (fs::exists(executable))
        {
            if (LaunchProjectEditor(executable.string(), projectFile))
            { result.clear(); PostQuitMessage(0); }
            else
                status = "The project editor did not finish startup. See " +
                    (root / "editor-startup.log").string();
            return;
        }

        building = true;
        creatingProject = false;
        setupProgress = std::make_shared<ProjectSetupProgress>();
        setupProgress->Set(0.10f, ProjectSetupStage::Configuring);
        openSetupPopup = true;
        status = "Configuring and building the project editor...";
        buildFuture = std::async(std::launch::async, BuildProjectEditor, projectFile, setupProgress);
    };

    auto removeSelectedFromRecent = [&]()
    {
        if (selected < 0 || selected >= static_cast<int>(projects.size()))
            return;
        const std::string removed = projects[selected];
        projects.erase(projects.begin() + selected);
        SaveRecentProjects(projects);
        selected = projects.empty() ? -1 : std::min(selected, static_cast<int>(projects.size()) - 1);
        status = "Removed from recent projects: " + removed;
    };

    window->WndProcHook = [&](HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        return uiBackend->HandleMessage(hwnd, message, wParam, lParam);
    };
    window->OnInputBegin = [&]() { uiBackend->BeginInput(); };
    window->OnInputEnd = [&]() { uiBackend->EndInput(); };
    window->OnResize = [&](uint32_t width, uint32_t height) {
        renderer->Resize(width, height);
        uiBackend->Resize(width, height);
    };
    window->OnUpdate = [&]()
    {
        if (building && buildFuture.valid() &&
            buildFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            ProjectBuildResult buildResult = buildFuture.get();
            building = false;
            status = buildResult.message;
            if (!buildResult.projectFile.empty())
                RememberProject(buildResult.projectFile);
            if (buildResult.success)
            {
                if (setupProgress) setupProgress->Set(1.f, ProjectSetupStage::Launching);
                if (LaunchProjectEditor(buildResult.editorExecutable, buildResult.projectFile))
                { result.clear(); PostQuitMessage(0); }
                else
                    status = "The editor built successfully but did not finish startup. See " +
                        (fs::path(buildResult.projectFile).parent_path() / "editor-startup.log").string();
            }
        }

        renderer->MarkDirty();
        renderer->RenderIfNeeded([&]()
        {
            renderer->Clear(0.075f, 0.085f, 0.11f, 1.0f);
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::Begin("Project Hub", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
            ImGui::SetCursorPos({ 36.f, 28.f });
            ImGui::BeginGroup();
            ImGui::Text("EngineProject");
            ImGui::TextDisabled("Open a recent project or create a new one");
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            ImGui::BeginGroup();
            ImGui::BeginChild("Recent", { 420.f, 390.f }, true);
            ImGui::Text("Recent Projects"); ImGui::Spacing();
            if (projects.empty()) ImGui::TextDisabled("No recent projects yet.");
            for (int i = 0; i < static_cast<int>(projects.size()); ++i)
            {
                const bool exists = fs::exists(projects[i]);
                ImGui::BeginDisabled(!exists);
                const fs::path path(projects[i]);
                std::string label = path.stem().string() + "\n" + path.parent_path().string();
                if (ImGui::Selectable(label.c_str(), selected == i, ImGuiSelectableFlags_AllowDoubleClick,
                    { 0.f, 48.f }))
                {
                    selected = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        openProject(projects[i]);
                }
                ImGui::EndDisabled();
            }
            ImGui::EndChild();
            ImGui::BeginDisabled(building);
            if (ImGui::Button("Open Selected", { 130.f, 32.f }) && selected >= 0 && fs::exists(projects[selected]))
                openProject(projects[selected]);
            ImGui::SameLine();
            if (ImGui::Button("Browse...", { 110.f, 32.f }))
            {
                std::string chosen = BrowseForProject(window->GetHWND());
                if (!chosen.empty()) openProject(chosen);
            }
            ImGui::Spacing();
            const bool hasSelection = selected >= 0 && selected < static_cast<int>(projects.size());
            const std::string selectedProject = hasSelection ? projects[selected] : std::string{};
            ImGui::BeginDisabled(!hasSelection);
            if (ImGui::Button("Remove from Recent", { 150.f, 32.f }))
                removeSelectedFromRecent();
            ImGui::EndDisabled();
            ImGui::SameLine();
            const bool canDelete = hasSelection && CanDeleteProjectDirectory(selectedProject);
            ImGui::BeginDisabled(!canDelete);
            if (ImGui::Button("Delete Project...", { 135.f, 32.f }))
            {
                pendingDeleteProject = selectedProject;
                ImGui::OpenPopup("Delete Project?");
            }
            ImGui::EndDisabled();
            if (hasSelection && !canDelete && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Permanent deletion is available only for generated project folders whose folder and .proj names match.");
            ImGui::EndDisabled();

            ImGui::EndGroup();
            ImGui::SameLine(); ImGui::BeginGroup();
            ImGui::BeginChild("Create", { 420.f, 390.f }, true);
            ImGui::Text("Create New Project"); ImGui::Spacing();
            ImGui::TextDisabled("Project Name"); ImGui::SetNextItemWidth(-1.f);
            ImGui::InputText("##ProjectName", projectName, sizeof(projectName));
            ImGui::Spacing(); ImGui::TextDisabled("Parent Directory");
            ImGui::SetNextItemWidth(-90.f); ImGui::InputText("##Location", location, sizeof(location));
            ImGui::SameLine();
            if (ImGui::Button("Browse##Folder"))
            {
                std::string folder = BrowseForFolder(window->GetHWND());
                if (!folder.empty()) strncpy_s(location, folder.c_str(), _TRUNCATE);
            }
            ImGui::Spacing();
            ImGui::TextWrapped("Starter scenes, meshes, and scripts are copied into the new project's Assets folder. The project references this engine installation.");
            if (!status.empty())
            { ImGui::Spacing(); ImGui::TextWrapped("%s", status.c_str()); }
            ImGui::EndChild();
            ImGui::BeginDisabled(building);
            if (ImGui::Button("Create Project", { 140.f, 32.f }))
            {
                const std::string requestedName = projectName;
                const std::string requestedLocation = location;
                building = true;
                creatingProject = true;
                openSetupPopup = true;
                setupProgress = std::make_shared<ProjectSetupProgress>();
                setupProgress->Set(0.01f, ProjectSetupStage::Validating);
                status = "Creating project...";
                buildFuture = std::async(std::launch::async,
                    [requestedName, requestedLocation, progress = setupProgress]()
                    {
                        ProjectBuildResult failure{};
                        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                        try
                        {
                            const std::string projectFile =
                                CreateProject(requestedName, requestedLocation, progress);
                            ProjectBuildResult result = BuildProjectEditor(projectFile, progress);
                            if (SUCCEEDED(comResult)) CoUninitialize();
                            return result;
                        }
                        catch (const std::exception& error)
                        {
                            if (SUCCEEDED(comResult)) CoUninitialize();
                            failure.message = error.what();
                            return failure;
                        }
                    });
            }
            ImGui::EndDisabled();
            ImGui::EndGroup();
            ImGui::EndGroup();

            if (openSetupPopup)
            {
                ImGui::OpenPopup("Project Setup");
                openSetupPopup = false;
            }
            ImGui::SetNextWindowSize({ 500.f, 0.f }, ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("Project Setup", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
            {
                ImGui::Text("%s", creatingProject ? "Creating New Project" : "Preparing Project Editor");
                ImGui::Spacing();
                const float progress = setupProgress ? setupProgress->value.load() : 0.f;
                const ProjectSetupStage stage = setupProgress
                    ? setupProgress->stage.load() : ProjectSetupStage::Validating;
                ImGui::TextWrapped("%s", SetupStageLabel(stage));
                ImGui::Spacing();
                ImGui::ProgressBar(progress, { 450.f, 22.f });
                ImGui::Spacing();
                ImGui::TextDisabled("The launcher will open the Editor automatically when setup finishes.");
                if (!building)
                {
                    ImGui::CloseCurrentPopup();
                    setupProgress.reset();
                    creatingProject = false;
                }
                ImGui::EndPopup();
            }

            ImGui::SetNextWindowSize({ 560.f, 0.f }, ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("Delete Project?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                const fs::path deleteRoot = fs::path(pendingDeleteProject).parent_path();
                ImGui::TextWrapped("Permanently delete this project and every file in its folder?");
                ImGui::Spacing();
                ImGui::TextWrapped("%s", deleteRoot.string().c_str());
                ImGui::Spacing();
                ImGui::TextColored({ 1.f, 0.45f, 0.35f, 1.f }, "This cannot be undone.");
                ImGui::Spacing();
                if (ImGui::Button("Delete Permanently", { 160.f, 32.f }))
                {
                    std::error_code deleteError;
                    fs::remove_all(deleteRoot, deleteError);
                    if (deleteError)
                        status = "Could not delete project: " + deleteError.message();
                    else
                    {
                        auto project = std::find(projects.begin(), projects.end(), pendingDeleteProject);
                        if (project != projects.end())
                            projects.erase(project);
                        SaveRecentProjects(projects);
                        selected = projects.empty() ? -1 : 0;
                        status = "Project deleted: " + deleteRoot.string();
                    }
                    pendingDeleteProject.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", { 100.f, 32.f }))
                {
                    pendingDeleteProject.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::End();
        });
    };

    window->Show();
    window->Run();
    uiBackend.reset();
    renderer.reset();
    window.reset();

    // Destroying the launcher window after its loop exits can enqueue another
    // WM_QUIT. Do not let that launcher-only message close the editor window
    // that is about to be created for the selected project.
    MSG message{};
    while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}

    if (!result.empty()) RememberProject(result);
    return result;
}
