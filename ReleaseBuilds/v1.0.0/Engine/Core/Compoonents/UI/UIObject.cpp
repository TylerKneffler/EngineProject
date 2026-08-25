#include "Core/Compoonents/UI/UIObject.h"

namespace Engine::Components
{
UIObject::UIObject()
{
    SetTypeName(COMPONENT_TYPE_NAME(UIObject));
    singlecomponent = true;
    RegisterField("anchorMin", anchorMin);
    RegisterField("anchorMax", anchorMax);
    RegisterField("pivot", pivot);
    RegisterField("anchoredPosition", anchoredPosition);
    RegisterField("sizeDelta", sizeDelta);
    RegisterField("minWidth", minWidth);
    RegisterField("minHeight", minHeight);
    RegisterField("maxWidth", maxWidth);
    RegisterField("maxHeight", maxHeight);
    RegisterField("marginLeft", marginLeft);
    RegisterField("marginTop", marginTop);
    RegisterField("marginRight", marginRight);
    RegisterField("marginBottom", marginBottom);
    RegisterField("paddingLeft", paddingLeft);
    RegisterField("paddingTop", paddingTop);
    RegisterField("paddingRight", paddingRight);
    RegisterField("paddingBottom", paddingBottom);
    RegisterField("layoutDirection", layoutDirection);
    RegisterField("justifyContent", justifyContent);
    RegisterField("alignItems", alignItems);
    RegisterField("spacing", spacing);
    RegisterField("flexGrow", flexGrow);
    RegisterField("clipChildren", clipChildren);
    RegisterField("visible", visible);
    RegisterField("zOrder", zOrder);
}
}
