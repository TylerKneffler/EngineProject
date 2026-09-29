#include "Core/Compoonents/UI/UIText.h"

namespace Engine::Components
{
UIText::UIText()
{
    SetTypeName(COMPONENT_TYPE_NAME(UIText));
    singlecomponent = true;
    RegisterField("text", text, "Content");
    RegisterField("fontPath", fontPath, "Typography");
    RegisterField("fontSize", fontSize, "Typography");
    RegisterField("color", color, "Appearance");
    RegisterField("alpha", alpha, "Appearance");
    RegisterField("horizontalAlignment", horizontalAlignment, "Layout");
    RegisterField("verticalAlignment", verticalAlignment, "Layout");
    RegisterField("wordWrap", wordWrap, "Layout");
    RegisterField("overflow", overflow, "Layout");
    RegisterField("lineSpacing", lineSpacing, "Layout");
}
}
