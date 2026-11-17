#pragma once

#include <cstddef>
#include <cstdint>

// Avoid Windows macro collisions with method names in this interface.
#ifdef Combo
#undef Combo
#endif
#ifdef Tooltip
#undef Tooltip
#endif

namespace Engine::Core
{
struct InspectorColor { float r = 1.f, g = 1.f, b = 1.f, a = 1.f; };

// The widget surface components use to draw their inspector properties.
// Core depends only on this interface; the editor's IEditorUi extends it with
// windows, trees, asset browsing and viewport controls. Keep this limited to
// what component inspectors need.
class IInspectorUi
{
public:
    virtual ~IInspectorUi() = default;

    // ---- Layout and identity -------------------------------------------------
    virtual void PushId(const void* id) = 0;
    virtual void PushId(const char* id) = 0;
    virtual void PopId() = 0;
    virtual void SameLine() = 0;
    // Continues the current row with an item group aligned to the right edge.
    virtual void SameLineRight(float) { SameLine(); }
    virtual void Separator() = 0;
    virtual void Indent(float width = 0.f) = 0;
    virtual void Unindent(float width = 0.f) = 0;
    virtual bool CollapsingHeader(const char* label, bool defaultOpen = true) = 0;
    virtual bool PropertyGroupHeader(const char* label,
        bool defaultOpen = true) { (void)defaultOpen; Label(label); return true; }
    virtual void BeginDisabled(bool disabled = true) = 0;
    virtual void EndDisabled() = 0;

    // ---- Text ----------------------------------------------------------------
    virtual void Label(const char* text) = 0;
    virtual void DisabledLabel(const char* text) = 0;
    virtual void ColoredLabel(const char* text, InspectorColor color) = 0;
    virtual void ValueLabel(const char* label, const char* value) = 0;
    virtual void Tooltip(const char* text) = 0;

    // ---- Value widgets -------------------------------------------------------
    // Marks the next value widget as representing differing values across a
    // multi-selection. Backends should render a dash until the user edits it.
    virtual void SetNextItemMixedValue(bool) {}
    virtual bool Button(const char* label, float width = 0.f, float height = 0.f) = 0;
    virtual bool Checkbox(const char* label, bool* value) = 0;
    virtual bool InputText(const char* label, char* buffer, size_t size) = 0;
    virtual bool InputTextSubmit(const char* label, char* buffer, size_t size) = 0;
    virtual bool InputUInt(const char* label, uint32_t* value) = 0;
    virtual bool DragFloat(const char* label, float* value, float speed, float minimum = 0.f, float maximum = 0.f) = 0;
    virtual bool DragFloat3(const char* label, float* values, float speed, float minimum = 0.f, float maximum = 0.f) = 0;
    virtual bool ColorEdit3(const char* label, float* color) = 0;
    virtual bool SliderInt(const char* label, int* value, int minimum, int maximum) = 0;
    virtual bool SliderFloat(const char* label, float* value, float minimum, float maximum) = 0;
    // Ordered distance boundaries; segmentCount is boundaryCount + 1.
    virtual bool LodRangeSlider(const char* label, float* boundaries,
        int boundaryCount, float maximum, const char* const* segmentLabels)
    {
        Label(label);
        bool changed = false;
        for (int i = 0; i < boundaryCount; ++i)
            changed |= SliderFloat(segmentLabels[i + 1], &boundaries[i],
                i ? boundaries[i - 1] : 0.f,
                i + 1 < boundaryCount ? boundaries[i + 1] : maximum);
        return changed;
    }
    virtual bool Combo(const char* label, int* selected, const char* const* items, int count) = 0;
    virtual bool Selectable(const char* label, bool selected = false, bool allowDoubleClick = false) = 0;

    // ---- Images --------------------------------------------------------------
    virtual void DrawImage(void* texture, float width, float height) = 0;
    // Draws a square texture as a circular item while preserving normal
    // hover/click/drag-drop behavior for the preview.
    virtual void DrawCircularImage(void* texture, float diameter,
        InspectorColor border = { 0.35f, 0.35f, 0.35f, 1.f }) = 0;

    // ---- Item state and drag-drop targets -----------------------------------
    virtual bool IsItemHovered() const = 0;
    virtual bool IsItemClicked() const = 0;
    virtual bool BeginDragDropTarget() = 0;
    virtual const void* AcceptDragDropPayload(const char* type, size_t* size = nullptr) = 0;
    virtual void EndDragDropTarget() = 0;
};
}
