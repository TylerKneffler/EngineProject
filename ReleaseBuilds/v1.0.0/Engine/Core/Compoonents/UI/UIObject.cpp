#include "Core/Compoonents/UI/UIObject.h"

namespace Engine::Components
{
UIObject::UIObject()
{
    SetTypeName(COMPONENT_TYPE_NAME(UIObject));
    singlecomponent = true;
    RegisterField("anchorMin", anchorMin, "Rect");
    RegisterField("anchorMax", anchorMax, "Rect");
    RegisterField("pivot", pivot, "Rect");
    RegisterField("anchoredPosition", anchoredPosition, "Rect");
    RegisterField("sizeDelta", sizeDelta, "Rect");
    RegisterField("minWidth", minWidth, "Size Constraints", false);
    RegisterField("minHeight", minHeight, "Size Constraints", false);
    RegisterField("maxWidth", maxWidth, "Size Constraints", false);
    RegisterField("maxHeight", maxHeight, "Size Constraints", false);
    RegisterField("marginLeft", marginLeft, "Spacing", false);
    RegisterField("marginTop", marginTop, "Spacing", false);
    RegisterField("marginRight", marginRight, "Spacing", false);
    RegisterField("marginBottom", marginBottom, "Spacing", false);
    RegisterField("paddingLeft", paddingLeft, "Spacing", false);
    RegisterField("paddingTop", paddingTop, "Spacing", false);
    RegisterField("paddingRight", paddingRight, "Spacing", false);
    RegisterField("paddingBottom", paddingBottom, "Spacing", false);
    RegisterField("layoutDirection", layoutDirection, "Layout");
    RegisterField("justifyContent", justifyContent, "Layout");
    RegisterField("alignItems", alignItems, "Layout");
    RegisterField("spacing", spacing, "Layout");
    RegisterField("flexGrow", flexGrow, "Layout");
    RegisterField("clipChildren", clipChildren, "Layout");
    RegisterField("visible", visible, "Display");
    RegisterField("zOrder", zOrder, "Display");
}
}
