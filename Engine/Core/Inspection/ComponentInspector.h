#pragma once

#include <string>
#include <vector>

namespace Engine::Core
{
class Component;
class Object;
class IInspectorUi;
struct ComponentReference;

// Draws component properties for the editor. Holds the editor-session state
// (in-progress path edits, asset/scene searches, viewport reference picking)
// so Component itself stays a runtime type.
class ComponentInspector
{
public:
    // Generic inspector built from the component's serialized fields. With
    // multiEditTargets, mixed values are flagged and each edited field is
    // applied to every target.
    static bool DrawGeneric(Component& component, IInspectorUi& ui,
        const std::vector<Component*>* multiEditTargets = nullptr);

    // Scene target control for ComponentReference fields.
    static bool DrawReferenceProperty(Component& owner, IInspectorUi& ui,
        const char* fieldKey, const char* label,
        ComponentReference& reference,
        const Component* defaultTarget = nullptr,
        const char* defaultDescription = nullptr);

    // Asset path control with drag-drop and project/engine asset search.
    static bool DrawAssetPathPicker(IInspectorUi& ui, const void* context,
        const char* fieldKey, const char* label, const char* assetKind,
        std::string& path);

    // ---- Editor session -------------------------------------------------------
    // Asset searches cover the active project's asset folder.
    static void SetAssetDirectory(const std::string& path);
    static void ClearReferenceSearches();
    // A reference field can temporarily capture hierarchy or viewport picks.
    static bool HasReferencePick();
    static void CancelReferencePick();
    static bool CaptureReferencePick(Object* object);
};
}
