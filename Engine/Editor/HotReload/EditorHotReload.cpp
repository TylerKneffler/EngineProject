#include "Engine/Editor/HotReload/EditorHotReload.h"
#include "Engine/Editor/EditorState.h"
#include "Core/Serialization/SceneSerializer.h"
#include <system_error>

namespace Engine::Editor
{
EditorHotReload::EditorHotReload(EditorState& editorState, ConsoleView* console,
    std::string scriptsDirectory, std::string buildDirectory,
    std::string modulePath)
    : m_editorState(editorState)
    , m_console(console)
    , m_buildDirectory(std::move(buildDirectory))
    , m_modulePath(std::move(modulePath))
{
    m_watcher.Initialize(std::move(scriptsDirectory));
}

EditorHotReload::~EditorHotReload()
{
    if (m_process)
    {
        TerminateProcess(m_process, 1);
        CloseHandle(m_process);
    }
    if (m_pipe) CloseHandle(m_pipe);
    // The scene can still contain objects whose virtual functions live in the
    // module. Windows will release it with the process after EditorState dies.
}

void EditorHotReload::Log(ConsoleView::Level level, const std::string& line)
{
    if (m_console) m_console->AddLog(level, line);
}

void EditorHotReload::Update(bool editorFocused)
{
    if (m_watcher.Poll())
        m_dirty = true;

    if (m_process)
    {
        DrainOutput();
        PollCompile();
    }
    else if (editorFocused && m_dirty)
    {
        StartCompile();
    }
}

void EditorHotReload::StartCompile()
{
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&m_pipe, &writePipe, &security, 0) ||
        !SetHandleInformation(m_pipe, HANDLE_FLAG_INHERIT, 0))
    {
        if (m_pipe) CloseHandle(m_pipe);
        if (writePipe) CloseHandle(writePipe);
        m_pipe = nullptr;
        Log(ConsoleView::Level::Error, "Could not start script compiler output capture.");
        return;
    }

    const std::string command = "cmake --build \"" + m_buildDirectory.string() +
        "\" --config Debug --target ProjectScripts --parallel 1";
    std::vector<char> buffer(command.begin(), command.end());
    buffer.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessA(nullptr, buffer.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    if (!started)
    {
        CloseHandle(m_pipe);
        m_pipe = nullptr;
        Log(ConsoleView::Level::Error, "Could not start script compiler.");
        return;
    }
    CloseHandle(process.hThread);
    m_process = process.hProcess;
    m_lineBuffer.clear();
    m_dirty = false;
    m_editorState.SetLoadingOverlay(true, "Compiling scripts...");
    Log(ConsoleView::Level::Build, "Compiling scripts...");
}

void EditorHotReload::DrainOutput()
{
    DWORD available = 0;
    while (m_pipe && PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &available, nullptr) && available)
    {
        char bytes[1024];
        DWORD read = 0;
        if (!ReadFile(m_pipe, bytes, std::min<DWORD>(available, sizeof(bytes)),
            &read, nullptr) || !read)
            break;
        for (DWORD index = 0; index < read; ++index)
        {
            if (bytes[index] == '\n')
            {
                if (!m_lineBuffer.empty() && m_lineBuffer.back() == '\r')
                    m_lineBuffer.pop_back();
                if (!m_lineBuffer.empty()) Log(ConsoleView::Level::Build, m_lineBuffer);
                m_lineBuffer.clear();
            }
            else m_lineBuffer.push_back(bytes[index]);
        }
    }
}

void EditorHotReload::PollCompile()
{
    if (!m_process || WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT)
        return;
    DrainOutput();
    DWORD exitCode = 1;
    GetExitCodeProcess(m_process, &exitCode);
    CloseHandle(m_process);
    m_process = nullptr;
    if (!m_lineBuffer.empty())
    {
        Log(ConsoleView::Level::Build, m_lineBuffer);
        m_lineBuffer.clear();
    }
    if (m_pipe) { CloseHandle(m_pipe); m_pipe = nullptr; }

    if (exitCode != 0)
    {
        Log(ConsoleView::Level::Error,
            "Script compilation failed; the last working scripts are still active.");
        m_editorState.SetLoadingOverlay(false);
        return;
    }

    m_editorState.SetLoadingOverlay(true, "Loading scripts...");
    m_applying = true;
    const bool applied = ApplyModule();
    m_applying = false;
    m_editorState.SetLoadingOverlay(false);
    Log(applied ? ConsoleView::Level::Info : ConsoleView::Level::Error,
        applied ? "Scripts compiled and loaded." :
        "Could not load compiled scripts; the last working scripts are still active.");
}

bool EditorHotReload::LoadModule(const std::filesystem::path& path,
    HMODULE& module, std::vector<std::string>& types,
    CreateScript& createFunction)
{
    module = LoadLibraryW(path.wstring().c_str());
    if (!module) return false;
    auto count = reinterpret_cast<ScriptTypeCount>(
        GetProcAddress(module, "EngineScriptTypeCount"));
    auto name = reinterpret_cast<ScriptTypeName>(
        GetProcAddress(module, "EngineScriptTypeName"));
    createFunction = reinterpret_cast<CreateScript>(
        GetProcAddress(module, "EngineCreateScript"));
    if (!count || !name || !createFunction)
    {
        FreeLibrary(module);
        module = nullptr;
        return false;
    }
    for (int index = 0; index < count(); ++index)
        if (const char* type = name(index); type && *type)
            types.emplace_back(type);
    return true;
}

bool EditorHotReload::ApplyModule()
{
    std::error_code error;
    if (!std::filesystem::exists(m_modulePath, error)) return false;
    const std::filesystem::path runtimeDirectory =
        m_buildDirectory / ".hotreload";
    std::filesystem::create_directories(runtimeDirectory, error);
    if (error) return false;
    const std::filesystem::path candidatePath = runtimeDirectory /
        ("ProjectScripts_" + std::to_string(++m_generation) + ".dll");
    if (!std::filesystem::copy_file(m_modulePath, candidatePath,
        std::filesystem::copy_options::overwrite_existing, error))
        return false;

    HMODULE candidate = nullptr;
    std::vector<std::string> candidateTypes;
    CreateScript candidateCreate = nullptr;
    if (!LoadModule(candidatePath, candidate, candidateTypes, candidateCreate))
        return false;

    if (BeforeApply) BeforeApply();
    Engine::Scene::Scene* scene = m_editorState.GetScene();
    if (!scene)
    {
        FreeLibrary(candidate);
        return false;
    }
    const std::string snapshot = scene->SaveToString();
    ::Engine::Scene::Scene::ObjectPath selectedPath;
    scene->TryGetObjectPath(scene->GetSelectedObject(), selectedPath);
    scene->ClearObjects();

    const HMODULE previousModule = m_module;
    const std::filesystem::path previousModulePath = m_loadedModulePath;
    const auto previousTypes = m_registeredTypes;
    std::vector<std::pair<std::string, Engine::Serialization::SceneSerializer::Factory>> previousFactories;
    std::vector<std::string> affectedTypes = previousTypes;
    for (const auto& type : candidateTypes)
        if (std::find(affectedTypes.begin(), affectedTypes.end(), type) ==
            affectedTypes.end())
            affectedTypes.push_back(type);
    previousFactories.reserve(affectedTypes.size());
    for (const auto& type : affectedTypes)
        previousFactories.emplace_back(type,
            Engine::Serialization::SceneSerializer::GetRegisteredFactory(type));
    for (const auto& type : previousTypes)
        Engine::Serialization::SceneSerializer::Unregister(type);
    for (const auto& type : candidateTypes)
        Engine::Serialization::SceneSerializer::Register(type,
            [candidateCreate, type]() { return candidateCreate(type.c_str()); });

    if (!scene->LoadFromString(snapshot))
    {
        scene->ClearObjects();
        for (const auto& type : candidateTypes) Engine::Serialization::SceneSerializer::Unregister(type);
        for (auto& [type, factory] : previousFactories)
            if (factory) Engine::Serialization::SceneSerializer::Register(type, std::move(factory));
        scene->LoadFromString(snapshot);
        m_editorState.RefreshSelectionAfterReload(selectedPath);
        FreeLibrary(candidate);
        return false;
    }

    m_editorState.RefreshSelectionAfterReload(selectedPath);
    m_module = candidate;
    m_loadedModulePath = candidatePath;
    m_registeredTypes = std::move(candidateTypes);
    m_createScript = candidateCreate;
    if (previousModule)
    {
        FreeLibrary(previousModule);
        std::filesystem::remove(previousModulePath, error);
    }
    return true;
}
}
