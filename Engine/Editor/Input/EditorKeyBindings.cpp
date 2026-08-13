#include "Engine/Editor/Input/EditorKeyBindings.h"

#include "Core/Serialization/Json.h"
#include "imgui.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

namespace Engine::Editor
{
namespace
{
using Engine::Serialization::JsonParseFile;
using Engine::Serialization::JsonValue;
using Engine::Serialization::JsonWrite;

EditorKeyBinding Key(const char* key, bool control = false,
    bool shift = false, bool alt = false)
{
    return { key, control, shift, alt };
}

ImGuiKey FindImGuiKey(const std::string& name)
{
    for (int value = ImGuiKey_NamedKey_BEGIN; value < ImGuiKey_NamedKey_END; ++value)
    {
        const ImGuiKey key = static_cast<ImGuiKey>(value);
        const char* candidate = ImGui::GetKeyName(key);
        if (candidate && name == candidate)
            return key;
    }
    return ImGuiKey_None;
}

bool IsMouseKey(const std::string& key)
{
    return key == "Mouse Left" || key == "Mouse Right" ||
        key == "Mouse Middle" || key == "Mouse X1" || key == "Mouse X2";
}

int MouseButton(const std::string& key)
{
    if (key == "Mouse Left") return ImGuiMouseButton_Left;
    if (key == "Mouse Right") return ImGuiMouseButton_Right;
    if (key == "Mouse Middle") return ImGuiMouseButton_Middle;
    if (key == "Mouse X1") return 3;
    if (key == "Mouse X2") return 4;
    return -1;
}

JsonValue SerializeBinding(const EditorKeyBinding& binding)
{
    return JsonValue::MakeObject()
        .Set("key", JsonValue(binding.key))
        .Set("control", JsonValue(binding.control))
        .Set("shift", JsonValue(binding.shift))
        .Set("alt", JsonValue(binding.alt));
}

EditorKeyBinding DeserializeBinding(const JsonValue& value)
{
    EditorKeyBinding binding;
    if (!value.IsObject()) return binding;
    if (value.Has("key") && value["key"].IsString())
        binding.key = value["key"].AsString();
    if (value.Has("control") && value["control"].IsBool())
        binding.control = value["control"].AsBool();
    if (value.Has("shift") && value["shift"].IsBool())
        binding.shift = value["shift"].AsBool();
    if (value.Has("alt") && value["alt"].IsBool())
        binding.alt = value["alt"].AsBool();
    return binding;
}
}

EditorKeyBindings& EditorKeyBindings::Get()
{
    static EditorKeyBindings instance;
    return instance;
}

EditorKeyBindings::EditorKeyBindings()
{
    SetCompiledDefaults();
}

void EditorKeyBindings::SetCompiledDefaults()
{
    m_entries = {
        { EditorCommand::Undo, "edit.undo", "Editing", "Undo", { Key("Z", true), Key("") } },
        { EditorCommand::Redo, "edit.redo", "Editing", "Redo", { Key("Y", true), Key("Z", true, true) } },
        { EditorCommand::SaveScene, "file.save", "File", "Save Scene", { Key("S", true), Key("") } },
        { EditorCommand::SaveAll, "file.save_all", "File", "Save All", { Key("S", true, true), Key("") } },
        { EditorCommand::Copy, "edit.copy", "Editing", "Copy", { Key("C", true), Key("") } },
        { EditorCommand::Paste, "edit.paste", "Editing", "Paste", { Key("V", true), Key("") } },
        { EditorCommand::Build, "build.build", "Build", "Build", { Key("B", true), Key("") } },
        { EditorCommand::BuildAndPlay, "build.play", "Build", "Build and Run in Editor", { Key("B", true, true), Key("") } },
        { EditorCommand::BuildStandalone, "build.standalone", "Build", "Build and Run Standalone", { Key("B", true, false, true), Key("") } },
        { EditorCommand::PlayPause, "play.toggle", "Play Mode", "Play / Pause / Resume", { Key("F5"), Key("F6") } },
        { EditorCommand::Stop, "play.stop", "Play Mode", "Stop", { Key("F5", false, true), Key("") } },
        { EditorCommand::Preferences, "window.preferences", "Window", "Project Preferences", { Key(",", true), Key("") } },
        { EditorCommand::ImportAsset, "file.import", "File", "Import Asset", { Key("I", true), Key("") } },
        { EditorCommand::BakeLighting, "lighting.bake", "Lighting", "Bake Lighting", { Key(""), Key("") } },
        { EditorCommand::ClearBakedLighting, "lighting.clear", "Lighting", "Clear Baked Lighting", { Key(""), Key("") } },
        { EditorCommand::ViewportSelect, "viewport.select", "Scene View", "Select Object", { Key("Mouse Left"), Key("") } },
        { EditorCommand::ViewportPan, "viewport.pan", "Scene View", "Pan Camera", { Key("Mouse Right"), Key("Mouse Middle", false, true), Key("Mouse Left", false, true, true) } },
        { EditorCommand::ViewportOrbit, "viewport.orbit", "Scene View", "Orbit Camera", { Key("Mouse Middle"), Key("Mouse Left", false, false, true) } },
        { EditorCommand::ViewportZoom, "viewport.zoom", "Scene View", "Zoom Camera", { Key("Mouse Wheel"), Key("Mouse Left", true, false, true) } }
    };
    NormalizeBindingColumns();
}

void EditorKeyBindings::NormalizeBindingColumns(std::size_t minimumColumns)
{
    std::size_t columns = std::max<std::size_t>(minimumColumns, 1);
    for (const auto& entry : m_entries)
        columns = std::max(columns, entry.bindings.size());
    for (auto& entry : m_entries)
        entry.bindings.resize(columns);
}

std::size_t EditorKeyBindings::BindingColumnCount() const
{
    return m_entries.empty() ? 1 :
        std::max<std::size_t>(1, m_entries.front().bindings.size());
}

void EditorKeyBindings::AddBindingColumn()
{
    for (auto& entry : m_entries)
        entry.bindings.emplace_back();
}

bool EditorKeyBindings::RemoveBindingColumn(std::size_t column)
{
    if (BindingColumnCount() <= 1 || column >= BindingColumnCount())
        return false;
    for (auto& entry : m_entries)
        if (column < entry.bindings.size())
            entry.bindings.erase(entry.bindings.begin() + column);
    return true;
}

void EditorKeyBindings::Initialize(const std::string& projectFilePath)
{
    namespace fs = std::filesystem;
    m_defaultPath = (fs::path(ENGINE_ASSETS_PATH) / "Editor" /
        "default-keybinds.json").lexically_normal().string();
    m_userPath = projectFilePath.empty()
        ? (fs::path(ENGINE_ASSETS_PATH) / "Editor" / "keybinds.json").lexically_normal().string()
        : (fs::path(projectFilePath).parent_path() / ".editor" /
            "keybinds.json").lexically_normal().string();

    SetCompiledDefaults();
    LoadFile(m_defaultPath, true);
    if (fs::is_regular_file(m_userPath))
        LoadFile(m_userPath, false);
    m_initialized = true;
}

const EditorKeyBindingEntry* EditorKeyBindings::Find(EditorCommand command) const
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
        [command](const auto& entry) { return entry.command == command; });
    return it == m_entries.end() ? nullptr : &*it;
}

EditorKeyBindingEntry* EditorKeyBindings::Find(EditorCommand command)
{
    return const_cast<EditorKeyBindingEntry*>(
        static_cast<const EditorKeyBindings*>(this)->Find(command));
}

bool EditorKeyBindings::ModifiersMatch(const EditorKeyBinding& binding) const
{
    const ImGuiIO& io = ImGui::GetIO();
    return io.KeyCtrl == binding.control &&
        io.KeyShift == binding.shift && io.KeyAlt == binding.alt;
}

bool EditorKeyBindings::MatchesPressed(const EditorKeyBinding& binding) const
{
    if (binding.Empty() || !ModifiersMatch(binding)) return false;
    if (IsMouseKey(binding.key))
    {
        const int button = MouseButton(binding.key);
        return button >= 0 && ImGui::IsMouseClicked(button, false);
    }
    if (binding.key == "Mouse Wheel")
        return ImGui::GetIO().MouseWheel != 0.f;
    const ImGuiKey key = FindImGuiKey(binding.key);
    return key != ImGuiKey_None && ImGui::IsKeyPressed(key, false);
}

bool EditorKeyBindings::MatchesDown(const EditorKeyBinding& binding) const
{
    if (binding.Empty() || !ModifiersMatch(binding)) return false;
    if (IsMouseKey(binding.key))
    {
        const int button = MouseButton(binding.key);
        return button >= 0 && ImGui::IsMouseDown(button);
    }
    const ImGuiKey key = FindImGuiKey(binding.key);
    return key != ImGuiKey_None && ImGui::IsKeyDown(key);
}

bool EditorKeyBindings::Pressed(EditorCommand command) const
{
    if (m_capturing || ImGui::GetIO().WantTextInput) return false;
    const auto* entry = Find(command);
    return entry && std::any_of(entry->bindings.begin(), entry->bindings.end(),
        [this](const EditorKeyBinding& binding)
        {
            return MatchesPressed(binding);
        });
}

bool EditorKeyBindings::Down(EditorCommand command) const
{
    if (m_capturing) return false;
    const auto* entry = Find(command);
    return entry && std::any_of(entry->bindings.begin(), entry->bindings.end(),
        [this](const EditorKeyBinding& binding)
        {
            return MatchesDown(binding);
        });
}

float EditorKeyBindings::Wheel(EditorCommand command) const
{
    if (m_capturing) return 0.f;
    const auto* entry = Find(command);
    if (!entry) return 0.f;
    for (const auto& binding : entry->bindings)
        if (binding.key == "Mouse Wheel" && ModifiersMatch(binding))
            return ImGui::GetIO().MouseWheel;
    return 0.f;
}

std::string EditorKeyBindings::BindingLabel(
    const EditorKeyBinding& binding) const
{
    if (binding.Empty()) return "Unassigned";
    std::string result;
    if (binding.control) result += "Ctrl+";
    if (binding.shift) result += "Shift+";
    if (binding.alt) result += "Alt+";
    result += binding.key;
    return result;
}

std::string EditorKeyBindings::ShortcutLabel(EditorCommand command) const
{
    const auto* entry = Find(command);
    if (!entry) return {};
    std::string result;
    for (const EditorKeyBinding& binding : entry->bindings)
    {
        if (binding.Empty()) continue;
        if (!result.empty()) result += " / ";
        result += BindingLabel(binding);
    }
    return result;
}

bool EditorKeyBindings::LoadFile(const std::string& path, bool required)
{
    try
    {
        const JsonValue root = JsonParseFile(path);
        const JsonValue& bindings = root["bindings"];
        if (!bindings.IsObject())
            throw std::runtime_error("missing bindings object");
        std::size_t fileColumns = 1;
        for (std::size_t index = 0; index < bindings.ObjectSize(); ++index)
            if (bindings.ObjectValue(index).IsArray())
                fileColumns = std::max(fileColumns,
                    bindings.ObjectValue(index).ArraySize());
        for (auto& entry : m_entries)
            entry.bindings.assign(fileColumns, {});
        for (std::size_t index = 0; index < bindings.ObjectSize(); ++index)
        {
            const std::string& id = bindings.ObjectKey(index);
            const JsonValue& slots = bindings.ObjectValue(index);
            auto it = std::find_if(m_entries.begin(), m_entries.end(),
                [&id](const auto& entry) { return id == entry.id; });
            if (it == m_entries.end() || !slots.IsArray()) continue;
            for (std::size_t slot = 0; slot < it->bindings.size() &&
                slot < slots.ArraySize(); ++slot)
                it->bindings[slot] = DeserializeBinding(slots.ArrayAt(slot));
        }
        NormalizeBindingColumns(fileColumns);
        m_lastError.clear();
        return true;
    }
    catch (const std::exception& error)
    {
        m_lastError = "Could not load keybinds '" + path + "': " + error.what();
        return !required;
    }
}

bool EditorKeyBindings::Save()
{
    try
    {
        JsonValue bindings = JsonValue::MakeObject();
        for (const auto& entry : m_entries)
        {
            JsonValue slots = JsonValue::MakeArray();
            for (const auto& binding : entry.bindings)
                slots.Push(SerializeBinding(binding));
            bindings.Set(entry.id, std::move(slots));
        }
        JsonValue root = JsonValue::MakeObject()
            .Set("version", JsonValue(1))
            .Set("bindings", std::move(bindings));
        const std::filesystem::path output(m_userPath);
        if (!output.parent_path().empty())
            std::filesystem::create_directories(output.parent_path());
        std::ofstream file(output, std::ios::trunc);
        if (!file) throw std::runtime_error("could not open output file");
        file << JsonWrite(root) << '\n';
        if (!file) throw std::runtime_error("could not write output file");
        m_lastError.clear();
        return true;
    }
    catch (const std::exception& error)
    {
        m_lastError = "Could not save keybinds '" + m_userPath + "': " + error.what();
        return false;
    }
}

bool EditorKeyBindings::ResetToDefaults()
{
    SetCompiledDefaults();
    const bool loaded = LoadFile(m_defaultPath, true);
    return loaded;
}
}
