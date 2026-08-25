#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Serialization/SceneSerializer.h"

namespace Engine::Editor
{
void PropertiesView::DrawTransform(IEditorUi& ui)
{
    Engine::Components::Transform& t = m_selectedObject->transform;
    Engine::Core::Object* prefabRoot = m_selectedObject->GetPrefabInstanceRoot();

    // A completed inspector drag needs one fresh prefab patch.  While the
    // control is active, keep the previous patch cached so drawing this panel
    // does not serialize and diff a large imported hierarchy every frame.
    if (m_deferredTransformPrefabRoot && !ui.IsAnyItemActive())
    {
        m_deferredTransformPrefabRoot->PrefabOverrideCacheValid = false;
        m_deferredTransformPrefabRoot = nullptr;
    }

    const bool transformOpen = ui.CollapsingHeader("Transform");
    EditorUiContextMenuResult menu;
    if (prefabRoot)
        HandlePrefabMenu(ui.PrefabOverrideMenu(&t,
            Engine::Serialization::SceneSerializer::HasPrefabOverrides(*prefabRoot, true)));
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
            if (auto* body =
                m_selectedObject->GetComponent<Engine::Components::RigidBody>())
                body->NotifyEditorTransformChanged();

            // A prefab root's placement transform is serialized separately.
            // Keep its potentially large non-transform override patch cached
            // while the user drags position/rotation/scale values.
            if (m_selectedObject != prefabRoot)
                m_deferredTransformPrefabRoot = prefabRoot;
            if (OnComponentsChanged) OnComponentsChanged();
        }
    }
}
}
