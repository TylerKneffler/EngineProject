#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Prefab/PrefabAsset.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"

bool PropertiesView::CanEditSelectedObject() const
{
    return m_selectedObject != nullptr;
}

bool PropertiesView::ApplyConnectedPrefabChanges(bool force)
{
    Object* root = m_selectedObject
        ? m_selectedObject->GetPrefabInstanceRoot() : nullptr;
    if (!m_scene || !root || !root->Prefab)
        return false;
    const std::string current = SceneSerializer::SavePrefabToString(*root, false);
    if (m_prefabSnapshotRoot != root || m_prefabSnapshot.empty())
    {
        m_prefabSnapshotRoot = root;
        m_prefabSnapshot = current;
        if (!force)
            return false;
    }
    else if (current == m_prefabSnapshot)
        return false;

    const std::string path = root->Prefab->GetPath();
    if (!SceneSerializer::SavePrefab(*root, path, true))
    {
        LogAssetDrop("[Properties] Could not save prefab asset: " + path, true);
        return false;
    }
    if (!SceneSerializer::RefreshPrefabInstances(
        *m_scene, path, m_scene->GetGraphicsProvider(), root))
    {
        LogAssetDrop(
            "[Properties] Prefab was saved but connected instances could not be refreshed: " +
            path, true);
        return false;
    }

    m_prefabSnapshotRoot = root;
    m_prefabSnapshot = SceneSerializer::SavePrefabToString(*root, false);
    return true;
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
