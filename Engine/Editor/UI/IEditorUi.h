#pragma once

#include <cstddef>
#include <cstdint>

struct EditorUiVec2 { float x = 0.f, y = 0.f; };
struct EditorUiColor { float r = 1.f, g = 1.f, b = 1.f, a = 1.f; };
struct EditorUiViewportInput
{
    EditorUiVec2 available;
    EditorUiVec2 mouseDelta;
    float mouseWheel = 0.f;
    bool hovered = false;
    bool rightDown = false;
    bool middleDown = false;
};

// Package-neutral immediate UI facade used throughout Editor/Core/View.
class IEditorUi
{
public:
    virtual ~IEditorUi() = default;
    virtual void SetNextWindowRect(float x, float y, float width, float height) = 0;
    virtual bool BeginWindow(const char* title, bool* open, bool noPadding = false) = 0;
    virtual void EndWindow() = 0;
    virtual bool Button(const char* label, float width = 0.f, float height = 0.f) = 0;
    virtual void Label(const char* text) = 0;
    virtual void DisabledLabel(const char* text) = 0;
    virtual void ColoredLabel(const char* text, EditorUiColor color) = 0;
    virtual void SameLine() = 0;
    virtual void Separator() = 0;
    virtual void Spacing() = 0;
    virtual bool Checkbox(const char* label, bool* value) = 0;
    virtual bool InputText(const char* label, char* buffer, size_t size) = 0;
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
    virtual bool Selectable(const char* label, bool selected = false, bool allowDoubleClick = false) = 0;
    virtual bool BeginChild(const char* id) = 0;
    virtual void EndChild() = 0;
    virtual bool IsItemHovered() const = 0;
    virtual bool IsItemClicked() const = 0;
    virtual bool IsItemDoubleClicked() const = 0;
    virtual bool IsWindowBackgroundClicked() const = 0;
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
    virtual EditorUiViewportInput Viewport(void* texture, float aspectRatio, EditorUiColor letterboxColor) = 0;
    virtual void FocusWindow(const char* title) = 0;
};
