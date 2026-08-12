#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Prefab/PrefabAsset.h"

bool PropertiesView::CanEditSelectedObject() const
{
    return m_selectedObject && !m_selectedObject->IsPartOfPrefabInstance();
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
