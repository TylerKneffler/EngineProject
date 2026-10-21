#include "HierarchyView.h"
#include "Engine/Editor/Core/PrimitiveObjectFactory.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Component.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/Compoonents/Lighting/LightProbe.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Serialization/SceneSerializer.h"
#include "../Focus/WindowFocusHandler.h"
#include <cctype>

namespace Engine::Editor
{
// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
HierarchyView::HierarchyView()
{
    // Hierarchy is an editor UI panel with normal cursor
    SetCursorBehaviorOnFocus(CursorBehaviorOnFocus::Visible);
}

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

std::string ObjectName(const Engine::Core::Object* object)
{
    if (!object) return "World";
    return object->name.empty() ? "(unnamed)" : object->name;
}

bool CanMoveInPrefabContext(Engine::Scene::Scene* scene, Engine::Core::Object* object, Engine::Core::Object* target,
    ::Engine::Scene::Scene::ObjectPlacement placement)
{
    if (!scene || !object)
        return false;
    Engine::Core::Object* sourceRoot = object->GetPrefabInstanceRoot();
    Engine::Core::Object* destinationParent = placement == ::Engine::Scene::Scene::ObjectPlacement::AsChild
        ? target : (target ? target->Parent : nullptr);
    Engine::Core::Object* destinationRoot = destinationParent
        ? destinationParent->GetPrefabInstanceRoot() : nullptr;
    if (!sourceRoot)
        return true;
    if (object == sourceRoot)
        return destinationRoot == nullptr;
    return destinationRoot == sourceRoot;
}
}

void HierarchyView::RequestDeleteSelectedObject()
{
    if (m_allowDelete && m_selectedObject)
        QueueDeleteObject(m_selectedObject);
}

void HierarchyView::QueueDeleteObject(Engine::Core::Object* object)
{
    if (!m_allowDelete || !object) return;
    if (m_objectFilter && (!m_allowFilteredContextActions ||
        object == m_filteredContextRoot)) return;
    const bool selectedObject = std::find(m_selectedObjects.begin(),
        m_selectedObjects.end(), object) != m_selectedObjects.end();
    if (Engine::Core::Object* prefabRoot = object->GetPrefabInstanceRoot())
        object = prefabRoot;
    if (m_objectFilter && object == m_filteredContextRoot) return;
    if (selectedObject)
    {
        for (Engine::Core::Object* selected : m_selectedObjects)
        {
            if (m_objectFilter && selected == m_filteredContextRoot) continue;
            Engine::Core::Object* target = selected->GetPrefabInstanceRoot();
            if (m_objectFilter && target == m_filteredContextRoot) continue;
            m_pendingDeletes.push_back(target ? target : selected);
        }
    }
    else
        m_pendingDeletes.push_back(object);
}

bool HierarchyView::MoveDraggedObjects(Engine::Core::Object* dragged,
    Engine::Core::Object* target,
    ::Engine::Scene::Scene::ObjectPlacement placement)
{
    if (!m_scene || !dragged || (m_objectFilter && !m_allowFilteredReparent))
        return false;
    const bool inSelection = std::find(m_selectedObjects.begin(),
        m_selectedObjects.end(), dragged) != m_selectedObjects.end();
    const std::vector<Engine::Core::Object*> candidates = inSelection
        ? m_selectedObjects : std::vector<Engine::Core::Object*>{ dragged };
    std::vector<Engine::Core::Object*> roots;
    for (Engine::Core::Object* candidate : candidates)
    {
        bool nested = false;
        for (Engine::Core::Object* parent = candidate->Parent; parent;
            parent = parent->Parent)
            if (std::find(candidates.begin(), candidates.end(), parent) !=
                candidates.end()) { nested = true; break; }
        if (!nested) roots.push_back(candidate);
    }
    for (Engine::Core::Object* root : roots)
    {
        for (Engine::Core::Object* parent = target; parent;
            parent = parent->Parent)
            if (parent == root) return false;
        if (!CanMoveInPrefabContext(m_scene, root, target, placement))
            return false;
    }
    if (placement == ::Engine::Scene::Scene::ObjectPlacement::After)
        std::reverse(roots.begin(), roots.end());
    bool moved = false;
    for (Engine::Core::Object* root : roots)
        moved = m_scene->MoveObject(root, target, placement) || moved;
    return moved;
}

void HierarchyView::DrawPanel(IEditorUi& ui)
{
    const bool windowVisible = ui.BeginWindow(m_title.c_str(), &m_open);
    if (ui.IsWindowFocused() && OnFocused) OnFocused();
    if (!windowVisible)
    {
        ui.EndWindow();
        return;
    }
    if (!m_scene) { ui.DisabledLabel("No scene loaded"); ui.EndWindow(); return; }
    ui.DisabledLabel("SCENE HIERARCHY");
    if (!m_objectFilter)
    {
        ui.SameLineRight(28.f);
        if (ui.Button("+", 28.f))
        {
            m_pendingAddParent = nullptr;
            m_pendingAddType = PendingAddType::Empty;
            m_hasPendingAdd = true;
        }
        ui.Tooltip("Create empty object");
    }
    ui.SearchInput("##hierarchySearch", m_search, sizeof(m_search),
        "Search objects...");
    m_searchMatches.clear();
    ui.Separator();
    const bool copyRequested = ui.CopyShortcutPressed();
    const bool cutRequested = ui.CutShortcutPressed();
    const bool pasteRequested = ui.PasteShortcutPressed();
    const bool deleteRequested = ui.DeleteShortcutPressed();
    if (copyRequested) CopySelection();
    if (cutRequested) CutSelection();
    if (pasteRequested) PasteClipboard();
    if (deleteRequested && m_allowDelete) RequestDeleteSelectedObject();
    m_dragObservedThisFrame = false;
    m_dropObservedThisFrame = false;
    const bool worldOpen = ui.TreeNode(
        this, "World", m_selectedObject == nullptr, false, true);
    const EditorUiContextMenuResult worldMenu = m_objectFilter
        ? EditorUiContextMenuResult{}
        : ui.ContextMenu(this, "Create", nullptr, true, nullptr, true,
            !m_objectClipboard.empty());
    if (worldMenu.pasteRequested)
        PasteClipboard(nullptr, true);
    if (worldMenu.addRequested || !worldMenu.primitive3D.empty() ||
        worldMenu.addSpriteRequested || worldMenu.addLightProbeRequested ||
        worldMenu.addLightProbeGroupRequested)
    {
        m_pendingAddParent = nullptr;
        m_pendingAddType = !worldMenu.primitive3D.empty() ? PendingAddType::Primitive3D
            : (worldMenu.addSpriteRequested ? PendingAddType::Sprite
            : (worldMenu.addLightProbeRequested ? PendingAddType::LightProbe
            : (worldMenu.addLightProbeGroupRequested
                ? PendingAddType::LightProbeGroup : PendingAddType::Empty)));
        m_pendingPrimitive3D = worldMenu.primitive3D;
        m_hasPendingAdd = true;
    }
    const EditorUiHierarchyDropResult worldDrop = m_objectFilter
        ? EditorUiHierarchyDropResult{}
        : ui.HierarchyDropTarget("ENGINE_SCENE_OBJECT");
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
    if (worldDrop.data && worldDrop.size == sizeof(Engine::Core::Object*))
    {
        m_pendingDragged = *static_cast<Engine::Core::Object* const*>(worldDrop.data);
        m_pendingTarget = nullptr;
        m_pendingPlacement = ::Engine::Scene::Scene::ObjectPlacement::AsChild;
        m_hasPendingMove = true;
        LogInteraction("Drop requested: '" + ObjectName(m_pendingDragged) + "' -> 'World' (root level)");
    }
    if (ui.IsItemClicked())
        SelectSceneRoot();
    if (worldOpen)
    {
        std::vector<Engine::Core::Object*> roots;
        for (const auto& obj : m_scene->GetObjects())
            if (!obj->Parent)
                roots.push_back(obj.get());
        for (size_t index = 0; index < roots.size(); ++index)
            DrawObjectNode(ui, roots[index], 0, index + 1 == roots.size());
        ui.TreePop();
    }
    if (m_hasPendingPaste)
    {
        PasteClipboard(m_pendingPasteTarget, true);
        m_pendingPasteTarget = nullptr;
        m_hasPendingPaste = false;
    }
    if (m_pendingPrefabRoot && m_pendingPrefabAction != PendingPrefabAction::None)
    {
        bool changed = false;
        if (m_pendingPrefabAction == PendingPrefabAction::Apply)
            changed = Engine::Serialization::SceneSerializer::ApplyPrefabOverridesToAsset(
                *m_pendingPrefabRoot, false, m_scene->GetGraphicsProvider());
        else if (m_pendingPrefabAction == PendingPrefabAction::ApplyAll)
            changed = Engine::Serialization::SceneSerializer::ApplyPrefabOverridesToAsset(
                *m_pendingPrefabRoot, true, m_scene->GetGraphicsProvider());
        else if (m_pendingPrefabAction == PendingPrefabAction::Revert)
            changed = Engine::Serialization::SceneSerializer::RevertPrefabOverrides(
                *m_pendingPrefabRoot, m_scene->GetGraphicsProvider());
        else if (m_pendingPrefabAction == PendingPrefabAction::Unpack &&
            m_pendingPrefabRoot->Prefab)
        {
            m_pendingPrefabRoot->Prefab.reset();
            m_pendingPrefabRoot->PrefabSourceSnapshot.clear();
            changed = true;
        }
        m_pendingPrefabRoot = nullptr;
        m_pendingPrefabAction = PendingPrefabAction::None;
        if (changed && OnHierarchyChanged) OnHierarchyChanged();
    }
    if (!m_pendingDeletes.empty())
    {
        std::vector<Engine::Core::Object*> roots;
        for (Engine::Core::Object* candidate : m_pendingDeletes)
        {
            if (!candidate || std::find(roots.begin(), roots.end(), candidate) != roots.end())
                continue;
            bool covered = false;
            for (Engine::Core::Object* parent = candidate->Parent; parent; parent = parent->Parent)
                if (std::find(m_pendingDeletes.begin(), m_pendingDeletes.end(), parent) !=
                    m_pendingDeletes.end()) { covered = true; break; }
            if (!covered) roots.push_back(candidate);
        }
        const auto isDeleted = [&roots](Engine::Core::Object* selected)
        {
            for (Engine::Core::Object* current = selected; current; current = current->Parent)
                if (std::find(roots.begin(), roots.end(), current) != roots.end()) return true;
            return false;
        };
        if (m_selectionAnchor && isDeleted(m_selectionAnchor))
            m_selectionAnchor = nullptr;
        const auto newEnd = std::remove_if(m_selectedObjects.begin(),
            m_selectedObjects.end(), isDeleted);
        if (newEnd != m_selectedObjects.end())
        {
            m_selectedObjects.erase(newEnd, m_selectedObjects.end());
            m_selectedObject = m_selectedObjects.empty() ? nullptr : m_selectedObjects.back();
            NotifySelectionChanged();
        }
        for (Engine::Core::Object* root : roots)
        {
            const std::string deletedName = ObjectName(root);
            m_scene->RemoveObject(root);
            LogInteraction("Deleted '" + deletedName + "' and its child hierarchy");
        }
        m_pendingDeletes.clear();
        if (OnHierarchyChanged) OnHierarchyChanged();
    }
    if (m_hasPendingAdd)
    {
        Engine::Core::Object* created = nullptr;
        try
        {
            if (m_pendingAddType == PendingAddType::Primitive3D)
                created = CreatePrimitiveObject(*m_scene, m_pendingPrimitive3D);
            else if (m_pendingAddType == PendingAddType::Sprite)
            {
                created = m_scene->AddObject("Sprite");
                Engine::Components::SpriteAnimationManager* manager =
                    created->AddComponent<Engine::Components::SpriteAnimationManager>();
                Engine::Components::Sprite* sprite = created->AddComponent<Engine::Components::Sprite>();
                sprite->SetAnimationManager(manager);
            }
            else if(m_pendingAddType==PendingAddType::LightProbe)
            {
                created=m_scene->AddObject("Light Probe");
                created->AddComponent<Engine::Components::LightProbe>();
            }
            else if(m_pendingAddType==PendingAddType::LightProbeGroup)
            {
                created=m_scene->AddObject("Light Probe Volume");
                created->AddComponent<Engine::Components::LightProbeGroup>();
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
            m_scene->MoveObject(created, m_pendingAddParent, ::Engine::Scene::Scene::ObjectPlacement::AsChild);
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
        m_pendingPrimitive3D.clear();
        m_hasPendingAdd = false;
        if (created && OnHierarchyChanged) OnHierarchyChanged();
    }
    const EditorUiHierarchyDropResult backgroundDrop = m_objectFilter
        ? EditorUiHierarchyDropResult{}
        : ui.HierarchyBackgroundDropTarget("ENGINE_SCENE_OBJECT");
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
        if (backgroundDrop.data && backgroundDrop.size == sizeof(Engine::Core::Object*))
        {
            m_pendingDragged = *static_cast<Engine::Core::Object* const*>(backgroundDrop.data);
            m_pendingTarget = nullptr;
            m_pendingPlacement = ::Engine::Scene::Scene::ObjectPlacement::AsChild;
            m_hasPendingMove = true;
            LogInteraction("Drop requested: '" + ObjectName(m_pendingDragged) +
                "' -> 'World' (root append)");
        }
    }
    if (m_hasPendingMove)
    {
        const std::string sourceName = ObjectName(m_pendingDragged);
        const std::string targetName = ObjectName(m_pendingTarget);
        const bool moved = MoveDraggedObjects(m_pendingDragged,
            m_pendingTarget, m_pendingPlacement);
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
            Engine::Core::Object* resolvedTarget = m_debugHoverTarget;
            ::Engine::Scene::Scene::ObjectPlacement placement = !resolvedTarget
                ? ::Engine::Scene::Scene::ObjectPlacement::AsChild
                : (m_debugHoverPosition == EditorUiHierarchyDropPosition::Before
                    ? ::Engine::Scene::Scene::ObjectPlacement::Before
                    : (m_debugHoverPosition == EditorUiHierarchyDropPosition::After
                        ? ::Engine::Scene::Scene::ObjectPlacement::After
                        : ::Engine::Scene::Scene::ObjectPlacement::AsChild));
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
                        ? ::Engine::Scene::Scene::ObjectPlacement::Before : ::Engine::Scene::Scene::ObjectPlacement::After)
                    : ::Engine::Scene::Scene::ObjectPlacement::AsChild;
            }
            const std::string targetName = ObjectName(resolvedTarget);
            LogInteraction("Release recovered at last valid target: '" + sourceName +
                "' -> '" + targetName + "' (" + PlacementName(m_debugHoverPosition) +
                ", depth " + std::to_string(m_debugDropDepth) + ")");
            const bool moved = MoveDraggedObjects(m_debugDragSource,
                resolvedTarget, placement);
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
    if (ui.IsWindowBackgroundClicked()) SelectSceneRoot();
    ui.EndWindow();
}

void HierarchyView::CopySelection()
{
    if (!m_scene || m_selectedObjects.empty()) return;
    m_objectClipboard.clear();
    m_clipboardCut = false;
    m_clipboardScene = m_scene;
    for (Engine::Core::Object* object : m_selectedObjects)
    {
        bool nested = false;
        for (Engine::Core::Object* parent = object->Parent; parent; parent = parent->Parent)
            if (std::find(m_selectedObjects.begin(), m_selectedObjects.end(), parent) !=
                m_selectedObjects.end()) { nested = true; break; }
        if (nested) continue;
        ClipboardObject copied;
        copied.serialized = Engine::Serialization::SceneSerializer::SaveObjectToString(*object);
        copied.sourceObject = object;
        m_scene->TryGetObjectPath(object, copied.sourcePath);
        m_objectClipboard.push_back(std::move(copied));
    }
    LogInteraction("Copied " + std::to_string(m_objectClipboard.size()) +
        " object hierarchy(s)");
}

void HierarchyView::CutSelection()
{
    if (!m_scene || m_selectedObjects.empty()) return;
    CopySelection();
    if (!m_objectClipboard.empty())
        m_clipboardCut = true;
}

void HierarchyView::PasteClipboard(Engine::Core::Object* pasteTarget,
    bool useTarget)
{
    if (!m_scene || m_objectClipboard.empty())
        return;
    if (m_clipboardCut && m_clipboardScene != m_scene)
        return;

    std::vector<Engine::Core::Object*> sources;
    for (const ClipboardObject& copied : m_objectClipboard)
    {
        if (m_clipboardCut)
        {
            const auto& objects = m_scene->GetObjects();
            const bool exists = std::any_of(objects.begin(), objects.end(),
                [&copied](const auto& object)
                { return object.get() == copied.sourceObject; });
            sources.push_back(exists ? copied.sourceObject : nullptr);
        }
        else
            sources.push_back(m_scene->FindObjectByPath(copied.sourcePath));
    }
    if (m_clipboardCut)
    {
        Engine::Core::Object* target = useTarget ? pasteTarget : m_selectedObject;
        if (!useTarget)
            for (Engine::Core::Object* source : sources)
                for (Engine::Core::Object* parent = target; parent; parent = parent->Parent)
                    if (parent == source) { target = source->Parent; break; }
        std::vector<Engine::Core::Object*> movedObjects;
        std::vector<ClipboardObject> remaining;
        for (size_t index = 0; index < sources.size(); ++index)
        {
            Engine::Core::Object* source = sources[index];
            if (source && (!m_objectFilter || m_allowFilteredReparent) &&
                CanMoveInPrefabContext(m_scene, source, target,
                    ::Engine::Scene::Scene::ObjectPlacement::AsChild) &&
                m_scene->MoveObject(source, target,
                    ::Engine::Scene::Scene::ObjectPlacement::AsChild))
                movedObjects.push_back(source);
            else
                remaining.push_back(m_objectClipboard[index]);
        }
        m_objectClipboard = std::move(remaining);
        if (movedObjects.empty()) return;
        m_selectedObjects = std::move(movedObjects);
        m_selectedObject = m_selectedObjects.back();
        m_selectionAnchor = m_selectedObject;
        NotifySelectionChanged();
        if (OnHierarchyChanged) OnHierarchyChanged();
        if (m_objectClipboard.empty()) m_clipboardCut = false;
        LogInteraction("Moved " + std::to_string(m_selectedObjects.size()) +
            " object hierarchy(s)");
        return;
    }
    std::vector<Engine::Core::Object*> pastedObjects;
    std::vector<Engine::Core::Object*> pastedByIndex(m_objectClipboard.size(), nullptr);
    for (size_t index = 0; index < m_objectClipboard.size(); ++index)
    {
        ClipboardObject& copied = m_objectClipboard[index];
        Engine::Core::Object* source = sources[index];
        Engine::Core::Object* pasted =
            Engine::Serialization::SceneSerializer::InstantiateObjectFromString(
                *m_scene, copied.serialized, m_scene->GetGraphicsProvider());
        if (!pasted) continue;
        const glm::vec3 localPosition = pasted->transform.position;
        const glm::vec3 localRotation = pasted->transform.rotation;
        const glm::vec3 localScale = pasted->transform.scale;
        if (useTarget)
        {
            if (pasteTarget && !m_scene->MoveObject(pasted, pasteTarget,
                ::Engine::Scene::Scene::ObjectPlacement::AsChild))
            {
                m_scene->RemoveObject(pasted);
                continue;
            }
            pasted->transform.position = localPosition;
            pasted->transform.rotation = localRotation;
            pasted->transform.scale = localScale;
        }
        else if (source)
        {
            m_scene->MoveObject(pasted, source,
                ::Engine::Scene::Scene::ObjectPlacement::After);
            pasted->transform.position = localPosition;
            pasted->transform.rotation = localRotation;
            pasted->transform.scale = localScale;
        }
        pasted->name += " (Copy)";
        pastedByIndex[index] = pasted;
        pastedObjects.push_back(pasted);
    }
    if (pastedObjects.empty())
    {
        LogInteraction("Paste failed: copied object data could not be instantiated");
        return;
    }
    for (size_t index = 0; index < pastedByIndex.size(); ++index)
        if (pastedByIndex[index])
            m_scene->TryGetObjectPath(pastedByIndex[index],
                m_objectClipboard[index].sourcePath);
    m_selectedObjects = std::move(pastedObjects);
    m_selectedObject = m_selectedObjects.back();
    m_selectionAnchor = m_selectedObject;
    NotifySelectionChanged();
    if (OnHierarchyChanged) OnHierarchyChanged();
    LogInteraction("Pasted " + std::to_string(m_selectedObjects.size()) +
        " object hierarchy(s)");
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

void HierarchyView::SetSelectedObject(Engine::Core::Object* obj)
{
    if (m_selectedObject == obj && m_selectedObjects.size() == (obj ? 1u : 0u))
    {
        if (Engine::Core::Component::HasEditorReferencePick())
            NotifySelectionChanged();
        return;
    }
    m_selectedObject = obj;
    m_selectionAnchor = obj;
    m_selectedObjects.clear();
    if (obj) m_selectedObjects.push_back(obj);
    NotifySelectionChanged();
}

void HierarchyView::NotifySelectionChanged()
{
    if (OnSelectionSetChanged) OnSelectionSetChanged(m_selectedObjects);
    else if (OnSelectionChanged) OnSelectionChanged(m_selectedObject);
}

void HierarchyView::SelectSceneRoot()
{
    // This is an explicit UI selection, not merely an object deselection.
    // Always notify so clicking World can replace an asset inspector even
    // when the hierarchy already has no selected object.
    m_selectedObject = nullptr;
    m_selectionAnchor = nullptr;
    m_selectedObjects.clear();
    NotifySelectionChanged();
}

bool HierarchyView::MatchesSearch(const Engine::Core::Object* object) const
{
    if (m_search[0] == '\0')
        return true;
    if (const auto cached = m_searchMatches.find(object);
        cached != m_searchMatches.end())
        return cached->second;
    const std::string query(m_search);
    const auto found = std::search(object->name.begin(), object->name.end(),
        query.begin(), query.end(), [](unsigned char left, unsigned char right)
        {
            return std::tolower(left) == std::tolower(right);
        });
    const bool matches = found != object->name.end() ||
        std::any_of(object->Children.begin(), object->Children.end(),
        [this](const Engine::Core::Object* child) { return MatchesSearch(child); });
    m_searchMatches.emplace(object, matches);
    return matches;
}

void HierarchyView::DrawObjectNode(
    IEditorUi& ui, Engine::Core::Object* obj, int depth, bool lastSibling,
    uint64_t ancestorGuideMask)
{
    if (!MatchesSearch(obj))
        return;
    if (m_objectFilter && !m_objectFilter(obj))
    {
        for (size_t index = 0; index < obj->Children.size(); ++index)
            DrawObjectNode(ui, obj->Children[index], depth,
                index + 1 == obj->Children.size(), ancestorGuideMask);
        return;
    }
    const bool hasChildren = !obj->Children.empty();
    Engine::Core::Object* prefabRoot = obj->GetPrefabInstanceRoot();
    char name[256]; strncpy_s(name, obj->name.c_str(), sizeof(name));
    bool enabled = obj->enabled;
    const bool readOnlySkeletonRow = m_objectFilter &&
        m_allowSkeletonContextActions;
    const EditorUiObjectRowResult row = ui.ObjectTreeRow(
        obj, EditorUiObjectIcon::Object, name, sizeof(name),
        readOnlySkeletonRow ? nullptr : &enabled,
        std::find(m_selectedObjects.begin(), m_selectedObjects.end(), obj) != m_selectedObjects.end(),
        !hasChildren, readOnlySkeletonRow,
        obj->IsEnabledInHierarchy(),
        depth, lastSibling, ancestorGuideMask, m_search[0] != '\0',
        &obj->editorOverlays,
        static_cast<uint8_t>(
            (obj->GetComponent<Engine::Components::Skeleton>()
                ? Engine::Core::Object::BoneOverlay |
                  Engine::Core::Object::SkeletonOverlay : 0) |
            (obj->GetComponent<Engine::Components::Mesh>() ||
             obj->GetComponent<Engine::Components::Sprite>() ||
             obj->GetComponent<Engine::Components::LightProbeGroup>()
                ? Engine::Core::Object::BoundsOverlay : 0) |
            (obj->GetComponent<Engine::Components::Light>()
                ? Engine::Core::Object::LightVolumeOverlay : 0) |
            (obj->GetComponent<Engine::Components::Camera>()
                ? Engine::Core::Object::CameraFrustumOverlay : 0)));
    const bool editableHierarchy = prefabRoot == nullptr;
    const bool deletable = editableHierarchy || prefabRoot == obj;
    EditorUiContextMenuResult menu;
    bool contextCopy = false, contextCut = false, contextPaste = false;
    if (m_objectFilter && m_allowSkeletonContextActions)
    {
        // These actions only change the editor selection. Imported model
        // hierarchy is deliberately not structurally editable here because
        // its serialized node bindings may use child-index paths.
        menu = ui.ContextMenu(obj, "Focus Bone", "Focus Skeleton Root",
            false, nullptr, false, false, true, row.contextMenuRequested);
        if (menu.addRequested)
            SetSelectedObject(obj);
        if (menu.deleteRequested && m_filteredContextRoot)
            SetSelectedObject(m_filteredContextRoot);
        const bool propertiesRequested = menu.propertiesRequested;
        menu = {};
        menu.propertiesRequested = propertiesRequested;
    }
    else if (prefabRoot)
    {
        const EditorUiPrefabMenuResult prefabMenu = ui.PrefabOverrideMenu(obj,
            Engine::Serialization::SceneSerializer::HasPrefabOverrides(*prefabRoot, true),
            !m_objectClipboard.empty(), true, row.contextMenuRequested);
        menu.propertiesRequested = prefabMenu.propertiesRequested;
        contextCopy = prefabMenu.copyRequested;
        contextCut = prefabMenu.cutRequested;
        contextPaste = prefabMenu.pasteRequested;
        if (prefabMenu.editRequested && prefabRoot->Prefab && OnPrefabRequested)
            OnPrefabRequested(prefabRoot->Prefab->GetPath());
        if (prefabMenu.applyRequested)
            m_pendingPrefabAction = PendingPrefabAction::Apply;
        if (prefabMenu.applyAllRequested)
            m_pendingPrefabAction = PendingPrefabAction::ApplyAll;
        if (prefabMenu.revertRequested)
            m_pendingPrefabAction = PendingPrefabAction::Revert;
        if (prefabMenu.unpackRequested)
            m_pendingPrefabAction = PendingPrefabAction::Unpack;
        if (prefabMenu.deleteRequested)
            QueueDeleteObject(obj);
        if (m_pendingPrefabAction != PendingPrefabAction::None)
            m_pendingPrefabRoot = prefabRoot;
    }
    else if (!m_objectFilter || m_allowFilteredContextActions)
    {
        const bool canDelete = m_objectFilter
            ? (m_allowFilteredContextActions && obj != m_filteredContextRoot)
            : deletable;
        menu = ui.ContextMenu(obj,
            m_objectFilter ? "Add Child" : "Create",
            canDelete ? "Delete Object" : nullptr, true, nullptr,
            true, !m_objectClipboard.empty(), true, row.contextMenuRequested);
    }
    else
    {
        menu = ui.ContextMenu(obj, nullptr, nullptr, false, nullptr,
            false, false, true, row.contextMenuRequested);
    }
    if (menu.propertiesRequested)
    {
        SetSelectedObject(obj);
        if (OnPropertiesRequested) OnPropertiesRequested(obj);
    }
    contextCopy = contextCopy || menu.copyRequested;
    contextCut = contextCut || menu.cutRequested;
    contextPaste = contextPaste || menu.pasteRequested;
    if (contextCopy || contextCut)
    {
        if (std::find(m_selectedObjects.begin(), m_selectedObjects.end(), obj) ==
            m_selectedObjects.end()) SetSelectedObject(obj);
        if (contextCopy) CopySelection();
        else CutSelection();
    }
    if (contextPaste)
    {
        m_pendingPasteTarget = obj;
        m_hasPendingPaste = true;
    }
    if (menu.addRequested || !menu.primitive3D.empty() || menu.addSpriteRequested ||
        menu.addLightProbeRequested || menu.addLightProbeGroupRequested)
    {
        m_pendingAddParent = obj;
        m_pendingAddType = !menu.primitive3D.empty() ? PendingAddType::Primitive3D
            : (menu.addSpriteRequested ? PendingAddType::Sprite
            : (menu.addLightProbeRequested ? PendingAddType::LightProbe
            : (menu.addLightProbeGroupRequested
                ? PendingAddType::LightProbeGroup : PendingAddType::Empty)));
        m_pendingPrimitive3D = menu.primitive3D;
        m_hasPendingAdd = true;
    }
    if (menu.deleteRequested)
        QueueDeleteObject(obj);
    if (row.nameChanged)
    {
        obj->name = name;
        obj->InvalidatePrefabOverrideCache();
    }
    if (row.enabledChanged)
    {
        const bool inSelection = std::find(m_selectedObjects.begin(),
            m_selectedObjects.end(), obj) != m_selectedObjects.end();
        const std::vector<Engine::Core::Object*> targets = inSelection
            ? m_selectedObjects : std::vector<Engine::Core::Object*>{ obj };
        for (Engine::Core::Object* target : targets)
        {
            enabled ? target->Enabled() : target->Disabled();
            target->InvalidatePrefabOverrideCache();
        }
        LogInteraction("Set " + std::to_string(targets.size()) +
            " object(s) " + (enabled ? "enabled" : "disabled"));
    }
    if ((row.nameChanged || row.enabledChanged) && OnHierarchyChanged)
        OnHierarchyChanged();
    if (row.clicked)
    {
        if (ui.IsRangeSelectModifierDown() && m_selectionAnchor && m_scene)
        {
            std::vector<Engine::Core::Object*> ordered;
            std::function<void(Engine::Core::Object*)> appendHierarchy =
                [&](Engine::Core::Object* current)
                {
                    ordered.push_back(current);
                    for (Engine::Core::Object* child : current->Children)
                        appendHierarchy(child);
                };
            for (const auto& candidate : m_scene->GetObjects())
                if (!candidate->Parent) appendHierarchy(candidate.get());
            const auto anchor = std::find(ordered.begin(), ordered.end(), m_selectionAnchor);
            const auto clicked = std::find(ordered.begin(), ordered.end(), obj);
            if (anchor != ordered.end() && clicked != ordered.end())
            {
                const auto first = anchor < clicked ? anchor : clicked;
                const auto last = anchor < clicked ? clicked : anchor;
                m_selectedObjects.assign(first, std::next(last));
                if (m_selectedObjects.back() != obj)
                {
                    m_selectedObjects.erase(std::find(m_selectedObjects.begin(),
                        m_selectedObjects.end(), obj));
                    m_selectedObjects.push_back(obj);
                }
                m_selectedObject = obj;
                NotifySelectionChanged();
            }
            else
                SetSelectedObject(obj);
        }
        else if (ui.IsMultiSelectModifierDown())
        {
            const auto selected = std::find(m_selectedObjects.begin(), m_selectedObjects.end(), obj);
            if (selected == m_selectedObjects.end())
                m_selectedObjects.push_back(obj);
            else
                m_selectedObjects.erase(selected);
            m_selectedObject = m_selectedObjects.empty() ? nullptr : m_selectedObjects.back();
            m_selectionAnchor = obj;
            NotifySelectionChanged();
        }
        else
            SetSelectedObject(obj);
        LogInteraction("Selected '" + ObjectName(obj) + "'");
    }
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
        m_pendingDragged = static_cast<Engine::Core::Object*>(const_cast<void*>(row.droppedItem));
        m_pendingTarget = obj;
        m_pendingPlacement = row.dropPosition == EditorUiHierarchyDropPosition::Before
            ? ::Engine::Scene::Scene::ObjectPlacement::Before
            : (row.dropPosition == EditorUiHierarchyDropPosition::After
                ? ::Engine::Scene::Scene::ObjectPlacement::After
                : ::Engine::Scene::Scene::ObjectPlacement::AsChild);

        const int naturalDepth = row.dropPosition == EditorUiHierarchyDropPosition::AsChild
            ? depth + 1 : depth;
        if (row.dropDepth >= 0 && row.dropDepth < naturalDepth)
        {
            // Outdent to the ancestor aligned with the pointer. Dropping in
            // the lower/center portion places the object after that ancestor;
            // the upper portion places it before the ancestor.
            Engine::Core::Object* anchor = obj;
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
                    ? ::Engine::Scene::Scene::ObjectPlacement::Before : ::Engine::Scene::Scene::ObjectPlacement::After;
            }
            else
            {
                m_pendingTarget = nullptr;
                m_pendingPlacement = ::Engine::Scene::Scene::ObjectPlacement::AsChild;
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
}
