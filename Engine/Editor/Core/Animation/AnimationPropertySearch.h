#pragma once
#include "Editor/Core/Animation/AnimationDocumentAccess.h"
#include "imgui.h"
#include <cstring>

namespace Engine::Editor::AnimationPropertySearch
{
template<class Document>
inline
bool AnimationPropertiesHeader(Document& document, const char* label,
    ImGuiTreeNodeFlags flags = 0)
{
    if (!document.drawingProperties) return ImGui::CollapsingHeader(label, flags);
    const std::string filter = AnimationDocumentAccess::LowerText(
        document.propertiesSearch);
    const bool childMatch = std::strcmp(label, "Selected bone") == 0 &&
        (!filter.empty() && AnimationDocumentAccess::LowerText(
            "Two-bone IK").find(filter) != std::string::npos);
    if (!filter.empty() && AnimationDocumentAccess::LowerText(label).find(
            filter) == std::string::npos && !childMatch)
        return false;
    const bool jump = document.propertiesJumpTarget == label;
    const bool childJump = std::strcmp(label, "Selected bone") == 0 &&
        document.propertiesJumpTarget == "Two-bone IK";
    if (document.propertiesHeaderOverride >= 0 || !filter.empty() || jump || childJump)
        ImGui::SetNextItemOpen(document.propertiesHeaderOverride >= 0
            ? document.propertiesHeaderOverride != 0 : true, ImGuiCond_Always);
    const bool open = ImGui::CollapsingHeader(label, flags);
    if (jump)
    {
        ImGui::SetScrollHereY(.15f);
        document.propertiesJumpTarget.clear();
    }
    return open;
}
}
