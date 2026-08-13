#pragma once
#include "pch.h"
#include "Engine/Editor/HotReload/ScriptFileWatcher.h"
#include "Engine/Editor/Core/View/Views/ConsoleView.h"
#include "Core/Scene/Scene.h"

namespace Engine::Editor
{
class EditorState;

class EditorHotReload
{
public:
    EditorHotReload(EditorState& editorState, ConsoleView* console,
        std::string scriptsDirectory, std::string buildDirectory,
        std::string modulePath);
    ~EditorHotReload();

    void Update(bool editorFocused);
    bool IsBusy() const { return m_process != nullptr || m_applying; }
    std::function<void()> BeforeApply;

private:
    using ScriptTypeCount = int (*)();
    using ScriptTypeName = const char* (*)(int);
    using CreateScript = Engine::Core::Component* (*)(const char*);

    void StartCompile();
    void DrainOutput();
    void PollCompile();
    bool ApplyModule();
    bool LoadModule(const std::filesystem::path& path, HMODULE& module,
        std::vector<std::string>& types, CreateScript& createFunction);
    void Log(ConsoleView::Level level, const std::string& line);

    EditorState& m_editorState;
    ConsoleView* m_console = nullptr;
    ScriptFileWatcher m_watcher;
    std::filesystem::path m_buildDirectory;
    std::filesystem::path m_modulePath;
    HANDLE m_process = nullptr;
    HANDLE m_pipe = nullptr;
    std::string m_lineBuffer;
    HMODULE m_module = nullptr;
    std::filesystem::path m_loadedModulePath;
    std::vector<std::string> m_registeredTypes;
    CreateScript m_createScript = nullptr;
    uint64_t m_generation = 0;
    bool m_dirty = false;
    bool m_wasFocused = true;
    bool m_applying = false;
};
}
