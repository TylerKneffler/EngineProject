#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Engine/Editor/UI/EditorComponentIcons.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Scene/Scene.h"
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

    const bool transformOpen = ui.ComponentHeader(
        ComponentIconForType("Transform"), "Transform");
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
        bool transformChanged = false;
        if (m_scene && m_scene->IsEditorMode2D())
        {
            // In a 2D scene X/Y are spatial axes while Z is only the painter's
            // ordering value.  Present that model directly instead of exposing
            // three misleading 3D vector controls.
            transformChanged |= ui.DragFloat("Position X", &t.position.x, 0.01f);
            transformChanged |= ui.DragFloat("Position Y", &t.position.y, 0.01f);
            transformChanged |= ui.DragFloat("Draw Order", &t.position.z, 0.01f);
            transformChanged |= ui.DragFloat("Rotation", &t.rotation.z, 0.01f);
            transformChanged |= ui.DragFloat("Scale X", &t.scale.x, 0.01f);
            transformChanged |= ui.DragFloat("Scale Y", &t.scale.y, 0.01f);
        }
        else
        {
            transformChanged = t.DrawProperties(ui);
        }
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
