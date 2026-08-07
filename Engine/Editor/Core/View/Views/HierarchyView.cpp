#include "HierarchyView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Sprite.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Serialization/SceneSerializer.h"

namespace
{
const char* PlacementName(EditorUiHierarchyDropPosition position)
{
    switch (position)
    {
    case EditorUiHierarchyDropPosition::Before: return "before";
    case EditorUiHierarchyDropPosition::AsChild: return "as child";
    case EditorUiHierarchyDropPosition::After: return "after";
    default: return "none";
    }
}

std::string ObjectName(const Object* object)
{
    if (!object) return "World";
    return object->name.empty() ? "(unnamed)" : object->name;
}

bool CanMoveInPrefabContext(Scene* scene, Object* object, Object* target,
    Scene::ObjectPlacement placement)
{
    if (!scene || !object)
        return false;
    Object* sourceRoot = object->GetPrefabInstanceRoot();
    Object* destinationParent = placement == Scene::ObjectPlacement::AsChild
        ? target : (target ? target->Parent : nullptr);
    Object* destinationRoot = destinationParent
        ? destinationParent->GetPrefabInstanceRoot() : nullptr;
    if (!sourceRoot)
        return true;
    if (object == sourceRoot)
        return destinationRoot == nullptr;
    return destinationRoot == sourceRoot;
}
}

void HierarchyView::DrawPanel(IEditorUi& ui)
{
    if (!ui.BeginWindow(m_title.c_str(), &m_open))
    {
        ui.EndWindow();
        return;
    }
    if (!m_scene) { ui.DisabledLabel("No scene loaded"); ui.EndWindow(); return; }
    const bool copyRequested = ui.CopyShortcutPressed();
    const bool pasteRequested = ui.PasteShortcutPressed();
    if (copyRequested) CopySelection();
    if (pasteRequested) PasteClipboard();
    m_dragObservedThisFrame = false;
    m_dropObservedThisFrame = false;
    const bool worldOpen = ui.TreeNode(
        this, "World", m_selectedObject == nullptr, false, true);
    const EditorUiContextMenuResult worldMenu =
        ui.ContextMenu(this, "Add Object", nullptr, true);
    if (worldMenu.addRequested || worldMenu.addCubeRequested ||
        worldMenu.addSpriteRequested)
    {
        m_pendingAddParent = nullptr;
        m_pendingAddType = worldMenu.addCubeRequested ? PendingAddType::Cube
            : (worldMenu.addSpriteRequested ? PendingAddType::Sprite
                : PendingAddType::Empty);
        m_hasPendingAdd = true;
    }
    const EditorUiHierarchyDropResult worldDrop =
        ui.HierarchyDropTarget("ENGINE_SCENE_OBJECT");
    if (worldDrop.position != EditorUiHierarchyDropPosition::None &&
        (m_debugHoverTarget != nullptr || m_debugHoverPosition != worldDrop.position))
    {
        m_debugHoverTarget = nullptr;
        m_debugHoverPosition = worldDrop.position;
        m_debugHoverTargetDepth = 0;
        m_debugDropDepth = 0;
        LogInteraction("Hover target 'World' (root level)");
    }
    if (worldDrop.position != EditorUiHierarchyDropPosition::None)
        m_dropObservedThisFrame = true;
    if (worldDrop.data && worldDrop.size == sizeof(Object*))
    {
        m_pendingDragged = *static_cast<Object* const*>(worldDrop.data);
        m_pendingTarget = nullptr;
        m_pendingPlacement = Scene::ObjectPlacement::AsChild;
        m_hasPendingMove = true;
        LogInteraction("Drop requested: '" + ObjectName(m_pendingDragged) + "' -> 'World' (root level)");
    }
    if (ui.IsItemClicked())
        SetSelectedObject(nullptr);
    if (worldOpen)
    {
        std::vector<Object*> roots;
        for (const auto& obj : m_scene->GetObjects())
            if (!obj->Parent)
                roots.push_back(obj.get());
        for (size_t index = 0; index < roots.size(); ++index)
            DrawObjectNode(ui, roots[index], 0, index + 1 == roots.size());
        ui.TreePop();
    }
    if (m_pendingDelete)
    {
        const std::string deletedName = ObjectName(m_pendingDelete);
        Object* deletedPrefabRoot = m_pendingDelete->GetPrefabInstanceRoot();
        bool deletesSelection = false;
        for (Object* current = m_selectedObject; current; current = current->Parent)
            if (current == m_pendingDelete)
                deletesSelection = true;
        if (deletesSelection)
            SetSelectedObject(deletedPrefabRoot != m_pendingDelete
                ? deletedPrefabRoot : nullptr);
        m_scene->RemoveObject(m_pendingDelete);
        LogInteraction("Deleted '" + deletedName + "' and its child hierarchy");
        m_pendingDelete = nullptr;
        if (OnHierarchyChanged) OnHierarchyChanged();
    }
    if (m_hasPendingAdd)
    {
        Object* created = nullptr;
        try
        {
            if (m_pendingAddType == PendingAddType::Cube)
            {
                created = m_scene->AddObject("Cube");
                Mesh* mesh = created->AddComponent<Mesh>();
                mesh->LoadFromFile("Assets/Mesh/cube.obj");
                if (m_scene->GetGraphicsProvider())
                    mesh->CreateBuffer(
                        m_scene->GetGraphicsProvider()->GetBufferFactory());
                created->AddComponent<Material>();
            }
            else if (m_pendingAddType == PendingAddType::Sprite)
            {
                created = m_scene->AddObject("Sprite");
                created->AddComponent<Sprite>();
            }
            else
            {
                created = m_scene->AddObject("GameObject");
            }
        }
        catch (const std::exception& error)
        {
            if (created)
                m_scene->RemoveObject(created);
            LogInteraction("Add object failed: " + std::string(error.what()));
            created = nullptr;
        }
        if (created && m_pendingAddParent)
        {
            m_scene->MoveObject(created, m_pendingAddParent, Scene::ObjectPlacement::AsChild);
            created->transform.position = glm::vec3(0.f);
            created->transform.rotation = glm::vec3(0.f);
            created->transform.scale = glm::vec3(1.f);
        }
        if (created)
        {
            SetSelectedObject(created);
            LogInteraction("Added '" + ObjectName(created) + "'" +
                std::string(m_pendingAddParent
                    ? " as child of '" + ObjectName(m_pendingAddParent) + "'"
                    : " to World"));
        }
        m_pendingAddParent = nullptr;
        m_hasPendingAdd = false;
        if (created && OnHierarchyChanged) OnHierarchyChanged();
    }
    const EditorUiHierarchyDropResult backgroundDrop =
        ui.HierarchyBackgroundDropTarget("ENGINE_SCENE_OBJECT");
    if (!m_dropObservedThisFrame &&
        backgroundDrop.position != EditorUiHierarchyDropPosition::None)
    {
        m_dropObservedThisFrame = true;
        const bool targetChanged = m_debugHoverTarget != nullptr ||
            m_debugHoverPosition != EditorUiHierarchyDropPosition::AsChild ||
            m_debugDropDepth != 0;
        m_debugHoverTarget = nullptr;
        m_debugHoverPosition = EditorUiHierarchyDropPosition::AsChild;
        m_debugHoverTargetDepth = 0;
        m_debugDropDepth = 0;
        if (targetChanged)
            LogInteraction("Hover hierarchy background (World root, append)");
        if (backgroundDrop.data && backgroundDrop.size == sizeof(Object*))
        {
            m_pendingDragged = *static_cast<Object* const*>(backgroundDrop.data);
            m_pendingTarget = nullptr;
            m_pendingPlacement = Scene::ObjectPlacement::AsChild;
            m_hasPendingMove = true;
            LogInteraction("Drop requested: '" + ObjectName(m_pendingDragged) +
                "' -> 'World' (root append)");
        }
    }
    if (m_hasPendingMove)
    {
        const std::string sourceName = ObjectName(m_pendingDragged);
        const std::string targetName = ObjectName(m_pendingTarget);
        const bool moved = CanMoveInPrefabContext(
            m_scene, m_pendingDragged, m_pendingTarget, m_pendingPlacement) &&
            m_scene->MoveObject(
                m_pendingDragged, m_pendingTarget, m_pendingPlacement);
        LogInteraction(std::string(moved ? "Move succeeded: '" : "Move rejected: '") +
            sourceName + "' -> '" + targetName + "'");
        if (moved && OnHierarchyChanged) OnHierarchyChanged();
        m_hasPendingMove = false;
        m_pendingDragged = nullptr;
        m_pendingTarget = nullptr;
        m_debugDragSource = nullptr;
        m_debugHoverTarget = nullptr;
        m_debugHoverPosition = EditorUiHierarchyDropPosition::None;
        m_debugDropDepth = -1;
    }
    else if (m_debugDragSource && !m_dragObservedThisFrame)
    {
        if (m_debugHoverPosition != EditorUiHierarchyDropPosition::None)
        {
            const std::string sourceName = ObjectName(m_debugDragSource);
            Object* resolvedTarget = m_debugHoverTarget;
            Scene::ObjectPlacement placement = !resolvedTarget
                ? Scene::ObjectPlacement::AsChild
                : (m_debugHoverPosition == EditorUiHierarchyDropPosition::Before
                    ? Scene::ObjectPlacement::Before
                    : (m_debugHoverPosition == EditorUiHierarchyDropPosition::After
                        ? Scene::ObjectPlacement::After
                        : Scene::ObjectPlacement::AsChild));
            const int naturalDepth = m_debugHoverPosition == EditorUiHierarchyDropPosition::AsChild
                ? m_debugHoverTargetDepth + 1 : m_debugHoverTargetDepth;
            if (resolvedTarget && m_debugDropDepth >= 0 && m_debugDropDepth < naturalDepth)
            {
                int anchorDepth = m_debugHoverTargetDepth;
                while (resolvedTarget && anchorDepth > m_debugDropDepth)
                {
                    resolvedTarget = resolvedTarget->Parent;
                    --anchorDepth;
                }
                placement = resolvedTarget
                    ? (m_debugHoverPosition == EditorUiHierarchyDropPosition::Before
                        ? Scene::ObjectPlacement::Before : Scene::ObjectPlacement::After)
                    : Scene::ObjectPlacement::AsChild;
            }
            const std::string targetName = ObjectName(resolvedTarget);
            LogInteraction("Release recovered at last valid target: '" + sourceName +
                "' -> '" + targetName + "' (" + PlacementName(m_debugHoverPosition) +
                ", depth " + std::to_string(m_debugDropDepth) + ")");
            const bool moved = CanMoveInPrefabContext(
                m_scene, m_debugDragSource, resolvedTarget, placement) &&
                m_scene->MoveObject(
                    m_debugDragSource, resolvedTarget, placement);
            LogInteraction(std::string(moved ? "Move succeeded: '" : "Move rejected: '") +
                sourceName + "' -> '" + targetName + "'");
            if (moved && OnHierarchyChanged) OnHierarchyChanged();
        }
        else
            LogInteraction("Drag cancelled: '" + ObjectName(m_debugDragSource) + "'");
        m_debugDragSource = nullptr;
        m_debugHoverTarget = nullptr;
        m_debugHoverPosition = EditorUiHierarchyDropPosition::None;
        m_debugDropDepth = -1;
    }
    else if (m_debugDragSource && !m_dropObservedThisFrame)
    {
        // Do not retain a stale target if the pointer leaves the hierarchy
        // while the drag is still active.
        m_debugHoverTarget = nullptr;
        m_debugHoverPosition = EditorUiHierarchyDropPosition::None;
        m_debugDropDepth = -1;
    }
    if (ui.IsWindowBackgroundClicked()) SetSelectedObject(nullptr);
    ui.EndWindow();
}

void HierarchyView::CopySelection()
{
    if (!m_scene || !m_selectedObject)
        return;
    m_objectClipboard = SceneSerializer::SaveObjectToString(*m_selectedObject);
    m_clipboardSourcePath.clear();
    m_scene->TryGetObjectPath(m_selectedObject, m_clipboardSourcePath);
    LogInteraction("Copied '" + ObjectName(m_selectedObject) + "' with " +
        std::to_string(m_selectedObject->Children.size()) + " direct child object(s)");
}

void HierarchyView::PasteClipboard()
{
    if (!m_scene || m_objectClipboard.empty())
        return;

    Object* source = m_scene->FindObjectByPath(m_clipboardSourcePath);
    Object* pasted = SceneSerializer::InstantiateObjectFromString(
        *m_scene, m_objectClipboard, m_scene->GetGraphicsProvider());
    if (!pasted)
    {
        LogInteraction("Paste failed: copied object data could not be instantiated");
        return;
    }

    const glm::vec3 localPosition = pasted->transform.position;
    const glm::vec3 localRotation = pasted->transform.rotation;
    const glm::vec3 localScale = pasted->transform.scale;
    if (source)
    {
        m_scene->MoveObject(pasted, source, Scene::ObjectPlacement::After);
        pasted->transform.position = localPosition;
        pasted->transform.rotation = localRotation;
        pasted->transform.scale = localScale;
    }
    pasted->name += " (Copy)";
    SetSelectedObject(pasted);
    m_scene->TryGetObjectPath(pasted, m_clipboardSourcePath);
    if (OnHierarchyChanged) OnHierarchyChanged();
    LogInteraction("Pasted '" + ObjectName(pasted) + "' with its child hierarchy");
}

void HierarchyView::SetDebugInteractionLogging(bool enabled)
{
    m_debugInteractionLogging = enabled;
    if (!enabled)
    {
        m_debugDragSource = nullptr;
        m_debugHoverTarget = nullptr;
        m_debugHoverPosition = EditorUiHierarchyDropPosition::None;
        m_debugDropDepth = -1;
    }
}

void HierarchyView::LogInteraction(const std::string& message) const
{
    if (m_debugInteractionLogging && OnInteractionLog)
        OnInteractionLog("[Hierarchy] " + message);
}

void HierarchyView::SetSelectedObject(Object* obj)
{
    if (m_selectedObject == obj) return;
    m_selectedObject = obj;
    if (OnSelectionChanged) OnSelectionChanged(obj);
}

void HierarchyView::DrawObjectNode(
    IEditorUi& ui, Object* obj, int depth, bool lastSibling,
    uint64_t ancestorGuideMask)
{
    const bool hasChildren = !obj->Children.empty();
    Object* prefabRoot = obj->GetPrefabInstanceRoot();
    const bool editingPrefab = prefabRoot != nullptr;
    char name[256]; strncpy_s(name, obj->name.c_str(), sizeof(name));
    bool enabled = obj->enabled;
    const EditorUiObjectRowResult row = ui.ObjectTreeRow(
        obj, name, sizeof(name), &enabled, obj == m_selectedObject,
        !hasChildren, obj->IsPartOfPrefabInstance() && !editingPrefab,
        obj->IsEnabledInHierarchy(),
        depth, lastSibling, ancestorGuideMask);
    const bool editableHierarchy = prefabRoot == nullptr || editingPrefab;
    const bool deletable = editableHierarchy || prefabRoot == obj;
    const EditorUiContextMenuResult menu = ui.ContextMenu(obj,
        editableHierarchy ? "Add Object" : nullptr,
        deletable ? "Delete Object" : nullptr,
        editableHierarchy,
        prefabRoot ? "Unpack Prefab" : nullptr);
    if (menu.addRequested || menu.addCubeRequested || menu.addSpriteRequested)
    {
        m_pendingAddParent = obj;
        m_pendingAddType = menu.addCubeRequested ? PendingAddType::Cube
            : (menu.addSpriteRequested ? PendingAddType::Sprite
                : PendingAddType::Empty);
        m_hasPendingAdd = true;
    }
    if (menu.deleteRequested)
        m_pendingDelete = obj;
    if (menu.unpackRequested && prefabRoot && prefabRoot->Prefab)
    {
        const std::string prefabPath = prefabRoot->Prefab->GetPath();
        const std::string instanceName = ObjectName(prefabRoot);
        prefabRoot->Prefab.reset();
        LogInteraction("Unpacked prefab instance '" + instanceName + "': " + prefabPath);
        if (OnHierarchyChanged) OnHierarchyChanged();
    }
    if (row.nameChanged) obj->name = name;
    if (row.enabledChanged && (editableHierarchy || prefabRoot == obj))
    {
        enabled ? obj->Enabled() : obj->Disabled();
        LogInteraction("Set '" + ObjectName(obj) + "' " + (enabled ? "enabled" : "disabled"));
    }
    if (row.clicked) { SetSelectedObject(obj); LogInteraction("Selected '" + ObjectName(obj) + "'"); }
    if (row.doubleClicked) { SetSelectedObject(obj); LogInteraction("Focused '" + ObjectName(obj) + "'"); if (OnFocusObject) OnFocusObject(obj); }
    if (row.dragActive)
    {
        m_dragObservedThisFrame = true;
        if (m_debugDragSource != obj)
        {
            m_debugDragSource = obj;
            LogInteraction("Drag started: '" + ObjectName(obj) + "'");
        }
    }
    if (row.dropHovered &&
        (m_debugHoverTarget != obj || m_debugHoverPosition != row.dropPosition ||
            m_debugDropDepth != row.dropDepth))
    {
        m_debugHoverTarget = obj;
        m_debugHoverPosition = row.dropPosition;
        m_debugHoverTargetDepth = depth;
        m_debugDropDepth = row.dropDepth;
        LogInteraction("Hover target '" + ObjectName(obj) + "' (" +
            PlacementName(row.dropPosition) + ", depth " + std::to_string(row.dropDepth) + ")");
    }
    if (row.dropHovered)
        m_dropObservedThisFrame = true;
    if (row.droppedItem)
    {
        m_pendingDragged = static_cast<Object*>(const_cast<void*>(row.droppedItem));
        m_pendingTarget = obj;
        m_pendingPlacement = row.dropPosition == EditorUiHierarchyDropPosition::Before
            ? Scene::ObjectPlacement::Before
            : (row.dropPosition == EditorUiHierarchyDropPosition::After
                ? Scene::ObjectPlacement::After
                : Scene::ObjectPlacement::AsChild);

        const int naturalDepth = row.dropPosition == EditorUiHierarchyDropPosition::AsChild
            ? depth + 1 : depth;
        if (row.dropDepth >= 0 && row.dropDepth < naturalDepth)
        {
            // Outdent to the ancestor aligned with the pointer. Dropping in
            // the lower/center portion places the object after that ancestor;
            // the upper portion places it before the ancestor.
            Object* anchor = obj;
            int anchorDepth = depth;
            while (anchor && anchorDepth > row.dropDepth)
            {
                anchor = anchor->Parent;
                --anchorDepth;
            }
            if (anchor)
            {
                m_pendingTarget = anchor;
                m_pendingPlacement = row.dropPosition == EditorUiHierarchyDropPosition::Before
                    ? Scene::ObjectPlacement::Before : Scene::ObjectPlacement::After;
            }
            else
            {
                m_pendingTarget = nullptr;
                m_pendingPlacement = Scene::ObjectPlacement::AsChild;
            }
        }
        m_hasPendingMove = true;
        LogInteraction("Drop requested: '" + ObjectName(m_pendingDragged) + "' -> '" +
            ObjectName(m_pendingTarget) + "' (" + PlacementName(row.dropPosition) +
            ", depth " + std::to_string(row.dropDepth) + ")");
    }
    if (row.open && hasChildren)
    {
        uint64_t childGuideMask = ancestorGuideMask;
        if (!lastSibling && depth < 64)
            childGuideMask |= uint64_t{1} << depth;
        for (size_t index = 0; index < obj->Children.size(); ++index)
            DrawObjectNode(ui, obj->Children[index], depth + 1,
                index + 1 == obj->Children.size(), childGuideMask);
        ui.ObjectTreePop();
    }
}
