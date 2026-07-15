#include "pch.h"
#include "Engine/Editor/ProjectLauncher.h"
#include "Core/Window.h"
#include "Core/ProjectLoader.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Renderers/IEditorRenderer.h"
#include <commdlg.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstdlib>
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

std::string CreateProject(const std::string& name, const std::string& location)
{
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
        fs::create_directories(projectRoot / ".vscode");
        fs::copy(fs::path(ENGINE_ASSETS_PATH), projectRoot / "Assets",
            fs::copy_options::recursive);
        WriteTemplate(templateRoot / "CMakeLists.txt.in", projectRoot / "CMakeLists.txt", name);
        WriteTemplate(templateRoot / "Project.proj.in", projectRoot / (name + ".proj"), name);
        WriteTemplate(templateRoot / "tasks.json.in", projectRoot / ".vscode" / "tasks.json", name);
        WriteTemplate(templateRoot / "gitignore.in", projectRoot / ".gitignore", name);
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

    std::vector<std::string> projects = LoadRecentProjects();
    int selected = projects.empty() ? -1 : 0;
    char projectName[128] = "NewGame";
    char location[MAX_PATH]{};
    const std::string userProfile = EnvironmentVariable("USERPROFILE");
    if (!userProfile.empty())
        strncpy_s(location, (fs::path(userProfile) / "Documents").string().c_str(), _TRUNCATE);
    std::string status;
    std::string result;

    window->WndProcHook = [](HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        return ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam) != 0;
    };
    window->OnResize = [&](uint32_t width, uint32_t height) { renderer->Resize(width, height); };
    window->OnUpdate = [&]()
    {
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
                    { result = projects[i]; PostQuitMessage(0); }
                }
                ImGui::EndDisabled();
            }
            ImGui::EndChild();
            if (ImGui::Button("Open Selected", { 130.f, 32.f }) && selected >= 0 && fs::exists(projects[selected]))
            { result = projects[selected]; PostQuitMessage(0); }
            ImGui::SameLine();
            if (ImGui::Button("Browse...", { 110.f, 32.f }))
            {
                std::string chosen = BrowseForProject(window->GetHWND());
                if (!chosen.empty()) { result = chosen; PostQuitMessage(0); }
            }

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
            if (ImGui::Button("Create Project", { 140.f, 32.f }))
            {
                try { result = CreateProject(projectName, location); RememberProject(result); PostQuitMessage(0); }
                catch (const std::exception& error) { status = error.what(); }
            }
            ImGui::EndGroup();
            ImGui::EndGroup();
            ImGui::End();
        });
    };

    window->Show();
    window->Run();
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
