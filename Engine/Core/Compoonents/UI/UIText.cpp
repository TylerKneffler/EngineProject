#include "Core/Compoonents/UI/UIText.h"

UIText::UIText()
{
    SetTypeName(COMPONENT_TYPE_NAME(UIText));
    singlecomponent = true;
    RegisterField("text", text);
    RegisterField("fontPath", fontPath);
    RegisterField("fontSize", fontSize);
    RegisterField("color", color);
    RegisterField("alpha", alpha);
    RegisterField("horizontalAlignment", horizontalAlignment);
    RegisterField("verticalAlignment", verticalAlignment);
    RegisterField("wordWrap", wordWrap);
    RegisterField("overflow", overflow);
    RegisterField("lineSpacing", lineSpacing);
}
