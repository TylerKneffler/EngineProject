#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Engine::Editor
{
enum class EditorCommand
{
    Undo,
    Redo,
    SaveScene,
    SaveAll,
    Copy,
    Paste,
    Build,
    BuildAndPlay,
    BuildStandalone,
    PlayPause,
    Stop,
    Preferences,
    ImportAsset,
    BakeLighting,
    ClearBakedLighting,
    ViewportSelect,
    ViewportPan,
    ViewportOrbit,
    ViewportZoom
};

struct EditorKeyBinding
{
    std::string key;
    bool control = false;
    bool shift = false;
    bool alt = false;

    bool Empty() const { return key.empty(); }
};

struct EditorKeyBindingEntry
{
    EditorCommand command{};
    const char* id = "";
    const char* category = "";
    const char* label = "";
    std::vector<EditorKeyBinding> bindings;
};

class EditorKeyBindings
{
public:
    static EditorKeyBindings& Get();

    void Initialize(const std::string& projectFilePath);
    const std::vector<EditorKeyBindingEntry>& Entries() const { return m_entries; }
    std::vector<EditorKeyBindingEntry>& Entries() { return m_entries; }

    const EditorKeyBindingEntry* Find(EditorCommand command) const;
    EditorKeyBindingEntry* Find(EditorCommand command);
    bool Pressed(EditorCommand command) const;
    bool Down(EditorCommand command) const;
    float Wheel(EditorCommand command) const;

    std::string BindingLabel(const EditorKeyBinding& binding) const;
    std::string ShortcutLabel(EditorCommand command) const;
    std::size_t BindingColumnCount() const;
    void AddBindingColumn();
    bool RemoveBindingColumn(std::size_t column);

    bool Save();
    bool ResetToDefaults();
    const std::string& UserPath() const { return m_userPath; }
    const std::string& DefaultPath() const { return m_defaultPath; }
    const std::string& LastError() const { return m_lastError; }

    void SetCapturing(bool capturing) { m_capturing = capturing; }
    bool IsCapturing() const { return m_capturing; }

private:
    EditorKeyBindings();
    void SetCompiledDefaults();
    void NormalizeBindingColumns(std::size_t minimumColumns = 1);
    bool LoadFile(const std::string& path, bool required);
    bool MatchesPressed(const EditorKeyBinding& binding) const;
    bool MatchesDown(const EditorKeyBinding& binding) const;
    bool ModifiersMatch(const EditorKeyBinding& binding) const;

    std::vector<EditorKeyBindingEntry> m_entries;
    std::string m_defaultPath;
    std::string m_userPath;
    std::string m_lastError;
    bool m_initialized = false;
    bool m_capturing = false;
};
}
