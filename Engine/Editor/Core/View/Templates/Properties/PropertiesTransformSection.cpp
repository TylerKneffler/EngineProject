#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Engine/Editor/UI/IEditorUi.h"

void PropertiesView::DrawTransform(IEditorUi& ui)
{
    Transform& t = m_selectedObject->transform;
    Object* prefabRoot = m_selectedObject->GetPrefabInstanceRoot();
    const bool locked = false;

    const bool transformOpen = ui.CollapsingHeader("Transform");
    const EditorUiContextMenuResult menu = ui.ContextMenu(&t,
        CanEditSelectedObject() ? "Add Component" : nullptr,
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
        const bool transformChanged = t.DrawProperties(ui);
        ui.EndDisabled();
        if (transformChanged && OnComponentsChanged)
            OnComponentsChanged();
    }
}
