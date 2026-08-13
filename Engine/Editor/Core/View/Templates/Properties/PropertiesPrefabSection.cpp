#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Prefab/PrefabAsset.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Scene/Scene.h"

namespace Engine::Editor
{
bool PropertiesView::CanEditSelectedObject() const
{
    return m_selectedObject && !m_selectedObject->IsPartOfPrefabInstance();
}

void PropertiesView::UnpackSelectedPrefab()
{
    Engine::Core::Object* prefabRoot = m_selectedObject
        ? m_selectedObject->GetPrefabInstanceRoot()
        : nullptr;
    if (!prefabRoot || !prefabRoot->Prefab)
        return;

    const std::string prefabPath = prefabRoot->Prefab->GetPath();
    prefabRoot->Prefab.reset();
    prefabRoot->PrefabSourceSnapshot.clear();
    LogAssetDrop("[Properties] Unpacked prefab instance: " + prefabPath);
    if (OnComponentsChanged)
        OnComponentsChanged();
}

void PropertiesView::ApplySelectedPrefabOverrides(bool includeRootTransform)
{
    Engine::Core::Object* prefabRoot = m_selectedObject
        ? m_selectedObject->GetPrefabInstanceRoot() : nullptr;
    if (!prefabRoot) return;
    if (Engine::Serialization::SceneSerializer::ApplyPrefabOverridesToAsset(*prefabRoot,
        includeRootTransform, m_scene ? m_scene->GetGraphicsProvider() : nullptr))
    {
        LogAssetDrop(includeRootTransform
            ? "[Properties] Applied all overrides to prefab."
            : "[Properties] Applied overrides to prefab (root transform excluded).",
            false);
        if (OnComponentsChanged) OnComponentsChanged();
    }
    else LogAssetDrop("[Properties] Could not apply prefab overrides.", true);
}

void PropertiesView::RevertSelectedPrefabOverrides()
{
    Engine::Core::Object* prefabRoot = m_selectedObject
        ? m_selectedObject->GetPrefabInstanceRoot() : nullptr;
    if (!prefabRoot) return;
    if (Engine::Serialization::SceneSerializer::RevertPrefabOverrides(*prefabRoot,
        m_scene ? m_scene->GetGraphicsProvider() : nullptr))
    {
        LogAssetDrop("[Properties] Reverted prefab instance overrides.");
        if (OnComponentsChanged) OnComponentsChanged();
    }
    else LogAssetDrop("[Properties] Could not revert prefab overrides.", true);
}

void PropertiesView::HandlePrefabMenu(const EditorUiPrefabMenuResult& menu)
{
    if (menu.applyRequested) ApplySelectedPrefabOverrides(false);
    if (menu.applyAllRequested) ApplySelectedPrefabOverrides(true);
    if (menu.revertRequested) RevertSelectedPrefabOverrides();
    if (menu.unpackRequested) UnpackSelectedPrefab();
}
}
