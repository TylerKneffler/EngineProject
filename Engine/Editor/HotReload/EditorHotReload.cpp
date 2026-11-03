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
    if (m_staging.valid()) m_staging.wait();
    if (m_moduleJobs) m_moduleJobs->CloseAndWait();
    if (m_setJobHost) m_setJobHost(nullptr);
    if (m_process)
    {
        if (m_processJob) TerminateJobObject(m_processJob, 1);
        else TerminateProcess(m_process, 1);
        CloseHandle(m_process);
    }
    if (m_processJob) CloseHandle(m_processJob);
    if (m_pipe) CloseHandle(m_pipe);
    m_buildTreeLock.Release();
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
    PollStaging();
    if (!m_process && !m_staging.valid() && m_stagedModule.empty() &&
        editorFocused && m_dirty)
    {
        StartCompile();
    }
}

void EditorHotReload::StartCompile()
{
    if (!m_buildTreeLock.TryAcquire(m_buildDirectory))
    {
        if (!m_waitingForBuildTree)
            Log(ConsoleView::Level::Build,
                "Script changes queued until the current game build finishes.");
        m_waitingForBuildTree = true;
        return;
    }
    m_waitingForBuildTree = false;

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
        m_buildTreeLock.Release();
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
        TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
    if (!started)
    {
        CloseHandle(writePipe);
        CloseHandle(m_pipe);
        m_pipe = nullptr;
        m_buildTreeLock.Release();
        Log(ConsoleView::Level::Error, "Could not start script compiler.");
        return;
    }

    m_processJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!m_processJob || !SetInformationJobObject(m_processJob,
        JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(m_processJob, process.hProcess))
    {
        TerminateProcess(process.hProcess, 1);
        if (m_processJob) CloseHandle(m_processJob);
        m_processJob = nullptr;
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        CloseHandle(writePipe);
        CloseHandle(m_pipe);
        m_pipe = nullptr;
        m_buildTreeLock.Release();
        Log(ConsoleView::Level::Error, "Could not isolate the script compiler process.");
        return;
    }
    ResumeThread(process.hThread);
    CloseHandle(writePipe);
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
    // Reading and formatting an unbounded build log can stall a frame.
    constexpr DWORD budget = 16 * 1024;
    constexpr DWORD lineBudget = 128;
    DWORD consumed = 0;
    DWORD lines = 0;
    while (consumed < budget && lines < lineBudget && m_pipe &&
        PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &available, nullptr) && available)
    {
        char bytes[256];
        DWORD read = 0;
        if (!ReadFile(m_pipe, bytes, std::min<DWORD>({available,
            static_cast<DWORD>(sizeof(bytes)), budget - consumed}),
            &read, nullptr) || !read)
            break;
        consumed += read;
        for (DWORD index = 0; index < read; ++index)
        {
            if (bytes[index] == '\n')
            {
                ++lines;
                if (!m_lineBuffer.empty() && m_lineBuffer.back() == '\r')
                    m_lineBuffer.pop_back();
                if (!m_lineBuffer.empty()) Log(ConsoleView::Level::Build, m_lineBuffer);
                m_lineBuffer.clear();
            }
            else if (m_lineBuffer.size() < 4096) m_lineBuffer.push_back(bytes[index]);
        }
    }
}

void EditorHotReload::PollCompile()
{
    if (!m_process || WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT)
        return;
    DWORD available = 0;
    if (m_pipe && PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &available, nullptr) && available)
        return;
    DWORD exitCode = 1;
    GetExitCodeProcess(m_process, &exitCode);
    CloseHandle(m_process);
    m_process = nullptr;
    if (m_processJob)
    {
        CloseHandle(m_processJob);
        m_processJob = nullptr;
    }
    if (!m_lineBuffer.empty())
    {
        Log(ConsoleView::Level::Build, m_lineBuffer);
        m_lineBuffer.clear();
    }
    if (m_pipe) { CloseHandle(m_pipe); m_pipe = nullptr; }

    if (exitCode != 0)
    {
        m_buildTreeLock.Release();
        Log(ConsoleView::Level::Error,
            "Script compilation failed; the last working scripts are still active.");
        m_editorState.SetLoadingOverlay(false);
        m_editorState.CheckAssetLocations();
        return;
    }

    m_editorState.SetLoadingOverlay(true, "Preparing scripts...");
    const auto source = m_modulePath;
    const auto runtimeDirectory = m_buildDirectory / ".hotreload";
    const auto generation = ++m_generation;
    m_staging = std::async(std::launch::async,
        [source, runtimeDirectory, generation]
        {
            StagedModule result;
            std::error_code error;
            if (!std::filesystem::exists(source, error)) return result;
            std::filesystem::create_directories(runtimeDirectory, error);
            if (error) return result;
            const auto directory = runtimeDirectory /
                ("ProjectScripts_" + std::to_string(generation));
            std::filesystem::create_directories(directory, error);
            if (error) return result;
            const auto candidate = directory / source.filename();
            if (!std::filesystem::copy_file(source, candidate,
                std::filesystem::copy_options::overwrite_existing, error))
                return result;
            auto symbols = source;
            symbols.replace_extension(".pdb");
            error.clear();
            if (std::filesystem::exists(symbols, error))
            {
                error.clear();
                std::filesystem::copy_file(symbols, directory / symbols.filename(),
                    std::filesystem::copy_options::overwrite_existing, error);
                if (error) result.symbolsError = error.message();
            }
            result.path = candidate;
            return result;
        });
}

void EditorHotReload::PollStaging()
{
    if (!m_staging.valid() ||
        m_staging.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    const auto staged = m_staging.get();
    m_buildTreeLock.Release();
    if (!staged.symbolsError.empty())
        Log(ConsoleView::Level::Error,
            "Could not copy script debug symbols: " + staged.symbolsError);
    if (staged.path.empty())
    {
        m_editorState.SetLoadingOverlay(false);
        Log(ConsoleView::Level::Error,
            "Could not prepare compiled scripts; the last working scripts are still active.");
        m_editorState.CheckAssetLocations();
        return;
    }
    m_stagedModule = staged.path;
    m_editorState.SetLoadingOverlay(true, "Loading scripts...");
}

bool EditorHotReload::PublishPending()
{
    if (m_stagedModule.empty()) return false;
    m_applying = true;
    const bool applied = ApplyModule(m_stagedModule);
    m_applying = false;
    m_stagedModule.clear();
    m_editorState.SetLoadingOverlay(false);
    Log(applied ? ConsoleView::Level::Info : ConsoleView::Level::Error,
        applied ? "Scripts compiled and loaded." :
        "Could not load compiled scripts; the last working scripts are still active.");
    m_editorState.CheckAssetLocations();
    return true;
}

bool EditorHotReload::LoadModule(const std::filesystem::path& path,
    HMODULE& module, std::vector<std::string>& types,
    CreateScript& createFunction, SetJobHost& setJobHost,
    Engine::Threading::IScriptModuleJobs* jobs)
{
    module = LoadLibraryW(path.wstring().c_str());
    if (!module) return false;
    auto count = reinterpret_cast<ScriptTypeCount>(
        GetProcAddress(module, "EngineScriptTypeCount"));
    auto name = reinterpret_cast<ScriptTypeName>(
        GetProcAddress(module, "EngineScriptTypeName"));
    createFunction = reinterpret_cast<CreateScript>(
        GetProcAddress(module, "EngineCreateScript"));
    setJobHost = reinterpret_cast<SetJobHost>(
        GetProcAddress(module, "EngineSetScriptJobHost"));
    if (!count || !name || !createFunction || !setJobHost)
    {
        FreeLibrary(module);
        module = nullptr;
        return false;
    }
    setJobHost(jobs);
    for (int index = 0; index < count(); ++index)
        if (const char* type = name(index); type && *type)
            types.emplace_back(type);
    return true;
}

bool EditorHotReload::ApplyModule(const std::filesystem::path& candidatePath)
{
    std::error_code error;
    std::filesystem::path symbolsPath = m_modulePath;
    symbolsPath.replace_extension(".pdb");

    HMODULE candidate = nullptr;
    std::vector<std::string> candidateTypes;
    CreateScript candidateCreate = nullptr;
    SetJobHost candidateSetJobHost = nullptr;
    auto candidateJobs = std::make_unique<Engine::Threading::ScriptModuleJobs>();
    if (!LoadModule(candidatePath, candidate, candidateTypes, candidateCreate,
        candidateSetJobHost, candidateJobs.get()))
        return false;

    if (BeforeApply) BeforeApply();
    Engine::Scene::Scene* scene = m_editorState.GetScene();
    if (!scene)
    {
        candidateJobs->CloseAndWait();
        candidateSetJobHost(nullptr);
        FreeLibrary(candidate);
        return false;
    }
    if (m_moduleJobs) m_moduleJobs->CloseAndWait();
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
        candidateJobs->CloseAndWait();
        candidateSetJobHost(nullptr);
        for (const auto& type : candidateTypes) Engine::Serialization::SceneSerializer::Unregister(type);
        for (auto& [type, factory] : previousFactories)
            if (factory) Engine::Serialization::SceneSerializer::Register(type, std::move(factory));
        m_moduleJobs = std::make_unique<Engine::Threading::ScriptModuleJobs>();
        if (m_setJobHost) m_setJobHost(m_moduleJobs.get());
        scene->LoadFromString(snapshot);
        m_editorState.RefreshSelectionAfterReload(selectedPath);
        FreeLibrary(candidate);
        return false;
    }

    m_editorState.RefreshSelectionAfterReload(selectedPath);
    if (m_setJobHost) m_setJobHost(nullptr);
    m_module = candidate;
    m_loadedModulePath = candidatePath;
    m_registeredTypes = std::move(candidateTypes);
    m_createScript = candidateCreate;
    m_setJobHost = candidateSetJobHost;
    m_moduleJobs = std::move(candidateJobs);
    previousFactories.clear();
    if (previousModule)
    {
        FreeLibrary(previousModule);
        std::filesystem::remove(previousModulePath, error);
        std::filesystem::remove(previousModulePath.parent_path() /
            symbolsPath.filename(), error);
        std::filesystem::remove(previousModulePath.parent_path(), error);
    }
    return true;
}
}
