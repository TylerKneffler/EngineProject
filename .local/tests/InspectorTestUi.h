#pragma once
#include "Engine/Editor/UI/IEditorUi.h"
#include <string>
namespace Engine::Editor {
// Headless inspector driver: untouched widgets return no edit.
class InspectorTestUi final : public IEditorUi {
public:
    std::string clickButton;
    bool changeClip=false, changePlaying=false;
    void SetNextWindowRect(float x, float y, float width, float height) override {  }
    bool BeginWindow(const char* title, bool* open, bool noPadding) override { return {}; }
    bool BeginViewportOverlay(const char* id, float width, float height) override { return {}; }
    int ViewportSideTabs(const char* id, const char* const* labels, int count, int active, float panelWidth) override { return {}; }
    bool BeginViewportHeader(const char* id, float width) override { return {}; }
    bool EndViewportHeader() override { return {}; }
    bool BeginViewportHeaderDropdown(const char* id, const char* preview, float width, float maxHeight) override { return {}; }
    void EndViewportHeaderDropdown() override {  }
    void EndWindow() override {  }
    void PushId(const void* id) override {  }
    void PushId(const char* id) override {  }
    void PopId() override {  }
    bool Button(const char* label, float width, float height) override { if (clickButton==label) {clickButton.clear();return true;} return false; }
    EditorUiBreadcrumbResult Breadcrumb(const char* label, const char* const* childFolders, int childFolderCount) override { return {}; }
    void Label(const char* text) override {  }
    void DisabledLabel(const char* text) override {  }
    void ColoredLabel(const char* text, EditorUiColor color) override {  }
    void BeginTextWrap() override {  }
    void EndTextWrap() override {  }
    void SameLine() override {  }
    void Separator() override {  }
    void Spacing() override {  }
    void Indent(float width) override {  }
    void Unindent(float width) override {  }
    bool Checkbox(const char* label, bool* value) override { if (changePlaying && std::string(label)=="Playing") {changePlaying=false;*value=!*value;return true;} return false; }
    bool InputText(const char* label, char* buffer, size_t size) override { return {}; }
    bool InputTextSubmit(const char* label, char* buffer, size_t size) override { return {}; }
    void ReadOnlyTextBlock(const char* label, const char* text, bool scrollToBottom, float reservedBottom) override {  }
    bool DragFloat(const char* label, float* value, float speed, float minimum, float maximum) override { return {}; }
    bool DragFloat3(const char* label, float* values, float speed, float minimum, float maximum) override { return {}; }
    bool ColorEdit3(const char* label, float* color) override { return {}; }
    bool ColorEdit4(const char* label, float* color) override { return {}; }
    bool SliderInt(const char* label, int* value, int minimum, int maximum) override { return {}; }
    bool SliderFloat(const char* label, float* value, float minimum, float maximum) override { return {}; }
    bool InputUInt(const char* label, uint32_t* value) override { return {}; }
    void ValueLabel(const char* label, const char* value) override {  }
    bool CollapsingHeader(const char* label, bool defaultOpen) override { return {}; }
    bool TreeNode(const void* id, const char* label, bool selected, bool leaf, bool defaultOpen) override { return {}; }
    void TreePop() override {  }
    EditorUiObjectRowResult ObjectTreeRow(const void* id, EditorUiObjectIcon icon, char* name, size_t size, bool* enabled, bool selected, bool leaf, bool lockName, bool enabledInHierarchy, int hierarchyDepth, bool lastSibling, uint64_t ancestorGuideMask, bool expandForFilter, uint8_t* editorOverlays, uint8_t extraOverlayOptions) override { return {}; }
    void ObjectTreePop() override {  }
    EditorUiHierarchyDropResult HierarchyDropTarget(const char* type) override { return {}; }
    EditorUiHierarchyDropResult HierarchyBackgroundDropTarget(const char* type) override { return {}; }
    EditorUiObjectRowResult ObjectHeader(const void* id, char* name, size_t size, bool* enabled, bool lockName) override { return {}; }
    bool Selectable(const char* label, bool selected, bool allowDoubleClick) override { return {}; }
    EditorUiAssetTileResult AssetTile(const char* label, const char* fallbackIcon, void* texture, bool selected, float size) override { return {}; }
    float AvailableContentWidth() const override { return {}; }
    EditorUiContextMenuResult ContextMenu(const void* id, const char* addLabel, const char* deleteLabel, bool objectCreationMenu, const char* unpackLabel, bool clipboardActions, bool canPaste, bool propertiesAction, bool openRequested) override { return {}; }
    EditorUiAssetCreateMenuResult AssetWindowContextMenu(bool canPaste) override { return {}; }
    EditorUiAssetItemMenuResult AssetItemContextMenu(const char* assetPath, bool canPackMeshes, bool canExtractMeshes, bool isDirectory, bool canPaste, bool canExtractModelAssets) override { return {}; }
    EditorUiTextEditResult RenameText(const char* label, char* buffer, size_t size, bool focus) override { return {}; }
    EditorUiPrefabMenuResult PrefabOverrideMenu(const void* id, bool hasOverrides, bool canPaste, bool propertiesAction, bool openRequested) override { return {}; }
    bool BeginChild(const char* id) override { return {}; }
    void EndChild() override {  }
    bool IsItemHovered() const override { return {}; }
    bool IsItemClicked() const override { return {}; }
    bool IsItemDoubleClicked() const override { return {}; }
    bool IsWindowBackgroundClicked() const override { return {}; }
    bool CopyShortcutPressed() const override { return {}; }
    bool CutShortcutPressed() const override { return {}; }
    bool PasteShortcutPressed() const override { return {}; }
    bool DeleteShortcutPressed() const override { return {}; }
    bool BeginDragDropSource() override { return {}; }
    void SetDragDropPayload(const char* type, const void* data, size_t size) override {  }
    void EndDragDropSource() override {  }
    bool BeginDragDropTarget() override { return {}; }
    const void* AcceptDragDropPayload(const char* type, size_t* size) override { return {}; }
    EditorUiDragDropPayloadResult InspectDragDropPayload(const char* type) override { return {}; }
    EditorUiDragDropPayloadResult WindowDragDropTarget(const char* type) override { return {}; }
    void EndDragDropTarget() override {  }
    void SetClipboardText(const char* text) override {  }
    void ScrollToBottom() override {  }
    bool BeginTabBar(const char* id) override { return {}; }
    void EndTabBar() override {  }
    bool BeginTab(const char* label) override { return {}; }
    void EndTab() override {  }
    bool BeginTable(const char* id, int columns) override { return {}; }
    void TableSetupColumn(const char* label) override {  }
    void TableSetupCompactColumn(const char* label) override {  }
    void TableHeadersRow() override {  }
    void TableNextRow() override {  }
    void TableNextColumn() override {  }
    void EndTable() override {  }
    bool BindingColumnHeader(const char* id, const char* label, bool canDelete) override { return {}; }
    bool AddBindingColumnHeader(const char* id) override { return {}; }
    bool KeyBindingInput(const char* id, const char* display, std::string* key, bool* control, bool* shift, bool* alt) override { return {}; }
    void CancelKeyBindingCapture() override {  }
    void BeginDisabled(bool disabled) override {  }
    void EndDisabled() override {  }
    bool Combo(const char* label, int* selected, const char* const* items, int count) override { if (changeClip && std::string(label)=="Active Animation" && count>1) {changeClip=false;*selected=1;return true;} return false; }
    void SetNextItemWidth(float width) override {  }
    void Tooltip(const char* text) override {  }
    void Progress(float fraction, const char* overlay) override {  }
    void PercentageGrid(const char* id, const EditorUiPercentageSegment* segments, size_t segmentCount, float width) override {  }
    void UsageHistory(const char* id, const float* values, size_t count, float minimum, float maximum, EditorUiColor color, float width, float height) override {  }
    float FrameRate() const override { return {}; }
    void DrawImage(void* texture, float width, float height) override {  }
    EditorUiViewportInput Viewport(void* texture, float aspectRatio, EditorUiColor letterboxColor) override { return {}; }
    void DrawViewportLine(EditorUiVec2 start, EditorUiVec2 end, EditorUiColor color, float thickness) override {  }
    void DrawViewportTriangle(EditorUiVec2 first, EditorUiVec2 second, EditorUiVec2 third, EditorUiColor color) override {  }
    void DrawViewportCircle(EditorUiVec2 center, float radius, EditorUiColor color, bool filled, float thickness) override {  }
    void DrawViewportText(EditorUiVec2 position, const char* text, EditorUiColor color) override {  }
    void FocusWindow(const char* title) override {  }
    void DrawCircularImage(void* texture, float diameter, EditorUiColor border) override {  }
    bool PropertyGroupHeader(const char*, bool) override {return false;}
};
}
