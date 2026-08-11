#include "PropertiesView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Component.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Scene/Scene.h"
#include <filesystem>
#include <shellapi.h>

namespace
{
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

void PropertiesView::SetSelectedAsset(const std::string& path)
{
    m_selectedObject = nullptr;
    m_assetInspector.Select(path);
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
    if (m_assetInspector.HasSelection())
    {
        m_assetInspector.OnRenamed = OnAssetRenamed;
        m_assetInspector.OnContentsChanged = OnAssetContentsChanged;
        m_assetInspector.Draw(ui, m_scene);
        ui.EndTextWrap();
        ui.EndWindow();
        return;
    }
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
    char name[256]; strncpy_s(name, m_selectedObject->name.c_str(), sizeof(name));
    bool enabled = m_selectedObject->enabled;
    const EditorUiObjectRowResult header = ui.ObjectHeader(
        m_selectedObject, name, sizeof(name), &enabled,
        false);
    if (header.nameChanged)
    {
        m_selectedObject->name = name;
        if (OnComponentsChanged)
            OnComponentsChanged();
    }
    if (header.enabledChanged)
    {
        enabled ? m_selectedObject->Enabled() : m_selectedObject->Disabled();
        if (OnComponentsChanged)
            OnComponentsChanged();
    }
    if (prefabRoot && prefabRoot->Prefab)
    {
        ui.ValueLabel("Prefab", prefabRoot->Prefab->GetPath().c_str());
        ui.ColoredLabel("Linked prefab - changes save automatically",
            { 0.35f, 0.7f, 1.f, 1.f });
        if (m_selectedObject == prefabRoot)
            ui.DisabledLabel(
                "Root transform is this instance's placement; prefab components and children are shared.");
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
    bool componentPropertiesChanged = false;
    for (Component* component : m_selectedObject->Components)
    {
        if (!component) continue;
        ui.PushId(component);
        
        std::string componentType = component->GetTypeName();
        if (componentType.empty()) componentType = "Component";
        
        const bool componentOpen = ui.CollapsingHeader(componentType.c_str());
        Object* componentPrefabRoot = m_selectedObject->GetPrefabInstanceRoot();
        const bool componentEditable = true;
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
            const bool locked = m_selectedObject->IsPartOfPrefabInstance() &&
                !CanEditSelectedObject();
            ui.BeginDisabled(locked);
            componentPropertiesChanged =
                component->DrawProperties(ui) || componentPropertiesChanged;
            ui.EndDisabled();
        }
        ui.PopId();
    }

    if (componentPropertiesChanged && OnComponentsChanged)
        OnComponentsChanged();

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
    ui.BeginDisabled(!CanEditSelectedObject());
    if (ui.Button("Add Component"))
    {
        m_componentPickerOpen = true;
        m_positionComponentPicker = true;
        m_componentSearch[0] = '\0';
    }
    ui.EndDisabled();

    ui.EndTextWrap();
    ui.EndWindow();
    DrawComponentPicker(ui);
}
