#pragma once

#include <cstddef>
#include <cstdint>

namespace Engine::Editor
{
// Avoid Windows macro collisions with method names in this interface.
#ifdef Combo
#undef Combo
#endif
#ifdef Tooltip
#undef Tooltip
#endif

struct EditorUiVec2 { float x = 0.f, y = 0.f; };
struct EditorUiColor { float r = 1.f, g = 1.f, b = 1.f, a = 1.f; };
struct EditorUiViewportInput
{
    EditorUiVec2 available;
    EditorUiVec2 mouseDelta;
    EditorUiVec2 mousePosInViewport;
    float mouseWheel = 0.f;
    bool hovered = false;
    bool leftClicked = false;
    bool leftDown = false;
    bool leftReleased = false;
    bool rightDown = false;
    bool middleDown = false;
};

enum class EditorUiHierarchyDropPosition { None, Before, AsChild, After };

struct EditorUiHierarchyDropResult
{
    EditorUiHierarchyDropPosition position = EditorUiHierarchyDropPosition::None;
    const void* data = nullptr;
    size_t size = 0;
};

struct EditorUiObjectRowResult
{
    bool open = false;
    bool clicked = false;
    bool doubleClicked = false;
    bool nameChanged = false;
    bool enabledChanged = false;
    const void* droppedItem = nullptr;
    EditorUiHierarchyDropPosition dropPosition = EditorUiHierarchyDropPosition::None;
    bool dragActive = false;
    bool dropHovered = false;
    int dropDepth = -1;
};

struct EditorUiContextMenuResult
{
    bool addRequested = false;
    bool addCubeRequested = false;
    bool addSpriteRequested = false;
    bool unpackRequested = false;
    bool deleteRequested = false;
};

struct EditorUiPrefabMenuResult
{
    bool applyRequested = false;
    bool applyAllRequested = false;
    bool revertRequested = false;
    bool unpackRequested = false;
};

struct EditorUiDragDropPayloadResult
{
    const void* data = nullptr;
    size_t size = 0;
    bool delivered = false;
};

struct EditorUiAssetCreateMenuResult
{
    bool folderRequested = false;
    bool scriptRequested = false;
};

struct EditorUiTextEditResult
{
    bool submitted = false;
    bool deactivated = false;
};

// Package-neutral immediate UI facade used throughout Editor/Core/View.
class IEditorUi
{
public:
    virtual ~IEditorUi() = default;
    virtual void SetNextWindowRect(float x, float y, float width, float height) = 0;
    virtual bool BeginWindow(const char* title, bool* open, bool noPadding = false) = 0;
    virtual void EndWindow() = 0;
    virtual bool IsWindowFocused() const { return false; }
    virtual void PushId(const void* id) = 0;
    virtual void PushId(const char* id) = 0;
    virtual void PopId() = 0;
    virtual bool Button(const char* label, float width = 0.f, float height = 0.f) = 0;
    virtual void Label(const char* text) = 0;
    virtual void DisabledLabel(const char* text) = 0;
    virtual void ColoredLabel(const char* text, EditorUiColor color) = 0;
    virtual void BeginTextWrap() = 0;
    virtual void EndTextWrap() = 0;
    virtual void SameLine() = 0;
    virtual void Separator() = 0;
    virtual void Spacing() = 0;
    virtual void Indent(float width = 0.f) = 0;
    virtual void Unindent(float width = 0.f) = 0;
    virtual bool Checkbox(const char* label, bool* value) = 0;
    virtual bool InputText(const char* label, char* buffer, size_t size) = 0;
    virtual bool InputTextSubmit(const char* label, char* buffer, size_t size) = 0;
    virtual void ReadOnlyTextBlock(const char* label, const char* text,
        bool scrollToBottom = false, float reservedBottom = 0.f) = 0;
    virtual bool DragFloat(const char* label, float* value, float speed, float minimum = 0.f, float maximum = 0.f) = 0;
    virtual bool DragFloat3(const char* label, float* values, float speed, float minimum = 0.f, float maximum = 0.f) = 0;
    virtual bool ColorEdit3(const char* label, float* color) = 0;
    virtual bool ColorEdit4(const char* label, float* color) = 0;
    virtual bool SliderInt(const char* label, int* value, int minimum, int maximum) = 0;
    virtual bool InputUInt(const char* label, uint32_t* value) = 0;
    virtual void ValueLabel(const char* label, const char* value) = 0;
    virtual bool CollapsingHeader(const char* label, bool defaultOpen = true) = 0;
    virtual bool TreeNode(const void* id, const char* label, bool selected, bool leaf, bool defaultOpen = false) = 0;
    virtual void TreePop() = 0;
    virtual EditorUiObjectRowResult ObjectTreeRow(const void* id, char* name, size_t size,
        bool* enabled, bool selected, bool leaf, bool lockName,
        bool enabledInHierarchy, int hierarchyDepth, bool lastSibling,
        uint64_t ancestorGuideMask) = 0;
    virtual void ObjectTreePop() = 0;
    virtual EditorUiHierarchyDropResult HierarchyDropTarget(const char* type) = 0;
    virtual EditorUiHierarchyDropResult HierarchyBackgroundDropTarget(const char* type) = 0;
    virtual EditorUiObjectRowResult ObjectHeader(const void* id, char* name, size_t size,
        bool* enabled, bool lockName) = 0;
    virtual bool Selectable(const char* label, bool selected = false, bool allowDoubleClick = false) = 0;
    virtual EditorUiContextMenuResult ContextMenu(const void* id,
        const char* addLabel, const char* deleteLabel,
        bool objectCreationMenu = false,
        const char* unpackLabel = nullptr) = 0;
    virtual EditorUiAssetCreateMenuResult AssetWindowContextMenu() = 0;
    virtual EditorUiTextEditResult RenameText(const char* label, char* buffer,
        size_t size, bool focus) = 0;
    virtual EditorUiPrefabMenuResult PrefabOverrideMenu(const void* id,
        bool hasOverrides) = 0;
    virtual bool BeginChild(const char* id) = 0;
    virtual void EndChild() = 0;
    virtual bool IsItemHovered() const = 0;
    virtual bool IsItemClicked() const = 0;
    virtual bool IsItemDoubleClicked() const = 0;
    virtual bool IsWindowBackgroundClicked() const = 0;
    virtual bool CopyShortcutPressed() const = 0;
    virtual bool PasteShortcutPressed() const = 0;
    virtual bool BeginDragDropSource() = 0;
    virtual void SetDragDropPayload(const char* type, const void* data, size_t size) = 0;
    virtual void EndDragDropSource() = 0;
    virtual bool BeginDragDropTarget() = 0;
    virtual const void* AcceptDragDropPayload(const char* type, size_t* size = nullptr) = 0;
    virtual EditorUiDragDropPayloadResult InspectDragDropPayload(const char* type) = 0;
    virtual EditorUiDragDropPayloadResult WindowDragDropTarget(const char* type) = 0;
    virtual void EndDragDropTarget() = 0;
    virtual void SetClipboardText(const char* text) = 0;
    virtual void ScrollToBottom() = 0;
    virtual bool BeginTabBar(const char* id) = 0;
    virtual void EndTabBar() = 0;
    virtual bool BeginTab(const char* label) = 0;
    virtual void EndTab() = 0;
    virtual void BeginDisabled(bool disabled = true) = 0;
    virtual void EndDisabled() = 0;
    virtual bool Combo(const char* label, int* selected, const char* const* items, int count) = 0;
    virtual void Tooltip(const char* text) = 0;
    virtual void Progress(float fraction, const char* overlay = nullptr) = 0;
    virtual void DrawImage(void* texture, float width, float height) = 0;
    virtual EditorUiViewportInput Viewport(void* texture, float aspectRatio, EditorUiColor letterboxColor) = 0;
    virtual void DrawViewportLine(EditorUiVec2 start, EditorUiVec2 end,
        EditorUiColor color, float thickness = 1.f) = 0;
    virtual void DrawViewportTriangle(EditorUiVec2 first, EditorUiVec2 second,
        EditorUiVec2 third, EditorUiColor color) = 0;
    virtual void DrawViewportCircle(EditorUiVec2 center, float radius,
        EditorUiColor color, bool filled = true, float thickness = 1.f) = 0;
    virtual void DrawViewportText(EditorUiVec2 position, const char* text,
        EditorUiColor color) = 0;
    virtual void FocusWindow(const char* title) = 0;
};
}
