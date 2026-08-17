#pragma once
#include "Engine/Editor/UI/IEditorUi.h"
#include <unordered_map>
#include <vector>

namespace Engine::Editor
{
class ImGuiEditorUi final : public IEditorUi
{
public:
    void SetNextWindowRect(float x, float y, float width, float height) override;
    bool BeginWindow(const char*, bool*, bool) override; void EndWindow() override;
    bool IsWindowFocused() const override;
    void PushId(const void*) override; void PushId(const char*) override; void PopId() override;
    bool Button(const char*, float, float) override; void Label(const char*) override;
    void DisabledLabel(const char*) override; void ColoredLabel(const char*, EditorUiColor) override;
    void BeginTextWrap() override; void EndTextWrap() override;
    void SameLine() override; void Separator() override; void Spacing() override;
    void Indent(float) override; void Unindent(float) override;
    bool Checkbox(const char*, bool*) override; bool InputText(const char*, char*, size_t) override;
    bool InputTextSubmit(const char*, char*, size_t) override;
    void ReadOnlyTextBlock(const char*, const char*, bool, float) override;
    bool DragFloat(const char*, float*, float, float, float) override;
    bool DragFloat3(const char*, float*, float, float, float) override;
    bool ColorEdit3(const char*, float*) override; bool ColorEdit4(const char*, float*) override;
    bool SliderInt(const char*, int*, int, int) override; bool InputUInt(const char*, uint32_t*) override;
    void ValueLabel(const char*, const char*) override; bool CollapsingHeader(const char*, bool) override;
    bool TreeNode(const void*, const char*, bool, bool, bool) override; void TreePop() override;
    EditorUiObjectRowResult ObjectTreeRow(const void*,char*,size_t,bool*,bool,bool,bool,bool,int,bool,uint64_t) override;
    void ObjectTreePop() override;
    EditorUiHierarchyDropResult HierarchyDropTarget(const char*) override;
    EditorUiHierarchyDropResult HierarchyBackgroundDropTarget(const char*) override;
    EditorUiObjectRowResult ObjectHeader(const void*,char*,size_t,bool*,bool) override;
    bool Selectable(const char*, bool, bool) override;
    EditorUiContextMenuResult ContextMenu(const void*,const char*,const char*,bool,const char*) override;
    EditorUiAssetCreateMenuResult AssetWindowContextMenu() override;
    EditorUiTextEditResult RenameText(const char*,char*,size_t,bool) override;
    EditorUiPrefabMenuResult PrefabOverrideMenu(const void*,bool) override;
    bool BeginChild(const char*) override; void EndChild() override;
    bool IsItemHovered() const override; bool IsItemClicked() const override;
    bool IsItemDoubleClicked() const override; bool IsWindowBackgroundClicked() const override;
    bool IsAnyItemActive() const override;
    bool CopyShortcutPressed() const override; bool PasteShortcutPressed() const override;
    bool BeginDragDropSource() override; void SetDragDropPayload(const char*,const void*,size_t) override; void EndDragDropSource() override;
    bool BeginDragDropTarget() override; const void* AcceptDragDropPayload(const char*,size_t*) override; void EndDragDropTarget() override;
    EditorUiDragDropPayloadResult InspectDragDropPayload(const char*) override;
    EditorUiDragDropPayloadResult WindowDragDropTarget(const char*) override;
    void SetClipboardText(const char*) override; void ScrollToBottom() override;
    bool BeginTabBar(const char*) override; void EndTabBar() override;
    bool BeginTab(const char*) override; void EndTab() override;
    bool BeginTable(const char*, int) override; void TableSetupColumn(const char*) override;
    void TableSetupCompactColumn(const char*) override;
    void TableHeadersRow() override; void TableNextRow() override;
    void TableNextColumn() override; void EndTable() override;
    bool BindingColumnHeader(const char*, const char*, bool) override;
    bool AddBindingColumnHeader(const char*) override;
    bool KeyBindingInput(const char*, const char*, std::string*, bool*, bool*, bool*) override;
    void CancelKeyBindingCapture() override;
    void BeginDisabled(bool) override; void EndDisabled() override;
    bool Combo(const char*, int*, const char* const*, int) override;
    void Tooltip(const char*) override; void Progress(float, const char*) override;
    void DrawImage(void*, float, float) override;
    EditorUiViewportInput Viewport(void*, float, EditorUiColor) override;
    void DrawViewportLine(EditorUiVec2, EditorUiVec2, EditorUiColor, float) override;
    void DrawViewportTriangle(EditorUiVec2, EditorUiVec2, EditorUiVec2, EditorUiColor) override;
    void DrawViewportCircle(EditorUiVec2, float, EditorUiColor, bool, float) override;
    void DrawViewportText(EditorUiVec2, const char*, EditorUiColor) override;
    void FocusWindow(const char*) override;
private:
    std::vector<unsigned char> m_dropResultPayload;
    std::unordered_map<const void*, bool> m_objectTreeOpen;
    std::unordered_map<const void*, bool> m_objectHadChildren;
    EditorUiVec2 m_viewportScreenMin{};
    EditorUiVec2 m_viewportScreenMax{};
    void* m_capturedViewportTexture = nullptr;
    unsigned int m_bindingCaptureId = 0;
};
}
