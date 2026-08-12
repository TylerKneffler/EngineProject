#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Serialization/SceneSerializer.h"

void PropertiesView::DrawTransform(IEditorUi& ui)
{
    Transform& t = m_selectedObject->transform;
    Object* prefabRoot = m_selectedObject->GetPrefabInstanceRoot();

    const bool transformOpen = ui.CollapsingHeader("Transform");
    EditorUiContextMenuResult menu;
    if (prefabRoot)
        HandlePrefabMenu(ui.PrefabOverrideMenu(&t,
            SceneSerializer::HasPrefabOverrides(*prefabRoot, true)));
    else
        menu = ui.ContextMenu(&t, "Add Component", nullptr, false);
    if (menu.addRequested)
    {
        m_componentPickerOpen = true;
        m_positionComponentPicker = true;
        m_componentSearch[0] = '\0';
    }
    if (transformOpen)
    {
        const bool transformChanged = t.DrawProperties(ui);
        if (transformChanged)
        {
            // A prefab root's placement transform is serialized separately.
            // Keep its potentially large non-transform override patch cached
            // while the user drags position/rotation/scale values.
            if (m_selectedObject != prefabRoot)
                m_selectedObject->InvalidatePrefabOverrideCache();
            if (OnComponentsChanged) OnComponentsChanged();
        }
    }
}
