#pragma once
#include "Engine/Editor/UI/IEditorUi.h"

class ImGuiEditorUi final : public IEditorUi
{
public:
    void SetNextWindowRect(float x, float y, float width, float height) override;
    bool BeginWindow(const char*, bool*, bool) override; void EndWindow() override;
    bool Button(const char*, float, float) override; void Label(const char*) override;
    void DisabledLabel(const char*) override; void ColoredLabel(const char*, EditorUiColor) override;
    void SameLine() override; void Separator() override; void Spacing() override;
    bool Checkbox(const char*, bool*) override; bool InputText(const char*, char*, size_t) override;
    bool DragFloat(const char*, float*, float, float, float) override;
    bool DragFloat3(const char*, float*, float, float, float) override;
    bool ColorEdit3(const char*, float*) override; bool ColorEdit4(const char*, float*) override;
    bool SliderInt(const char*, int*, int, int) override; bool InputUInt(const char*, uint32_t*) override;
    void ValueLabel(const char*, const char*) override; bool CollapsingHeader(const char*, bool) override;
    bool TreeNode(const void*, const char*, bool, bool, bool) override; void TreePop() override;
    bool Selectable(const char*, bool, bool) override; bool BeginChild(const char*) override; void EndChild() override;
    bool IsItemHovered() const override; bool IsItemClicked() const override;
    bool IsItemDoubleClicked() const override; bool IsWindowBackgroundClicked() const override;
    void SetClipboardText(const char*) override; void ScrollToBottom() override;
    bool BeginTabBar(const char*) override; void EndTabBar() override;
    bool BeginTab(const char*) override; void EndTab() override;
    void BeginDisabled(bool) override; void EndDisabled() override;
    bool Combo(const char*, int*, const char* const*, int) override;
    void Tooltip(const char*) override; void Progress(float, const char*) override;
    EditorUiViewportInput Viewport(void*, float, EditorUiColor) override;
    void FocusWindow(const char*) override;
};
