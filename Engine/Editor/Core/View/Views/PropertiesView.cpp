#include "PropertiesView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Sprite.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Component.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <shellapi.h>
#include <sstream>

namespace
{
std::string LowerExtension(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

bool IsTextureExtension(const std::string& extension)
{
    static const char* extensions[] = {
        ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".dds", ".hdr"
    };
    return std::find_if(std::begin(extensions), std::end(extensions),
        [&extension](const char* candidate) { return extension == candidate; }) !=
        std::end(extensions);
}

bool ContainsIgnoringCase(const std::string& value, const std::string& search)
{
    if (search.empty())
        return true;
    std::string loweredValue = value;
    std::string loweredSearch = search;
    std::transform(loweredValue.begin(), loweredValue.end(), loweredValue.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    std::transform(loweredSearch.begin(), loweredSearch.end(), loweredSearch.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return loweredValue.find(loweredSearch) != std::string::npos;
}

std::string FindComponentSubclass(const std::string& path)
{
    std::ifstream input(path);
    if (!input)
        return {};
    std::ostringstream contents;
    contents << input.rdbuf();

    // Capture each class/struct declaration and inspect its base list. A class
    // must inherit Component publicly; structs may use their implicit public
    // inheritance as well.
    const std::regex declaration(
        R"(\b(class|struct)\s+([A-Za-z_]\w*)\s*(?:final\s*)?:\s*([^\{]+)\{)");
    const std::regex publicComponent(
        R"((?:^|,)\s*public\s+(?:[A-Za-z_]\w*::)*(?:Component|Script)\b)");
    const std::regex componentBase(
        R"((?:^|,)\s*(?:[A-Za-z_]\w*::)*(?:Component|Script)\b)");
    const std::string source = contents.str();
    const std::string preferred = std::filesystem::path(path).stem().string();
    std::string firstMatch;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), declaration);
        it != std::sregex_iterator(); ++it)
    {
        const bool isStruct = (*it)[1].str() == "struct";
        const std::string className = (*it)[2].str();
        const std::string bases = (*it)[3].str();
        if (!std::regex_search(bases, isStruct ? componentBase : publicComponent))
            continue;
        if (className == preferred)
            return className;
        if (firstMatch.empty())
            firstMatch = className;
    }
    return firstMatch;
}

template<typename T>
void ReplaceOrAddComponent(Object& object, T* component)
{
    component->Owner = &object;
    for (auto it = object.Components.begin(); it != object.Components.end(); ++it)
    {
        if (dynamic_cast<T*>(*it))
        {
            delete *it;
            *it = component;
            return;
        }
    }
    object.Components.push_back(component);
}

void RevealFileInExplorer(const std::string& path)
{
    if (path.empty())
        return;
    std::error_code error;
    const std::filesystem::path absolutePath =
        std::filesystem::absolute(path, error);
    const std::filesystem::path selectedPath = error
        ? std::filesystem::path(path) : absolutePath;
    const std::wstring parameters =
        L"/select,\"" + selectedPath.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(),
        nullptr, SW_SHOWNORMAL);
}
}

void PropertiesView::DrawPanel(IEditorUi& ui)
{
    if (m_skyboxRevealPending &&
        std::chrono::steady_clock::now() - m_skyboxRevealRequestedAt >=
            std::chrono::milliseconds(GetDoubleClickTime()))
    {
        RevealFileInExplorer(m_skyboxRevealPath);
        m_skyboxRevealPending = false;
        m_skyboxRevealPath.clear();
    }
    if (!ui.BeginWindow(m_title.c_str(), &m_open))
    {
        ui.EndWindow();
        return;
    }
    ui.BeginTextWrap();
    if (!m_selectedObject)
    {
        ui.Label("Scene");
        ui.Separator();
        if (!m_scene)
            ui.DisabledLabel("No scene loaded");
        else
        {
            ui.Label("Skybox Texture Override");
            if (!m_editingSkyboxTexture)
            {
                const Texture* skybox = m_scene->GetSkyboxPreviewTexture();
                void* textureHandle = skybox && skybox->GetGraphicsTexture()
                    ? skybox->GetGraphicsTexture()->GetNativeHandle()
                    : nullptr;
                if (textureHandle)
                {
                    constexpr float previewWidth = 160.f;
                    const float aspect = skybox->GetHeight() > 0
                        ? static_cast<float>(skybox->GetWidth()) /
                            static_cast<float>(skybox->GetHeight())
                        : 2.f;
                    ui.DrawImage(textureHandle,
                        previewWidth, previewWidth / std::max(aspect, 0.1f));
                }
                else
                {
                    ui.DisabledLabel("[Skybox preview unavailable]");
                }

                const bool previewDoubleClicked = ui.IsItemDoubleClicked();
                const bool previewClicked = ui.IsItemClicked();
                if (previewDoubleClicked)
                {
                    m_skyboxRevealPending = false;
                    m_skyboxRevealPath.clear();
                    strncpy_s(m_skyboxTextureEdit,
                        m_scene->settings.skyboxTexture.c_str(),
                        sizeof(m_skyboxTextureEdit));
                    m_editingSkyboxTexture = true;
                }
                else if (previewClicked && skybox)
                {
                    m_skyboxRevealPending = true;
                    m_skyboxRevealRequestedAt =
                        std::chrono::steady_clock::now();
                    m_skyboxRevealPath = skybox->GetFilePath();
                }
            }
            else
            {
                ui.InputText("##sceneSkyboxTexture",
                    m_skyboxTextureEdit, sizeof(m_skyboxTextureEdit));
                if (ui.Button("Apply"))
                {
                    m_scene->settings.skyboxTexture = m_skyboxTextureEdit;
                    m_editingSkyboxTexture = false;
                    if (OnComponentsChanged)
                        OnComponentsChanged();
                }
                ui.SameLine();
                if (ui.Button("Cancel"))
                    m_editingSkyboxTexture = false;
            }
        }
        ui.EndTextWrap();
        ui.EndWindow();
        return;
    }
    const std::string assetDropPreview = HandleWindowAssetDrop(ui);
    Object* prefabRoot = m_selectedObject->GetPrefabInstanceRoot();
    const bool linked = prefabRoot != nullptr;
    char name[256]; strncpy_s(name, m_selectedObject->name.c_str(), sizeof(name));
    bool enabled = m_selectedObject->enabled;
    const EditorUiObjectRowResult header = ui.ObjectHeader(
        m_selectedObject, name, sizeof(name), &enabled, linked);
    if (header.nameChanged) m_selectedObject->name = name;
    if (header.enabledChanged)
        enabled ? m_selectedObject->Enabled() : m_selectedObject->Disabled();
    if (prefabRoot && prefabRoot->Prefab)
    {
        ui.ValueLabel("Prefab", prefabRoot->Prefab->GetPath().c_str());
        ui.DisabledLabel(
            m_selectedObject == prefabRoot
                ? "Properties come from the prefab; root transform is instance placement."
                : "Properties and transform come from the prefab asset.");
        ui.DisabledLabel(
            "Right-click it in the Hierarchy, or a component header, and choose Unpack Prefab.");
    }
    else
        ui.DisabledLabel("Scene-only object");
    
    ui.Separator();
    
    // Draw Transform (always present, not in Components list)
    DrawTransform(ui);
    
    // Draw all components with accordion views
    Component* componentToDelete = nullptr;
    Component* reorderSource = nullptr;
    Component* reorderTarget = nullptr;
    for (Component* component : m_selectedObject->Components)
    {
        if (!component) continue;
        ui.PushId(component);
        
        std::string componentType = component->GetTypeName();
        if (componentType.empty()) componentType = "Component";
        
        const bool componentOpen = ui.CollapsingHeader(componentType.c_str());
        Object* componentPrefabRoot = m_selectedObject->GetPrefabInstanceRoot();
        const bool componentEditable = componentPrefabRoot == nullptr;
        // Bind the menu to the header before drag/drop helpers replace the
        // UI backend's current item.
        const EditorUiContextMenuResult menu = ui.ContextMenu(component,
            componentEditable ? "Add Component" : nullptr,
            componentEditable ? "Delete Component" : nullptr,
            false,
            componentPrefabRoot ? "Unpack Prefab" : nullptr);
        if (menu.addRequested)
        {
            m_componentPickerOpen = true;
            m_positionComponentPicker = true;
            m_componentSearch[0] = '\0';
        }
        if (menu.deleteRequested)
            componentToDelete = component;
        if (menu.unpackRequested)
            UnpackSelectedPrefab();
        if (componentEditable && ui.BeginDragDropSource())
        {
            Component* payload = component;
            ui.SetDragDropPayload(
                "ENGINE_COMPONENT_REORDER", &payload, sizeof(payload));
            ui.Label(componentType.c_str());
            ui.EndDragDropSource();
        }
        if (componentEditable && ui.BeginDragDropTarget())
        {
            size_t payloadSize = 0;
            const void* payload = ui.AcceptDragDropPayload(
                "ENGINE_COMPONENT_REORDER", &payloadSize);
            if (payload && payloadSize == sizeof(Component*))
            {
                reorderSource = *static_cast<Component* const*>(payload);
                reorderTarget = component;
            }
            ui.EndDragDropTarget();
        }
        if (componentOpen)
        {
            const bool locked = m_selectedObject->IsPartOfPrefabInstance();
            ui.BeginDisabled(locked);
            component->DrawProperties(ui);
            ui.EndDisabled();
        }
        ui.PopId();
    }

    if (reorderSource && reorderTarget && reorderSource != reorderTarget)
    {
        auto& components = m_selectedObject->Components;
        const auto source = std::find(
            components.begin(), components.end(), reorderSource);
        const auto target = std::find(
            components.begin(), components.end(), reorderTarget);
        if (source != components.end() && target != components.end())
        {
            bool sourceBeforeTarget = false;
            for (auto current = components.begin(); current != components.end(); ++current)
            {
                if (current == source)
                {
                    sourceBeforeTarget = true;
                    break;
                }
                if (current == target)
                    break;
            }
            const auto destination = sourceBeforeTarget
                ? std::next(target)
                : target;
            components.splice(destination, components, source);
            LogAssetDrop("[Properties] Moved component '" +
                reorderSource->GetTypeName() + "' " +
                (sourceBeforeTarget ? "down" : "up"));
            if (OnComponentsChanged)
                OnComponentsChanged();
        }
    }

    if (componentToDelete)
    {
        const std::string deletedType = componentToDelete->GetTypeName();
        m_selectedObject->Components.remove(componentToDelete);
        delete componentToDelete;
        LogAssetDrop("[Properties] Deleted component '" + deletedType + "'");
        if (OnComponentsChanged)
            OnComponentsChanged();
    }

    if (!assetDropPreview.empty())
    {
        ui.PushId("assetDropPreview");
        ui.BeginDisabled();
        const std::string label = assetDropPreview + " (Drop to Add)";
        ui.CollapsingHeader(label.c_str(), false);
        ui.EndDisabled();
        ui.PopId();
    }

    ui.Separator();
    if (ui.Button("Add Component"))
    {
        m_componentPickerOpen = true;
        m_positionComponentPicker = true;
        m_componentSearch[0] = '\0';
    }
    
    ui.EndTextWrap();
    ui.EndWindow();
    DrawComponentPicker(ui);
}

std::string PropertiesView::HandleWindowAssetDrop(IEditorUi& ui)
{
    const EditorUiDragDropPayloadResult payload =
        ui.WindowDragDropTarget("ENGINE_ASSET_PATH");
    if (!payload.data || payload.size == 0)
        return {};

    const char* bytes = static_cast<const char*>(payload.data);
    size_t length = 0;
    while (length < payload.size && bytes[length] != '\0')
        ++length;
    const std::string path(bytes, length);
    if (path.empty())
        return {};

    if (payload.delivered)
    {
        std::string message;
        const bool added = AddComponentFromAsset(path, message);
        LogAssetDrop(message, !added);
        if (added && OnComponentsChanged)
            OnComponentsChanged();
        return {};
    }

    const std::string extension = LowerExtension(path);
    if (extension == ".mesh" || extension == ".obj")
        return "Mesh Preview";
    if (extension == ".material" || extension == ".mat" ||
        IsTextureExtension(extension))
        return "Material Preview";
    if (extension == ".spritesheet")
        return "Sprite Preview";
    if (extension == ".h" || extension == ".hpp")
    {
        const std::string className = FindComponentSubclass(path);
        return (className.empty()
            ? std::filesystem::path(path).stem().string()
            : className) + " Preview";
    }
    return std::filesystem::path(path).filename().string() + " Preview";
}

bool PropertiesView::AddComponentFromAsset(const std::string& path, std::string& message)
{
    if (!m_selectedObject)
    {
        message = "[Properties] Select an object before dropping an asset.";
        return false;
    }
    if (m_selectedObject->IsPartOfPrefabInstance())
    {
        message = "[Properties] Components on linked prefab objects are read-only.";
        return false;
    }
    if (path.empty() || !std::filesystem::is_regular_file(path))
    {
        message = "[Properties] Dropped asset does not exist: " + path;
        return false;
    }

    const std::string extension = LowerExtension(path);
    try
    {
        if (extension == ".mesh" || extension == ".obj")
        {
            auto mesh = std::make_unique<Mesh>();
            mesh->LoadFromFile(path);
            mesh->OnAfterDeserialize(m_scene ? m_scene->GetGraphicsProvider() : nullptr);
            ReplaceOrAddComponent(*m_selectedObject, mesh.release());
            message = "[Properties] Assigned Mesh from: " + path;
            return true;
        }

        if (extension == ".material" || extension == ".mat")
        {
            auto material = std::make_unique<Material>();
            if (!material->LoadFromFile(path))
            {
                message = "[Properties] Failed to read material asset: " + path;
                return false;
            }
            material->PrepareTextures(m_scene ? m_scene->GetGraphicsProvider() : nullptr);
            ReplaceOrAddComponent(*m_selectedObject, material.release());
            message = "[Properties] Assigned Material from: " + path;
            return true;
        }

        if (extension == ".spritesheet")
        {
            auto sprite = std::make_unique<Sprite>();
            if (!sprite->LoadFromFile(path))
            {
                message = "[Properties] Failed to read spritesheet asset: " + path;
                return false;
            }
            sprite->OnAfterDeserialize(
                m_scene ? m_scene->GetGraphicsProvider() : nullptr);
            ReplaceOrAddComponent(*m_selectedObject, sprite.release());
            message = "[Properties] Assigned Sprite from: " + path;
            return true;
        }

        if (IsTextureExtension(extension))
        {
            Material* material = m_selectedObject->GetComponent<Material>();
            if (!material)
                material = m_selectedObject->AddComponent<Material>();
            material->SetBaseColorTexture(path);
            material->PrepareTextures(m_scene ? m_scene->GetGraphicsProvider() : nullptr);
            message = "[Properties] Assigned texture as Material base color: " + path;
            return true;
        }

        if (extension == ".h" || extension == ".hpp")
        {
            const std::string className = FindComponentSubclass(path);
            if (className.empty())
            {
                message = "[Properties] Header does not declare a public Component/Script subclass: " + path;
                return false;
            }

            if (!AddRegisteredComponent(className, message))
                return false;
            message += " from: " + path;
            return true;
        }
    }
    catch (const std::exception& error)
    {
        message = "[Properties] Could not apply asset '" + path + "': " + error.what();
        return false;
    }

    message = "[Properties] No compatible component mapping for asset: " + path;
    return false;
}

bool PropertiesView::AddRegisteredComponent(const std::string& typeName, std::string& message)
{
    if (!m_selectedObject)
    {
        message = "[Properties] Select an object before adding a component.";
        return false;
    }
    if (m_selectedObject->IsPartOfPrefabInstance())
    {
        message = "[Properties] Components on linked prefab objects are read-only.";
        return false;
    }

    try
    {
        std::unique_ptr<Component> component(
            SceneSerializer::CreateRegisteredComponent(typeName));
        if (!component)
        {
            message = "[Properties] Component '" + typeName +
                "' is not compiled and registered; rebuild/register it before adding it.";
            return false;
        }
        if (component->singlecomponent)
        {
            const std::string serializedType = component->GetTypeName();
            const auto duplicate = std::find_if(m_selectedObject->Components.begin(),
                m_selectedObject->Components.end(), [&serializedType](const Component* existing)
                {
                    return existing && existing->GetTypeName() == serializedType;
                });
            if (duplicate != m_selectedObject->Components.end())
            {
                message = "[Properties] Object already has the single-instance component '" +
                    serializedType + "'.";
                return false;
            }
        }
        component->Owner = m_selectedObject;
        component->OnAfterDeserialize(m_scene ? m_scene->GetGraphicsProvider() : nullptr);
        m_selectedObject->Components.push_back(component.release());
        message = "[Properties] Added component '" + typeName + "'";
        return true;
    }
    catch (const std::exception& error)
    {
        message = "[Properties] Could not add component '" + typeName + "': " + error.what();
        return false;
    }
}

void PropertiesView::DrawComponentPicker(IEditorUi& ui)
{
    if (!m_componentPickerOpen || !m_selectedObject)
        return;
    if (m_positionComponentPicker)
    {
        ui.SetNextWindowRect(420.f, 220.f, 360.f, 420.f);
        m_positionComponentPicker = false;
    }
    if (!ui.BeginWindow("Add Component", &m_componentPickerOpen))
    {
        ui.EndWindow();
        return;
    }

    ui.InputText("Search", m_componentSearch, sizeof(m_componentSearch));
    ui.Separator();
    const std::vector<std::string> types = SceneSerializer::GetRegisteredComponentTypes();
    bool foundMatch = false;
    for (const std::string& type : types)
    {
        if (!ContainsIgnoringCase(type, m_componentSearch))
            continue;
        foundMatch = true;
        if (ui.Selectable(type.c_str()))
        {
            std::string message;
            const bool added = AddRegisteredComponent(type, message);
            LogAssetDrop(message, !added);
            if (added)
            {
                if (OnComponentsChanged)
                    OnComponentsChanged();
                m_componentPickerOpen = false;
            }
        }
    }
    if (!foundMatch)
        ui.DisabledLabel("No matching components");
    ui.EndWindow();
}

void PropertiesView::LogAssetDrop(const std::string& message, bool error) const
{
    if (OnAssetDropLog)
        OnAssetDropLog(message, error);
    else
        OutputDebugStringA((message + "\n").c_str());
}

void PropertiesView::UnpackSelectedPrefab()
{
    Object* prefabRoot = m_selectedObject
        ? m_selectedObject->GetPrefabInstanceRoot()
        : nullptr;
    if (!prefabRoot || !prefabRoot->Prefab)
        return;

    const std::string prefabPath = prefabRoot->Prefab->GetPath();
    prefabRoot->Prefab.reset();
    LogAssetDrop("[Properties] Unpacked prefab instance: " + prefabPath);
    if (OnComponentsChanged)
        OnComponentsChanged();
}

void PropertiesView::DrawTransform(IEditorUi& ui)
{
    Transform& t = m_selectedObject->transform;
    Object* prefabRoot = m_selectedObject->GetPrefabInstanceRoot();
    const bool locked = prefabRoot && prefabRoot != m_selectedObject;
    
    const bool transformOpen = ui.CollapsingHeader("Transform");
    const EditorUiContextMenuResult menu = ui.ContextMenu(&t,
        locked || m_selectedObject->IsPartOfPrefabInstance() ? nullptr : "Add Component",
        nullptr,
        false,
        prefabRoot ? "Unpack Prefab" : nullptr);
    if (menu.addRequested)
    {
        m_componentPickerOpen = true;
        m_positionComponentPicker = true;
        m_componentSearch[0] = '\0';
    }
    if (menu.unpackRequested)
        UnpackSelectedPrefab();
    if (transformOpen)
    {
        ui.BeginDisabled(locked);
        t.DrawProperties(ui);
        ui.EndDisabled();
    }
}
