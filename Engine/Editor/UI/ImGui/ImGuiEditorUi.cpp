#include "pch.h"
#include "ImGuiEditorUi.h"
#include "Engine/Editor/Core/View/IEditorPanel.h"
#include "Engine/Editor/Core/PrimitiveObjectFactory.h"
#include "Engine/Editor/Input/EditorKeyBindings.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <string>

namespace Engine::Editor
{
namespace
{
constexpr float viewportTabWidth = 36.f;

float ViewportToolsTop(const EditorUiVec2& minimum,
    const EditorUiVec2& maximum)
{
    const float viewportHeight = std::max(0.f, maximum.y - minimum.y);
    return minimum.y + std::min(80.f, std::max(8.f, viewportHeight * 0.22f));
}

struct InteractionAnimation
{
    float value = 0.f;
    int lastFrame = 0;
};

float AnimateInteraction(ImGuiID id, bool active, float speed = 14.f)
{
    static std::unordered_map<ImGuiID, InteractionAnimation> animations;
    static int lastCleanupFrame = -1;
    InteractionAnimation& animation = animations[id];
    animation.lastFrame = ImGui::GetFrameCount();
    const float target = active ? 1.f : 0.f;
    const float blend = 1.f - std::exp(-speed * ImGui::GetIO().DeltaTime);
    animation.value = ImLerp(animation.value, target, blend);

    if ((ImGui::GetFrameCount() % 240) == 0 &&
        lastCleanupFrame != ImGui::GetFrameCount())
    {
        lastCleanupFrame = ImGui::GetFrameCount();
        for (auto iterator = animations.begin(); iterator != animations.end();)
            if (iterator->second.lastFrame + 240 < ImGui::GetFrameCount())
                iterator = animations.erase(iterator);
            else
                ++iterator;
    }
    return animation.value;
}

void DrawObjectIcon(ImDrawList* draw, EditorUiObjectIcon icon,
    ImVec2 center, float scale, ImU32 color)
{
    const float radius = 5.5f * scale;
    const float line = 1.35f;
    switch (icon)
    {
    case EditorUiObjectIcon::Light:
        draw->AddCircle({ center.x, center.y - 1.5f * scale },
            3.1f * scale, color, 12, line);
        draw->AddLine({ center.x - 2.f * scale, center.y + 2.f * scale },
            { center.x + 2.f * scale, center.y + 2.f * scale }, color, line);
        draw->AddLine({ center.x - 1.5f * scale, center.y + 4.f * scale },
            { center.x + 1.5f * scale, center.y + 4.f * scale }, color, line);
        for (int ray = 0; ray < 5; ++ray)
        {
            const float angle = -2.75f + ray * 1.375f;
            const ImVec2 direction{ std::cos(angle), std::sin(angle) };
            draw->AddLine({ center.x + direction.x * 4.5f * scale,
                            center.y - 1.5f * scale + direction.y * 4.5f * scale },
                { center.x + direction.x * 6.f * scale,
                  center.y - 1.5f * scale + direction.y * 6.f * scale }, color, line);
        }
        break;
    case EditorUiObjectIcon::Camera:
        draw->AddRect({ center.x - radius, center.y - 3.8f * scale },
            { center.x + 2.f * scale, center.y + 3.8f * scale }, color,
            1.5f * scale, 0, line);
        draw->AddTriangle({ center.x + 2.f * scale, center.y - 2.5f * scale },
            { center.x + radius, center.y - 4.5f * scale },
            { center.x + radius, center.y + 4.5f * scale }, color, line);
        break;
    case EditorUiObjectIcon::Audio:
        draw->AddTriangleFilled({ center.x - radius, center.y - 2.f * scale },
            { center.x - 2.f * scale, center.y - 2.f * scale },
            { center.x + 1.f * scale, center.y - radius }, color);
        draw->AddTriangleFilled({ center.x - radius, center.y + 2.f * scale },
            { center.x - 2.f * scale, center.y + 2.f * scale },
            { center.x + 1.f * scale, center.y + radius }, color);
        draw->PathArcTo(center, 3.5f * scale, -.75f, .75f, 8);
        draw->PathStroke(color, 0, line);
        draw->PathArcTo(center, 6.f * scale, -.75f, .75f, 8);
        draw->PathStroke(color, 0, line);
        break;
    case EditorUiObjectIcon::Object:
    case EditorUiObjectIcon::Mesh:
    {
        const ImVec2 back{ center.x, center.y - 5.f * scale };
        const ImVec2 leftTop{ center.x - 5.f * scale,
            center.y - 2.f * scale };
        const ImVec2 frontTop{ center.x, center.y + 1.f * scale };
        const ImVec2 rightTop{ center.x + 5.f * scale,
            center.y - 2.f * scale };
        const ImVec2 leftBottom{ leftTop.x, leftTop.y + 5.f * scale };
        const ImVec2 frontBottom{ frontTop.x,
            frontTop.y + 5.f * scale };
        const ImVec2 rightBottom{ rightTop.x,
            rightTop.y + 5.f * scale };
        const ImU32 rgb = color & 0x00FFFFFFu;
        const unsigned alpha = color >> 24;
        draw->AddQuadFilled(back, leftTop, frontTop, rightTop,
            rgb | ((alpha * 3u / 10u) << 24));
        draw->AddQuadFilled(leftTop, frontTop, frontBottom, leftBottom,
            rgb | ((alpha * 2u / 10u) << 24));
        draw->AddQuadFilled(frontTop, rightTop, rightBottom, frontBottom,
            rgb | ((alpha / 10u) << 24));
        draw->AddLine(back, leftTop, color, line);
        draw->AddLine(back, rightTop, color, line);
        draw->AddLine(leftTop, frontTop, color, line);
        draw->AddLine(frontTop, rightTop, color, line);
        draw->AddLine(leftTop, leftBottom, color, line);
        draw->AddLine(frontTop, frontBottom, color, line);
        draw->AddLine(rightTop, rightBottom, color, line);
        draw->AddLine(leftBottom, frontBottom, color, line);
        draw->AddLine(frontBottom, rightBottom, color, line);
        break;
    }
    case EditorUiObjectIcon::Sprite:
        draw->AddRect({ center.x - radius, center.y - radius * .8f },
            { center.x + radius, center.y + radius * .8f }, color, 1.f, 0, line);
        draw->AddCircleFilled({ center.x + 2.8f * scale, center.y - 2.2f * scale },
            1.2f * scale, color);
        draw->AddTriangle({ center.x - 4.5f * scale, center.y + 3.5f * scale },
            { center.x - 1.f * scale, center.y },
            { center.x + 4.5f * scale, center.y + 3.5f * scale }, color, line);
        break;
    case EditorUiObjectIcon::Physics:
        draw->AddCircle(center, radius, color, 14, line);
        draw->AddCircleFilled(center, 1.6f * scale, color);
        draw->AddLine({ center.x - radius, center.y },
            { center.x + radius, center.y }, color, line);
        break;
    case EditorUiObjectIcon::UserInterface:
        draw->AddRect({ center.x - radius, center.y - radius * .8f },
            { center.x + radius, center.y + radius * .8f }, color, 1.f, 0, line);
        draw->AddLine({ center.x - radius, center.y - 2.f * scale },
            { center.x + radius, center.y - 2.f * scale }, color, line);
        draw->AddCircleFilled({ center.x - 3.5f * scale, center.y - 4.f * scale },
            .7f * scale, color);
        break;
    case EditorUiObjectIcon::Transform:
        draw->AddLine({ center.x - radius, center.y },
            { center.x + radius, center.y }, color, line);
        draw->AddLine({ center.x, center.y - radius },
            { center.x, center.y + radius }, color, line);
        draw->AddTriangleFilled({ center.x + radius, center.y },
            { center.x + 2.5f * scale, center.y - 2.f * scale },
            { center.x + 2.5f * scale, center.y + 2.f * scale }, color);
        draw->AddTriangleFilled({ center.x, center.y - radius },
            { center.x - 2.f * scale, center.y - 2.5f * scale },
            { center.x + 2.f * scale, center.y - 2.5f * scale }, color);
        break;
    default:
        draw->AddRectFilled({ center.x - 4.2f * scale, center.y - 4.2f * scale },
            { center.x + 4.2f * scale, center.y + 4.2f * scale }, color,
            1.4f * scale);
        break;
    }
}

template<typename DrawControl>
bool InspectorField(const char* label, DrawControl&& drawControl)
{
    const char* windowName = ImGui::GetCurrentWindow()->Name;
    const bool inspector = std::strncmp(windowName, "Properties", 10) == 0 &&
        (windowName[10] == '\0' || windowName[10] == ' ');
    if (!inspector || !label || label[0] == '#')
        return drawControl(label);

    const float rowStart = ImGui::GetCursorPosX();
    const float rowWidth = ImGui::GetWindowContentRegionMax().x - rowStart;
    const float labelWidth = ImGui::CalcTextSize(label).x;
    if (rowWidth < 190.f || labelWidth > rowWidth * .40f)
        return drawControl(label);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0.f, 0.f);
    ImGui::SetCursorPosX(rowStart + rowWidth * .44f);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool changed = drawControl("##value");
    ImGui::PopID();
    return changed;
}
}

void ImGuiEditorUi::SetNextWindowRect(float x,float y,float w,float h){ ImGui::SetNextWindowPos({x,y},ImGuiCond_FirstUseEver); ImGui::SetNextWindowSize({w,h},ImGuiCond_FirstUseEver); }
bool ImGuiEditorUi::BeginWindow(const char* title, bool* open, bool noPadding)
{
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!(context->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSize))
    {
        ImGuiWindow* window = ImGui::FindWindowByName(title);
        ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(title));
        const bool docked = window ? window->DockNode != nullptr
            : settings && settings->DockId != 0 &&
                ImGui::DockBuilderGetNode(settings->DockId) != nullptr;
        const ImVec2 savedSize = window ? window->SizeFull
            : settings ? ImVec2(static_cast<float>(settings->Size.x),
                static_cast<float>(settings->Size.y)) : ImVec2(0.f, 0.f);
        if (!docked && ((!window && !settings) ||
                savedSize.x < 64.f || savedSize.y < 64.f))
        {
            const ImVec2 workSize = ImGui::GetMainViewport()->WorkSize;
            if (workSize.x > 0.f && workSize.y > 0.f)
                ImGui::SetNextWindowSize({ workSize.x * 0.9f, workSize.y * 0.9f },
                    ImGuiCond_Always);
        }
    }
    if (noPadding)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.f, 0.f });
    const bool visible = ImGui::Begin(title, open);
    if (noPadding)
        ImGui::PopStyleVar();
    return visible;
}
bool ImGuiEditorUi::BeginViewportOverlay(const char* id, float width, float height)
{
    const float availableWidth = std::max(0.f,
        m_viewportScreenMax.x - m_viewportScreenMin.x - viewportTabWidth - 16.f);
    const float panelTop = ViewportToolsTop(m_viewportScreenMin, m_viewportScreenMax);
    const float availableHeight = std::max(0.f,
        m_viewportScreenMax.y - panelTop - 8.f);
    const float panelWidth = std::min(width, availableWidth);
    const float panelMaxHeight = std::min(height, availableHeight);
    ImGui::SetNextWindowPos({m_viewportScreenMax.x - panelWidth, panelTop},
        ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints({panelWidth, 0.f},
        {panelWidth, panelMaxHeight});
    ImGui::SetNextWindowBgAlpha(0.94f);
    return ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_AlwaysAutoResize);
}
int ImGuiEditorUi::ViewportSideTabs(const char*, const char* const* labels,
    int count, int active, float panelWidth)
{
    int selected = active;
    const float panelTop = ViewportToolsTop(m_viewportScreenMin, m_viewportScreenMax);
    const float availableHeight = std::max(0.f, m_viewportScreenMax.y - panelTop - 8.f);
    const float availableWidth = std::max(0.f,
        m_viewportScreenMax.x - m_viewportScreenMin.x - viewportTabWidth - 16.f);
    const float actualPanelWidth = std::min(panelWidth, availableWidth);
    if (count <= 0 || availableHeight <= 0.f ||
        m_viewportScreenMax.x - m_viewportScreenMin.x < viewportTabWidth)
        return selected;

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool windowHovered = ImGui::IsWindowHovered();
    const float hitX = m_viewportScreenMax.x - viewportTabWidth -
        (active >= 0 ? actualPanelWidth : 0.f);
    float hitY = panelTop;
    for (int i = 0; i < count; ++i)
    {
        const char* label = labels && labels[i] ? labels[i] : "?";
        const float remainingHeight = m_viewportScreenMax.y - hitY - 8.f;
        if (remainingHeight <= 0.f)
            break;
        const float tabHeight = std::min(remainingHeight,
            std::max(82.f, ImGui::CalcTextSize(label).x + 24.f));
        if (windowHovered && mouse.x >= hitX &&
            mouse.x < hitX + viewportTabWidth && mouse.y >= hitY &&
            mouse.y < hitY + tabHeight &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            selected = active == i ? -1 : i;
        hitY += tabHeight + ImGui::GetStyle().ItemSpacing.y;
    }

    const float tabX = m_viewportScreenMax.x - viewportTabWidth -
        (selected >= 0 ? actualPanelWidth : 0.f);
    float tabY = panelTop;
    for (int i = 0; i < count; ++i)
    {
        const char* label = labels && labels[i] ? labels[i] : "?";
        const float remainingHeight = m_viewportScreenMax.y - tabY - 8.f;
        if (remainingHeight <= 0.f)
            break;
        const ImVec2 tabSize{viewportTabWidth, std::min(remainingHeight,
            std::max(82.f, ImGui::CalcTextSize(label).x + 24.f))};
        const ImVec2 tabMin{tabX, tabY};
        const ImVec2 tabMax{tabX + tabSize.x, tabY + tabSize.y};
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 background = ImGui::GetColorU32(selected == i
            ? ImGuiCol_TabActive : ImGuiCol_Tab);
        draw->AddRectFilled(tabMin, tabMax, background, 5.f,
            ImDrawFlags_RoundCornersLeft);
        draw->AddLine({tabMin.x + 1.f, tabMin.y + 7.f},
            {tabMin.x + 1.f, tabMax.y - 7.f},
            ImGui::GetColorU32(selected == i ? ImGuiCol_CheckMark
                : ImGuiCol_Border), 2.f);
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize();
        const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label);
        const ImVec2 center{(tabMin.x + tabMax.x) * 0.5f,
            (tabMin.y + tabMax.y) * 0.5f};
        const int firstVertex = draw->VtxBuffer.Size;
        draw->AddText(font, fontSize,
            {center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f},
            ImGui::GetColorU32(selected == i ? ImGuiCol_CheckMark
                : ImGuiCol_Text), label);
        for (int vertex = firstVertex; vertex < draw->VtxBuffer.Size; ++vertex)
        {
            ImVec2& position = draw->VtxBuffer[vertex].pos;
            const float x = position.x - center.x;
            const float y = position.y - center.y;
            position = {center.x + y, center.y - x};
        }
        tabY += tabSize.y + ImGui::GetStyle().ItemSpacing.y;
    }
    return selected;
}
bool ImGuiEditorUi::BeginViewportHeader(const char* id, float width)
{
    const float viewportWidth = std::max(0.f,
        m_viewportScreenMax.x - m_viewportScreenMin.x);
    // Keep the mode selector in the top left; the view cube owns the right.
    const float availableWidth = viewportWidth - 8.f - 70.f;
    if (availableWidth < 120.f)
        return false;

    const float headerWidth = std::min(width, availableWidth);
    ImGui::SetNextWindowPos({m_viewportScreenMin.x + 8.f,
        m_viewportScreenMin.y + 6.f},
        ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints({headerWidth, 0.f},
        {headerWidth, 48.f});
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.f, 6.f});
    const bool visible = ImGui::Begin(id, nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    if (!visible)
        ImGui::End();
    return visible;
}
bool ImGuiEditorUi::EndViewportHeader()
{
    const bool clicked = ImGui::IsWindowHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    ImGui::End();
    return clicked;
}
bool ImGuiEditorUi::BeginViewportHeaderDropdown(const char* id,
    const char* preview, float width, float maxHeight)
{
    const float popupWidth = std::min(300.f, std::max(120.f,
        m_viewportScreenMax.x - m_viewportScreenMin.x - 16.f));
    const float popupHeight = std::min(maxHeight, std::max(100.f,
        m_viewportScreenMax.y - ImGui::GetCursorScreenPos().y - 40.f));
    ImGui::SetNextItemWidth(width);
    ImGui::SetNextWindowSizeConstraints({popupWidth, 0.f},
        {popupWidth, popupHeight});
    return ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLargest);
}
void ImGuiEditorUi::EndViewportHeaderDropdown(){ImGui::EndCombo();}
void ImGuiEditorUi::EndWindow(){ImGui::End();}
bool ImGuiEditorUi::IsWindowFocused() const{return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);}
void ImGuiEditorUi::WindowTitleTooltip(const char* text)
{
    if (text && ImGui::IsWindowHovered())
        ImGui::SetTooltip("%s", text);
}
void ImGuiEditorUi::PushId(const void* id){ImGui::PushID(id);}
void ImGuiEditorUi::PushId(const char* id){ImGui::PushID(id);}
void ImGuiEditorUi::PopId(){ImGui::PopID();}
bool ImGuiEditorUi::Button(const char* label,float width,float height)
{
    const bool pressed=ImGui::Button(label,{width,height});
    const float hover=AnimateInteraction(ImGui::GetItemID(),ImGui::IsItemHovered());
    if(hover>.01f){
        ImVec4 accent=ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        accent.w*=hover*.8f;
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax(),ImGui::GetColorU32(accent),
            ImGui::GetStyle().FrameRounding,0,1.f+hover);
    }
    return pressed;
}
EditorUiBreadcrumbResult ImGuiEditorUi::Breadcrumb(const char* label,
    const char* const* childFolders, int childFolderCount)
{
    (void)childFolders;
    (void)childFolderCount;
    EditorUiBreadcrumbResult result;
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::CalcTextSize(label);
    ImGui::InvisibleButton("##breadcrumb", size);
    const bool hovered = ImGui::IsItemHovered();
    result.clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const ImU32 color = ImGui::GetColorU32(hovered
        ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddText(position, color, label);
    drawList->AddLine({ position.x, position.y + size.y + 1.f },
        { position.x + size.x, position.y + size.y + 1.f }, color, 1.f);
    return result;
}
void ImGuiEditorUi::Label(const char* t){ImGui::TextUnformatted(t);}
void ImGuiEditorUi::DisabledLabel(const char* t){ImGui::TextDisabled("%s",t);}
void ImGuiEditorUi::ColoredLabel(const char* t,EditorUiColor c){ImGui::TextColored({c.r,c.g,c.b,c.a},"%s",t);}
void ImGuiEditorUi::BeginTextWrap(){ImGui::PushTextWrapPos(0.f);}
void ImGuiEditorUi::EndTextWrap(){ImGui::PopTextWrapPos();}
void ImGuiEditorUi::SameLine(){ImGui::SameLine();}
void ImGuiEditorUi::SameLineRight(float width)
{
    const float right = ImGui::GetWindowContentRegionMax().x;
    const float previousRight = ImGui::GetItemRectMax().x -
        ImGui::GetWindowPos().x + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine(std::max(previousRight, right - width));
}
void ImGuiEditorUi::Separator(){ImGui::Separator();} void ImGuiEditorUi::Spacing(){ImGui::Spacing();}
void ImGuiEditorUi::Indent(float width){ImGui::Indent(width);}
void ImGuiEditorUi::Unindent(float width){ImGui::Unindent(width);}
void ImGuiEditorUi::SetNextItemMixedValue(bool mixed){m_nextItemMixed=mixed;}
bool ImGuiEditorUi::ConsumeMixedValue(){const bool mixed=m_nextItemMixed;m_nextItemMixed=false;return mixed;}
bool ImGuiEditorUi::Checkbox(const char*l,bool*v){const bool mixed=ConsumeMixedValue();if(mixed)ImGui::PushItemFlag(ImGuiItemFlags_MixedValue,true);const bool changed=InspectorField(l,[&](const char* id){return ImGui::Checkbox(id,v);});if(mixed)ImGui::PopItemFlag();return changed;}
bool ImGuiEditorUi::InputText(const char*l,char*b,size_t s){const bool mixed=ConsumeMixedValue();if(l&&l[0]=='#'&&l[1]=='#')ImGui::SetNextItemWidth(-FLT_MIN);if(!mixed)return InspectorField(l,[&](const char* id){return ImGui::InputText(id,b,s);});std::vector<char> display(s,0);strncpy_s(display.data(),s,"-",_TRUNCATE);const bool changed=InspectorField(l,[&](const char* id){return ImGui::InputText(id,display.data(),s);});if(changed)strncpy_s(b,s,display.data(),_TRUNCATE);return changed;}
bool ImGuiEditorUi::SearchInput(const char* label,char* buffer,size_t size,
    const char* hint)
{
    ImGui::SetNextItemWidth(-FLT_MIN);
    return ImGui::InputTextWithHint(label,hint,buffer,size);
}
bool ImGuiEditorUi::InputTextSubmit(const char*l,char*b,size_t s){if(l&&l[0]=='#'&&l[1]=='#')ImGui::SetNextItemWidth(-FLT_MIN);return InspectorField(l,[&](const char* id){return ImGui::InputText(id,b,s,ImGuiInputTextFlags_EnterReturnsTrue);});}
void ImGuiEditorUi::ReadOnlyTextBlock(const char* label,const char* text,bool scrollToBottom,float reservedBottom)
{
    const char* value=text?text:"";
    const ImGuiID id=ImGui::GetID(label);
    ImVec2 available=ImGui::GetContentRegionAvail();
    available.y=std::max(1.f,available.y-reservedBottom);
    ImGui::InputTextMultiline(label,const_cast<char*>(value),strlen(value)+1,
        available,ImGuiInputTextFlags_ReadOnly|ImGuiInputTextFlags_NoUndoRedo);
    if(scrollToBottom)
    {
        ImGuiContext& context=*GImGui;
        for(ImGuiWindow* window:context.Windows)
            if(window&&window->ChildId==id)
            {
                ImGui::SetScrollY(window,window->ScrollMax.y);
                break;
            }
    }
}
bool ImGuiEditorUi::DragFloat(const char*l,float*v,float s,float a,float b){const bool mixed=ConsumeMixedValue();return InspectorField(l,[&](const char* id){return ImGui::DragFloat(id,v,s,a,b,mixed?"-":"%.3f");});}
bool ImGuiEditorUi::DragFloat3(const char*l,float*v,float s,float a,float b){const bool mixed=ConsumeMixedValue();return InspectorField(l,[&](const char* id){return ImGui::DragFloat3(id,v,s,a,b,mixed?"-":"%.3f");});}
bool ImGuiEditorUi::IsAnyItemActive() const{return ImGui::IsAnyItemActive();}
bool ImGuiEditorUi::ColorEdit3(const char*l,float*v){const bool mixed=ConsumeMixedValue();return InspectorField(l,[&](const char* id){return mixed?ImGui::DragFloat3(id,v,.01f,0.f,1.f,"-"):ImGui::ColorEdit3(id,v);});}
bool ImGuiEditorUi::ColorEdit4(const char*l,float*v){const bool mixed=ConsumeMixedValue();return InspectorField(l,[&](const char* id){return mixed?ImGui::DragFloat4(id,v,.01f,0.f,1.f,"-"):ImGui::ColorEdit4(id,v);});}
bool ImGuiEditorUi::SliderInt(const char*l,int*v,int a,int b){return InspectorField(l,[&](const char* id){return ImGui::SliderInt(id,v,a,b);});}
bool ImGuiEditorUi::SliderFloat(const char*l,float*v,float a,float b){return InspectorField(l,[&](const char* id){return ImGui::SliderFloat(id,v,a,b,"%.0f units");});}
bool ImGuiEditorUi::InputUInt(const char*l,uint32_t*v){return InspectorField(l,[&](const char* id){return ImGui::InputScalar(id,ImGuiDataType_U32,v);});}
void ImGuiEditorUi::ValueLabel(const char*l,const char*v){const char* value=ConsumeMixedValue()?"-":v;InspectorField(l,[&](const char* id){ImGui::LabelText(id,"%s",value);return false;});}
bool ImGuiEditorUi::CollapsingHeader(const char*l,bool d){return ImGui::CollapsingHeader(l,d?ImGuiTreeNodeFlags_DefaultOpen:0);}
bool ImGuiEditorUi::PropertyGroupHeader(const char* label,bool defaultOpen)
{
    const ImGuiStyle& style=ImGui::GetStyle();
    const ImVec4 background=ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    const ImVec4 header=ImGui::GetStyleColorVec4(ImGuiCol_Header);
    const ImVec4 hovered=ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        {style.FramePadding.x+1.f,style.FramePadding.y+1.f});
    ImGui::PushStyleColor(ImGuiCol_Header,ImLerp(background,header,.6f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
        ImLerp(background,hovered,.68f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
        ImLerp(background,hovered,.82f));
    const bool open=ImGui::CollapsingHeader(label,
        defaultOpen?ImGuiTreeNodeFlags_DefaultOpen:0);
    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),
        ImGui::GetItemRectMax(),ImGui::GetColorU32(ImGuiCol_Border),
        std::max(style.FrameRounding,3.f));
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    return open;
}
bool ImGuiEditorUi::ComponentHeader(EditorUiObjectIcon icon,const char* label,
    bool defaultOpen)
{
    ImGui::PushID(label);
    const ImGuiStyle& style=ImGui::GetStyle();
    const ImVec4 background=ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const ImVec4 header=ImGui::GetStyleColorVec4(ImGuiCol_Header);
    const ImVec4 accent=ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        {style.FramePadding.x+2.f,style.FramePadding.y+1.f});
    ImGui::PushStyleColor(ImGuiCol_Header,ImLerp(background,header,.55f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
        ImLerp(background,ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered),.78f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
        ImLerp(background,ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive),.85f));
    const ImGuiTreeNodeFlags flags=defaultOpen?ImGuiTreeNodeFlags_DefaultOpen:0;
    const bool open=ImGui::CollapsingHeader("##componentHeader",flags);
    const ImVec2 minimum=ImGui::GetItemRectMin();
    const ImVec2 maximum=ImGui::GetItemRectMax();
    const float hover=AnimateInteraction(ImGui::GetItemID(),ImGui::IsItemHovered(),12.f);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    const float iconSize=16.f;
    const float iconLeft=minimum.x+ImGui::GetFontSize()+
        style.FramePadding.x;
    const ImVec2 center{iconLeft+iconSize*.5f,(minimum.y+maximum.y)*.5f};
    const ImVec4 iconColor=ImLerp(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
        accent,.55f+hover*.35f);
    ImDrawList* draw=ImGui::GetWindowDrawList();
    const float rounding=std::max(style.FrameRounding,4.f);
    draw->AddRect(minimum,maximum,
        ImGui::GetColorU32(ImLerp(ImGui::GetStyleColorVec4(ImGuiCol_Border),
            accent,hover*.3f)),rounding,0,1.f);
    draw->AddRectFilled({minimum.x+1.f,minimum.y+1.f},
        {minimum.x+4.f,maximum.y-1.f},
        ImGui::GetColorU32(ImLerp(header,accent,.7f)),rounding,
        ImDrawFlags_RoundCornersLeft);
    draw->AddRectFilled({iconLeft-2.f,center.y-9.f},
        {iconLeft+iconSize+2.f,center.y+9.f},
        ImGui::GetColorU32(ImLerp(background,accent,.16f)),3.f);
    DrawObjectIcon(draw,icon,center,.92f+hover*.08f,
        ImGui::GetColorU32(iconColor));
    const ImVec2 textSize=ImGui::CalcTextSize(label);
    draw->AddText({iconLeft+iconSize+8.f,
        minimum.y+(maximum.y-minimum.y-textSize.y)*.5f},
        ImGui::GetColorU32(ImGuiCol_Text),label);
    ImGui::PopID();
    return open;
}
bool ImGuiEditorUi::TreeNode(const void*id,const char*l,bool s,bool leaf,bool d){ImGuiTreeNodeFlags f=ImGuiTreeNodeFlags_OpenOnArrow|ImGuiTreeNodeFlags_SpanAvailWidth|(s?ImGuiTreeNodeFlags_Selected:0)|(d?ImGuiTreeNodeFlags_DefaultOpen:0);if(leaf)f|=ImGuiTreeNodeFlags_Leaf|ImGuiTreeNodeFlags_NoTreePushOnOpen;return ImGui::TreeNodeEx(id,f,"%s",l);}
void ImGuiEditorUi::TreePop(){ImGui::TreePop();}
EditorUiObjectRowResult ImGuiEditorUi::ObjectTreeRow(const void* id,
    EditorUiObjectIcon icon,char* name,size_t size,bool* enabled,bool selected,
    bool leaf,bool lockName,bool enabledInHierarchy,int hierarchyDepth,
    bool lastSibling,uint64_t ancestorGuideMask,bool expandForFilter)
{
    EditorUiObjectRowResult result;
    (void)size;
    ImGui::PushID(id);
    const bool hasChildren=!leaf;
    auto [openState,inserted]=m_objectTreeOpen.emplace(id,hasChildren);
    (void)inserted;
    const bool hadChildren=m_objectHadChildren[id];
    if(hasChildren&&!hadChildren)
        openState->second=true;
    m_objectHadChildren[id]=hasChildren;
    if(hasChildren)
        ImGui::SetNextItemOpen(expandForFilter || openState->second,ImGuiCond_Always);
    ImGuiTreeNodeFlags flags=ImGuiTreeNodeFlags_OpenOnArrow|
        ImGuiTreeNodeFlags_SpanAvailWidth|ImGuiTreeNodeFlags_FramePadding|
        ImGuiTreeNodeFlags_AllowOverlap|
        (selected?ImGuiTreeNodeFlags_Selected:0);
    if(leaf)flags|=ImGuiTreeNodeFlags_Leaf|ImGuiTreeNodeFlags_NoTreePushOnOpen;
    ImGui::SetNextItemAllowOverlap();
    result.open=ImGui::TreeNodeEx("##object",flags,"");
    const bool rowHovered=ImGui::IsItemHovered();
    const float hover=AnimateInteraction(ImGui::GetItemID(),rowHovered,12.f);
    if(hasChildren && !expandForFilter)
        openState->second=result.open;
    const ImVec2 rowMinimum=ImGui::GetItemRectMin();
    const ImVec2 rowMaximum=ImGui::GetItemRectMax();
    const float hierarchyIndent=std::max(ImGui::GetStyle().IndentSpacing,1.f);
    const float branchX=rowMinimum.x-hierarchyIndent*.5f;
    const float rowCenterY=(rowMinimum.y+rowMaximum.y)*.5f;
    ImVec4 guideColor=ImGui::GetStyleColorVec4(ImGuiCol_Separator);
    guideColor.w*=.7f;
    ImDrawList* hierarchyDraw=ImGui::GetWindowDrawList();
    if(hover>.01f){
        ImVec4 hoverColor=ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
        hoverColor.w*=hover*.32f;
        hierarchyDraw->AddRectFilled(rowMinimum,rowMaximum,
            ImGui::GetColorU32(hoverColor),ImGui::GetStyle().FrameRounding);
    }
    for(int level=0;level<hierarchyDepth;++level){
        if(level<64&&(ancestorGuideMask&(uint64_t{1}<<level))){
            const float x=branchX-static_cast<float>(hierarchyDepth-level)*hierarchyIndent;
            hierarchyDraw->AddLine({x,rowMinimum.y},{x,rowMaximum.y},
                ImGui::GetColorU32(guideColor),1.f);
        }
    }
    hierarchyDraw->AddLine({branchX,rowMinimum.y},
        {branchX,lastSibling?rowCenterY:rowMaximum.y},
        ImGui::GetColorU32(guideColor),1.f);
    hierarchyDraw->AddLine({branchX,rowCenterY},{rowMinimum.x+4.f,rowCenterY},
        ImGui::GetColorU32(guideColor),1.f);
    result.clicked=ImGui::IsItemClicked()&&!ImGui::IsItemToggledOpen();
    result.doubleClicked=ImGui::IsItemHovered()&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const EditorUiHierarchyDropResult drop=HierarchyDropTarget("ENGINE_SCENE_OBJECT");
    result.dropHovered=drop.position!=EditorUiHierarchyDropPosition::None;
    result.dropPosition=drop.position;
    if(result.dropHovered){
        const float indent=std::max(ImGui::GetStyle().IndentSpacing,1.f);
        const float rootX=rowMinimum.x-static_cast<float>(hierarchyDepth)*indent;
        const int maximumDepth=hierarchyDepth+
            (drop.position==EditorUiHierarchyDropPosition::AsChild?1:0);
        result.dropDepth=std::clamp(static_cast<int>(
            std::floor((ImGui::GetIO().MousePos.x-rootX+indent*.35f)/indent)),
            0,maximumDepth);
        const bool makeChild=drop.position==EditorUiHierarchyDropPosition::AsChild&&
            result.dropDepth==hierarchyDepth+1;
        if(!makeChild){
            const float y=drop.position==EditorUiHierarchyDropPosition::Before
                ? rowMinimum.y:rowMaximum.y;
            const float x=rootX+static_cast<float>(result.dropDepth)*indent;
            ImDrawList* preview=ImGui::GetWindowDrawList();
            const ImU32 color=IM_COL32(90,160,255,255);
            preview->AddCircleFilled({x,y},3.f,color);
            preview->AddLine({x,y},{rowMaximum.x,y},color,3.f);
        }
    }
    if(drop.data&&drop.size==sizeof(const void*)){
        result.droppedItem=*static_cast<const void* const*>(drop.data);
    }
    if(ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)){
        result.dragActive=true;
        const void* payload=id;
        ImGui::SetDragDropPayload("ENGINE_SCENE_OBJECT",&payload,sizeof(payload));
        ImGui::EndDragDropSource();
    }
    ImGui::SameLine(0.f,2.f);
    if(enabled)
        result.enabledChanged=ImGui::Checkbox("##enabled",enabled);
    else
        ImGui::Dummy({ImGui::GetFrameHeight(),ImGui::GetFrameHeight()});
    result.clicked=result.clicked||ImGui::IsItemClicked();
    ImGui::SameLine(0.f,4.f);
    const float iconWidth=18.f;
    const ImVec2 iconMinimum=ImGui::GetCursorScreenPos();
    ImGui::Dummy({iconWidth,ImGui::GetFrameHeight()});
    result.clicked=result.clicked||ImGui::IsItemClicked();
    const ImVec4 iconColor=ImLerp(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
        ImGui::GetStyleColorVec4(ImGuiCol_CheckMark),hover);
    DrawObjectIcon(ImGui::GetWindowDrawList(),icon,
        {iconMinimum.x+iconWidth*.5f,iconMinimum.y+ImGui::GetFrameHeight()*.5f},
        .92f+hover*.08f,ImGui::GetColorU32(iconColor));
    ImGui::SameLine(0.f,3.f);
    const char* display=name[0]?name:"(unnamed)";
    const ImVec2 textMin=ImGui::GetCursorScreenPos();
    const ImVec2 textMax={
        ImGui::GetWindowPos().x+ImGui::GetWindowContentRegionMax().x,
        textMin.y+ImGui::GetFrameHeight()};
    const ImVec2 textSize=ImGui::CalcTextSize(display);
    ImGui::GetWindowDrawList()->AddText(
        {textMin.x,textMin.y+(textMax.y-textMin.y-textSize.y)*0.5f},
        ImGui::GetColorU32(lockName?ImGuiCol_TextDisabled:ImGuiCol_Text),display);
    ImVec4 separatorColor=ImGui::GetStyleColorVec4(ImGuiCol_Separator);
    separatorColor.w*=0.45f;
    const ImVec2 contentMin={branchX,textMax.y};
    const ImVec2 contentMax={
        ImGui::GetWindowPos().x+ImGui::GetWindowContentRegionMax().x,textMax.y};
    ImGui::GetWindowDrawList()->AddLine(
        contentMin,contentMax,ImGui::GetColorU32(separatorColor),1.f);
    // Keep the name hit target on the same line as the arrow and checkbox.
    // A new-line dummy here used to add a second, empty row to every object.
    ImGui::InvisibleButton("##name",
        {std::max(ImGui::GetContentRegionAvail().x,1.f),ImGui::GetFrameHeight()});
    result.clicked=result.clicked||ImGui::IsItemClicked();
    result.doubleClicked=result.doubleClicked||
        (ImGui::IsItemHovered()&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
    if(!enabledInHierarchy&&!result.dragActive){
        // Effective hierarchy disablement is visual only. The checkbox still
        // reflects and edits this object's own saved enabled state.
        ImVec4 dimColor=ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        dimColor.w=.58f;
        ImGui::GetWindowDrawList()->AddRectFilled(
            rowMinimum,rowMaximum,ImGui::GetColorU32(dimColor));
        ImGui::GetWindowDrawList()->AddText(
            {textMin.x,textMin.y+(textMax.y-textMin.y-textSize.y)*.5f},
            ImGui::GetColorU32(ImGuiCol_TextDisabled),display);
    }
    if(result.dragActive){
        // Remove the copy-like duplicate at the source and leave a subdued
        // placeholder showing where the lifted row came from.
        ImDrawList* rowDraw=ImGui::GetWindowDrawList();
        rowDraw->AddRectFilled(rowMinimum,rowMaximum,
            ImGui::GetColorU32(ImGuiCol_WindowBg));
        rowDraw->AddRect(rowMinimum,rowMaximum,
            ImGui::GetColorU32(ImGuiCol_Separator),2.f,0,1.f);

        // Draw one row-shaped moving item instead of ImGui's tooltip preview.
        const ImVec2 mouse=ImGui::GetIO().MousePos;
        const float ghostWidth=std::max(rowMaximum.x-rowMinimum.x,160.f);
        const ImVec2 ghostMin={mouse.x+14.f,mouse.y+12.f};
        const ImVec2 ghostMax={ghostMin.x+ghostWidth,ghostMin.y+ImGui::GetFrameHeight()};
        ImDrawList* foreground=ImGui::GetForegroundDrawList();
        foreground->AddRectFilled(ghostMin,ghostMax,IM_COL32(35,40,48,238),3.f);
        foreground->AddRect(ghostMin,ghostMax,IM_COL32(90,160,255,255),3.f,0,1.5f);
        foreground->AddText({ghostMin.x+8.f,ghostMin.y+
            (ghostMax.y-ghostMin.y-textSize.y)*.5f},
            ImGui::GetColorU32(ImGuiCol_Text),display);
    }
    ImGui::PopID();
    return result;
}
void ImGuiEditorUi::ObjectTreePop(){ImGui::TreePop();}
EditorUiHierarchyDropResult ImGuiEditorUi::HierarchyDropTarget(const char* type)
{
    EditorUiHierarchyDropResult result;
    const ImVec2 minimum=ImGui::GetItemRectMin(),maximum=ImGui::GetItemRectMax();
    if(!ImGui::BeginDragDropTarget())return result;
    const ImGuiPayload* payload=ImGui::AcceptDragDropPayload(type,
        ImGuiDragDropFlags_AcceptBeforeDelivery|ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
    if(payload){
        const float fraction=(ImGui::GetIO().MousePos.y-minimum.y)/std::max(maximum.y-minimum.y,1.f);
        result.position=fraction<.25f?EditorUiHierarchyDropPosition::Before:
            (fraction>.75f?EditorUiHierarchyDropPosition::After:EditorUiHierarchyDropPosition::AsChild);
        ImDrawList* draw=ImGui::GetWindowDrawList();
        if(result.position==EditorUiHierarchyDropPosition::AsChild)
            draw->AddRect(minimum,maximum,ImGui::GetColorU32(ImGuiCol_DragDropTarget),2.f,0,2.f);
        else{const float y=result.position==EditorUiHierarchyDropPosition::Before?minimum.y:maximum.y;
            draw->AddLine({minimum.x,y},{maximum.x,y},ImGui::GetColorU32(ImGuiCol_DragDropTarget),2.f);}
        // ImGui's built-in delivery additionally requires this exact target ID
        // to have won acceptance on the previous frame. Hierarchy rows contain
        // overlapping tree/checkbox items, so that ID can change while the
        // pointer visibly remains over the same row. Once this typed payload
        // has reached the current hovered target, releasing the mouse is an
        // unambiguous drop and should be delivered.
        if(payload->IsDelivery()||!ImGui::IsMouseDown(ImGuiMouseButton_Left)){
            const auto* bytes=static_cast<const unsigned char*>(payload->Data);
            m_dropResultPayload.assign(bytes,bytes+payload->DataSize);
            result.data=m_dropResultPayload.data();
            result.size=m_dropResultPayload.size();
        }
    }
    ImGui::EndDragDropTarget();return result;
}
EditorUiHierarchyDropResult ImGuiEditorUi::HierarchyBackgroundDropTarget(const char* type)
{
    EditorUiHierarchyDropResult result;
    ImGuiWindow* window=ImGui::GetCurrentWindow();
    const ImRect bounds=window->InnerRect;
    const ImGuiID id=window->GetID("##hierarchyBackgroundDrop");
    if(!ImGui::BeginDragDropTargetCustom(bounds,id))return result;
    const ImGuiPayload* payload=ImGui::AcceptDragDropPayload(type,
        ImGuiDragDropFlags_AcceptBeforeDelivery|ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
    if(payload){
        result.position=EditorUiHierarchyDropPosition::AsChild;
        if(payload->IsDelivery()||!ImGui::IsMouseDown(ImGuiMouseButton_Left)){
            const auto* bytes=static_cast<const unsigned char*>(payload->Data);
            m_dropResultPayload.assign(bytes,bytes+payload->DataSize);
            result.data=m_dropResultPayload.data();
            result.size=m_dropResultPayload.size();
        }
    }
    ImGui::EndDragDropTarget();
    return result;
}
EditorUiObjectRowResult ImGuiEditorUi::ObjectHeader(const void* id,char* name,size_t size,bool* enabled,bool lockName)
{
    EditorUiObjectRowResult result;
    ImGui::PushID(id);
    const float available=ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(available>125.f?available-110.f:available*0.62f);
    ImGui::BeginDisabled(lockName);
    result.nameChanged=ImGui::InputText("##name",name,size);
    ImGui::EndDisabled();
    ImGui::SameLine();
    result.enabledChanged=ImGui::Checkbox("Enabled",enabled);
    ImGui::PopID();
    return result;
}
bool ImGuiEditorUi::Selectable(const char*l,bool s,bool d){return ImGui::Selectable(l,s,d?ImGuiSelectableFlags_AllowDoubleClick:0);}
EditorUiAssetTileResult ImGuiEditorUi::AssetTile(const char* label,
    const char* fallbackIcon, void* texture, bool selected, float size)
{
    EditorUiAssetTileResult result;
    size = std::max(size, 48.f);
    ImGui::BeginGroup();
    if (selected)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        ImGui::PushStyleColor(ImGuiCol_Border,
            ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.f);
    }
    ImGui::Button("##assetTile", { size, size });
    const float hover = AnimateInteraction(ImGui::GetItemID(),
        ImGui::IsItemHovered(), 12.f);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float padding = 6.f;
    if (texture)
    {
        drawList->AddImage(
            static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)),
            { minimum.x + padding, minimum.y + padding },
            { maximum.x - padding, maximum.y - padding });
    }
    else if (fallbackIcon && *fallbackIcon)
    {
        const ImVec2 nativeSize = ImGui::CalcTextSize(fallbackIcon);
        const float fitWidth = nativeSize.x > 0.f
            ? ImGui::GetFontSize() * (size - padding * 2.f) / nativeSize.x
            : ImGui::GetFontSize();
        const float fontSize = std::min({ size * 0.42f, 48.f, fitWidth });
        const ImVec2 iconSize = ImGui::GetFont()->CalcTextSizeA(
            fontSize, FLT_MAX, 0.f, fallbackIcon);
        drawList->AddText(ImGui::GetFont(), fontSize,
            { minimum.x + (size - iconSize.x) * 0.5f,
              minimum.y + (size - iconSize.y) * 0.5f },
            ImGui::GetColorU32(ImGuiCol_TextDisabled), fallbackIcon);
    }
    if (hover > .01f)
    {
        ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        accent.w *= hover * .85f;
        drawList->AddRect(minimum, maximum, ImGui::GetColorU32(accent),
            ImGui::GetStyle().FrameRounding, 0, 1.f + hover);
    }
    if (selected)
    {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size);
    ImGui::TextWrapped("%s", label);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    result.clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    result.doubleClicked = ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    return result;
}
float ImGuiEditorUi::AvailableContentWidth() const
{
    return ImGui::GetContentRegionAvail().x;
}
EditorUiContextMenuResult ImGuiEditorUi::ContextMenu(const void* id,const char* addLabel,const char* deleteLabel,bool objectCreationMenu,const char* unpackLabel)
{
    EditorUiContextMenuResult result;
    const bool hasAdd=addLabel&&addLabel[0];
    const bool hasDelete=deleteLabel&&deleteLabel[0];
    const bool hasUnpack=unpackLabel&&unpackLabel[0];
    if(!hasAdd&&!hasDelete&&!hasUnpack)return result;
    ImGui::PushID(id);
    if(!ImGui::GetIO().KeyAlt&&ImGui::IsItemHovered()&&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##context");
    if(ImGui::BeginPopup("##context")){
        if(hasAdd){
            if(objectCreationMenu){
                if(ImGui::BeginMenu(addLabel)){
                    result.addRequested=ImGui::MenuItem("Empty");
                    if(ImGui::BeginMenu("3D")){
                        for(const PrimitiveObjectDefinition& primitive:PrimitiveObjectDefinitions())
                            if(ImGui::MenuItem(primitive.name))result.primitive3D=primitive.name;
                        ImGui::EndMenu();
                    }
                    if(ImGui::BeginMenu("2D")){
                        result.addSpriteRequested=ImGui::MenuItem("Sprite");
                        ImGui::EndMenu();
                    }
                    if(ImGui::BeginMenu("Lighting")){
                        result.addLightProbeRequested=ImGui::MenuItem("Light Probe");
                        result.addLightProbeGroupRequested=ImGui::MenuItem("Light Probe Volume");
                        ImGui::EndMenu();
                    }
                    ImGui::EndMenu();
                }
            }else result.addRequested=ImGui::MenuItem(addLabel);
        }
        if(hasUnpack)result.unpackRequested=ImGui::MenuItem(unpackLabel);
        if(hasDelete)result.deleteRequested=ImGui::MenuItem(deleteLabel);
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return result;
}
EditorUiAssetCreateMenuResult ImGuiEditorUi::AssetWindowContextMenu()
{
    EditorUiAssetCreateMenuResult result;
    if(ImGui::BeginPopupContextWindow("##windowContext",
        ImGuiPopupFlags_MouseButtonRight|ImGuiPopupFlags_NoOpenOverItems)){
        if(ImGui::BeginMenu("Create")){
            result.folderRequested=ImGui::MenuItem("Folder");
            result.scriptRequested=ImGui::MenuItem("Script");
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    return result;
}
EditorUiAssetItemMenuResult ImGuiEditorUi::AssetItemContextMenu(const void* id)
{
    EditorUiAssetItemMenuResult result;
    ImGui::PushID(id);
    if(ImGui::BeginPopupContextItem("##assetItemContext"))
    {
        result.renameRequested=ImGui::MenuItem("Rename");
        result.deleteRequested=ImGui::MenuItem("Delete");
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return result;
}
EditorUiTextEditResult ImGuiEditorUi::RenameText(const char* label,char* buffer,size_t size,bool focus)
{
    EditorUiTextEditResult result;
    if(focus)ImGui::SetKeyboardFocusHere();
    result.submitted=ImGui::InputText(label,buffer,size,
        ImGuiInputTextFlags_AutoSelectAll|ImGuiInputTextFlags_EnterReturnsTrue);
    result.deactivated=ImGui::IsItemDeactivated();
    return result;
}
EditorUiPrefabMenuResult ImGuiEditorUi::PrefabOverrideMenu(const void* id,bool hasOverrides)
{
    EditorUiPrefabMenuResult result;
    ImGui::PushID(id);
    if(ImGui::BeginPopupContextItem("##prefabOverrides")){
        result.editRequested=ImGui::MenuItem("Edit Prefab");
        ImGui::BeginDisabled(!hasOverrides);
        result.applyRequested=ImGui::MenuItem("Apply Overrides to Prefab");
        result.applyAllRequested=ImGui::MenuItem("Apply All to Prefab (Including Transform)");
        result.revertRequested=ImGui::MenuItem("Revert Overrides");
        ImGui::EndDisabled();
        ImGui::Separator();
        result.unpackRequested=ImGui::MenuItem("Unpack Prefab");
        result.deleteRequested=ImGui::MenuItem("Delete From Scene");
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return result;
}
bool ImGuiEditorUi::BeginChild(const char*i){return ImGui::BeginChild(i,{0,0},false,ImGuiWindowFlags_HorizontalScrollbar);} void ImGuiEditorUi::EndChild(){ImGui::EndChild();}
bool ImGuiEditorUi::IsItemHovered()const{return ImGui::IsItemHovered();} bool ImGuiEditorUi::IsItemClicked()const{return ImGui::IsItemClicked()&&!ImGui::IsItemToggledOpen();}
bool ImGuiEditorUi::IsItemDoubleClicked()const{return ImGui::IsItemHovered()&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);}
bool ImGuiEditorUi::IsWindowBackgroundClicked()const{return ImGui::IsMouseClicked(ImGuiMouseButton_Left)&&ImGui::IsWindowHovered()&&!ImGui::IsAnyItemHovered();}
bool ImGuiEditorUi::CopyShortcutPressed()const{return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)&&EditorKeyBindings::Get().Pressed(EditorCommand::Copy);}
bool ImGuiEditorUi::PasteShortcutPressed()const{return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)&&EditorKeyBindings::Get().Pressed(EditorCommand::Paste);}
bool ImGuiEditorUi::DeleteShortcutPressed()const
{
    if(!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))return false;
    if(EditorKeyBindings::Get().IsCapturing()||ImGui::GetIO().WantTextInput)return false;
    return ImGui::IsKeyPressed(ImGuiKey_Delete,false)||ImGui::IsKeyPressed(ImGuiKey_Backspace,false);
}
bool ImGuiEditorUi::IsMultiSelectModifierDown()const{return ImGui::GetIO().KeyCtrl;}
bool ImGuiEditorUi::IsRangeSelectModifierDown()const{return ImGui::GetIO().KeyShift;}
bool ImGuiEditorUi::BeginDragDropSource(){
    const ImVec2 minimum=ImGui::GetItemRectMin(),maximum=ImGui::GetItemRectMax();
    ImDrawList* rowDrawList=ImGui::GetWindowDrawList();
    // Composite widgets such as AssetTile finish with EndGroup(). ImGui gives
    // that group a zero ID whenever none of its children is the active item.
    // If another control in the same window is held (for example the asset
    // list/grid toggle), probing the group as a drag source otherwise asserts.
    // ImGui's supported null-ID path derives a temporary ID from the group's
    // rectangle and still returns false unless this item is actually dragged.
    if(!ImGui::BeginDragDropSource(
        ImGuiDragDropFlags_SourceAllowNullID))return false;
    // Keep the source row visible as the item being moved while ImGui's
    // standard preview tooltip follows the pointer.
    rowDrawList->AddRectFilled(minimum,maximum,IM_COL32(90,160,255,55));
    return true;
}
void ImGuiEditorUi::SetDragDropPayload(const char*t,const void*d,size_t s){ImGui::SetDragDropPayload(t,d,s);}
void ImGuiEditorUi::EndDragDropSource(){ImGui::EndDragDropSource();}
bool ImGuiEditorUi::BeginDragDropTarget(){return ImGui::BeginDragDropTarget();}
const void* ImGuiEditorUi::AcceptDragDropPayload(const char*t,size_t*s){const ImGuiPayload*p=ImGui::AcceptDragDropPayload(t);if(!p)return nullptr;if(s)*s=static_cast<size_t>(p->DataSize);return p->Data;}
EditorUiDragDropPayloadResult ImGuiEditorUi::InspectDragDropPayload(const char*t){EditorUiDragDropPayloadResult r;const ImGuiPayload*p=ImGui::AcceptDragDropPayload(t,ImGuiDragDropFlags_AcceptBeforeDelivery|ImGuiDragDropFlags_AcceptNoDrawDefaultRect);if(p){r.data=p->Data;r.size=static_cast<size_t>(p->DataSize);r.delivered=p->IsDelivery();}return r;}
EditorUiDragDropPayloadResult ImGuiEditorUi::WindowDragDropTarget(const char*t)
{
    EditorUiDragDropPayloadResult result;
    ImGuiWindow* window=ImGui::GetCurrentWindow();
    if(!window||window->SkipItems)return result;
    const ImGuiID id=window->GetID("##windowAssetDrop");
    if(!ImGui::BeginDragDropTargetCustom(window->InnerRect,id))return result;
    const ImGuiPayload* payload=ImGui::AcceptDragDropPayload(t,
        ImGuiDragDropFlags_AcceptBeforeDelivery|ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
    if(payload){
        const auto* bytes=static_cast<const unsigned char*>(payload->Data);
        m_dropResultPayload.assign(bytes,bytes+payload->DataSize);
        result.data=m_dropResultPayload.data();
        result.size=m_dropResultPayload.size();
        result.delivered=payload->IsDelivery()||!ImGui::IsMouseDown(ImGuiMouseButton_Left);
    }
    ImGui::EndDragDropTarget();
    return result;
}
void ImGuiEditorUi::EndDragDropTarget(){ImGui::EndDragDropTarget();}
void ImGuiEditorUi::SetClipboardText(const char*t){ImGui::SetClipboardText(t);} void ImGuiEditorUi::ScrollToBottom(){ImGui::SetScrollHereY(1.f);}
bool ImGuiEditorUi::BeginTabBar(const char*i){return ImGui::BeginTabBar(i);} void ImGuiEditorUi::EndTabBar(){ImGui::EndTabBar();}
bool ImGuiEditorUi::BeginTab(const char*l){return ImGui::BeginTabItem(l);} void ImGuiEditorUi::EndTab(){ImGui::EndTabItem();}
bool ImGuiEditorUi::BeginTable(const char* id,int columns){return ImGui::BeginTable(id,columns,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable|ImGuiTableFlags_SizingStretchProp);}
void ImGuiEditorUi::TableSetupColumn(const char* label){ImGui::TableSetupColumn(label);}
void ImGuiEditorUi::TableSetupCompactColumn(const char* label){ImGui::TableSetupColumn(label,ImGuiTableColumnFlags_WidthFixed,28.f);}
void ImGuiEditorUi::TableHeadersRow(){ImGui::TableHeadersRow();}
void ImGuiEditorUi::TableNextRow(){ImGui::TableNextRow();}
void ImGuiEditorUi::TableNextColumn(){ImGui::TableNextColumn();}
void ImGuiEditorUi::EndTable(){ImGui::EndTable();}
bool ImGuiEditorUi::BindingColumnHeader(const char* id,const char* label,bool canDelete)
{
    ImGui::PushID(id);ImGui::TextUnformatted(label);bool remove=false;
    if(ImGui::BeginPopupContextItem("##bindingColumnMenu"))
    {
        if(ImGui::MenuItem("Delete binding column",nullptr,false,canDelete))remove=true;
        if(!canDelete&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("At least one binding column is required.");
        ImGui::EndPopup();
    }
    ImGui::PopID();return remove;
}
bool ImGuiEditorUi::AddBindingColumnHeader(const char* id)
{
    ImGui::PushID(id);const bool add=ImGui::SmallButton("+");
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Add another binding column");
    ImGui::PopID();return add;
}
bool ImGuiEditorUi::KeyBindingInput(const char* id,const char* display,std::string* key,bool* control,bool* shift,bool* alt)
{
    if(!key||!control||!shift||!alt)return false;
    const ImGuiID captureId=ImGui::GetID(id);
    const bool capturing=m_bindingCaptureId==captureId;
    const std::string buttonLabel=std::string(capturing?"Press a key...":display)+"##"+id;
    const bool activated=ImGui::Button(buttonLabel.c_str(),{-1.f,0.f});
    if(activated)m_bindingCaptureId=captureId;
    bool changed=false;
    if(m_bindingCaptureId==captureId&&!activated)
    {
        if(ImGui::IsKeyPressed(ImGuiKey_Escape,false))m_bindingCaptureId=0;
        else if(ImGui::IsKeyPressed(ImGuiKey_Delete,false)||ImGui::IsKeyPressed(ImGuiKey_Backspace,false))
        {
            key->clear();*control=*shift=*alt=false;m_bindingCaptureId=0;changed=true;
        }
        else
        {
            const ImGuiIO& io=ImGui::GetIO();
            const char* mouseName=nullptr;
            if(ImGui::IsMouseClicked(ImGuiMouseButton_Left,false))mouseName="Mouse Left";
            else if(ImGui::IsMouseClicked(ImGuiMouseButton_Right,false))mouseName="Mouse Right";
            else if(ImGui::IsMouseClicked(ImGuiMouseButton_Middle,false))mouseName="Mouse Middle";
            else if(ImGui::IsMouseClicked(3,false))mouseName="Mouse X1";
            else if(ImGui::IsMouseClicked(4,false))mouseName="Mouse X2";
            else if(io.MouseWheel!=0.f)mouseName="Mouse Wheel";
            if(mouseName)
            {
                *key=mouseName;*control=io.KeyCtrl;*shift=io.KeyShift;*alt=io.KeyAlt;
                m_bindingCaptureId=0;changed=true;
            }
            else
            {
                for(int value=ImGuiKey_NamedKey_BEGIN;value<ImGuiKey_NamedKey_END;++value)
                {
                    const ImGuiKey candidate=static_cast<ImGuiKey>(value);
                    if(candidate==ImGuiKey_LeftCtrl||candidate==ImGuiKey_RightCtrl||candidate==ImGuiKey_LeftShift||candidate==ImGuiKey_RightShift||candidate==ImGuiKey_LeftAlt||candidate==ImGuiKey_RightAlt||!ImGui::IsKeyPressed(candidate,false))continue;
                    const char* name=ImGui::GetKeyName(candidate);if(!name||!*name)continue;
                    *key=name;*control=io.KeyCtrl;*shift=io.KeyShift;*alt=io.KeyAlt;
                    m_bindingCaptureId=0;changed=true;break;
                }
            }
        }
    }
    EditorKeyBindings::Get().SetCapturing(m_bindingCaptureId!=0);
    return changed;
}
void ImGuiEditorUi::CancelKeyBindingCapture(){m_bindingCaptureId=0;EditorKeyBindings::Get().SetCapturing(false);}
void ImGuiEditorUi::BeginDisabled(bool d){ImGui::BeginDisabled(d);} void ImGuiEditorUi::EndDisabled(){ImGui::EndDisabled();}
bool ImGuiEditorUi::Combo(const char*l,int*s,const char*const*i,int c)
{
    const ImGuiStyle& style=ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{style.FramePadding.x,2.f});
    const bool changed=InspectorField(l,[&](const char* id){return ImGui::Combo(id,s,i,c);});
    ImGui::PopStyleVar();
    return changed;
}
void ImGuiEditorUi::SetNextItemWidth(float width){ImGui::SetNextItemWidth(width);}
void ImGuiEditorUi::Tooltip(const char*t){if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%s",t);}
void ImGuiEditorUi::Progress(float f,const char*o){ImGui::ProgressBar(f,{-1,0},o);}
void ImGuiEditorUi::PercentageGrid(const char* id,
    const EditorUiPercentageSegment* segments,size_t segmentCount,float requestedWidth)
{
    const float width=requestedWidth>0.f?requestedWidth:ImGui::GetContentRegionAvail().x;
    const float gap=2.f;
    const float cell=std::max(2.f,(width-gap*9.f)/10.f);
    const float actualWidth=cell*10.f+gap*9.f;
    const float height=actualWidth;
    const ImVec2 origin=ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id,{actualWidth,height});
    ImDrawList* draw=ImGui::GetWindowDrawList();
    const ImU32 empty=ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 border=ImGui::GetColorU32(ImGuiCol_Border);
    struct CellFill
    {
        float r=0.f,g=0.f,b=0.f,a=0.f;
        float coverage=0.f;
    };
    std::array<CellFill,100> fills{};
    float cursor=0.f;
    for(size_t segment=0;segment<segmentCount&&cursor<100.f;++segment)
    {
        const float amount=std::clamp(segments[segment].percentage,0.f,100.f-cursor);
        const float end=cursor+amount;
        const int first=std::clamp(static_cast<int>(std::floor(cursor)),0,99);
        const int last=std::clamp(static_cast<int>(std::ceil(end))-1,0,99);
        for(int index=first;index<=last&&amount>0.f;++index)
        {
            const float overlap=std::max(0.f,std::min(end,static_cast<float>(index+1))-
                std::max(cursor,static_cast<float>(index)));
            CellFill& fill=fills[index];
            fill.r+=segments[segment].color.r*overlap;
            fill.g+=segments[segment].color.g*overlap;
            fill.b+=segments[segment].color.b*overlap;
            fill.a+=segments[segment].color.a*overlap;
            fill.coverage+=overlap;
        }
        cursor=end;
    }
    const ImVec4 background=ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    for(int index=0;index<100;++index)
    {
        const int row=9-index/10;
        const int column=index%10;
        const ImVec2 minimum{origin.x+column*(cell+gap),origin.y+row*(cell+gap)};
        const ImVec2 maximum{minimum.x+cell,minimum.y+cell};
        ImU32 color=empty;
        const CellFill& fill=fills[index];
        if(fill.coverage>0.f)
        {
            const float coverage=std::clamp(fill.coverage,0.f,1.f);
            const float inverse=1.f-coverage;
            // A partially occupied cell fades continuously from its normal
            // background. Category boundaries blend by their share of the cell.
            color=ImGui::ColorConvertFloat4ToU32({
                background.x*inverse+fill.r,
                background.y*inverse+fill.g,
                background.z*inverse+fill.b,
                background.w*inverse+fill.a
            });
        }
        draw->AddRectFilled(minimum,maximum,color,cell*.16f);
        draw->AddRect(minimum,maximum,border,cell*.16f);
    }
    int hoveredCell=-1;
    if(ImGui::IsItemHovered())
    {
        const ImVec2 mouse=ImGui::GetIO().MousePos;
        const float localX=mouse.x-origin.x;
        const float localY=mouse.y-origin.y;
        const int column=static_cast<int>(localX/(cell+gap));
        const int visualRow=static_cast<int>(localY/(cell+gap));
        if(column>=0&&column<10&&visualRow>=0&&visualRow<10&&
            localX-column*(cell+gap)<=cell&&localY-visualRow*(cell+gap)<=cell)
        {
            hoveredCell=(9-visualRow)*10+column;
            const ImVec2 minimum{origin.x+column*(cell+gap),
                origin.y+visualRow*(cell+gap)};
            const ImVec2 maximum{minimum.x+cell,minimum.y+cell};
            draw->AddRect(minimum,maximum,
                ImGui::GetColorU32(ImGuiCol_Text),cell*.16f,0,2.f);
        }

        ImGui::BeginTooltip();
        if(hoveredCell>=0)
        {
            ImGui::Text("Cell %d  |  %d%% - %d%%",hoveredCell+1,
                hoveredCell,hoveredCell+1);
            ImGui::TextDisabled("Occupied %.1f%% of this cell",
                std::clamp(fills[hoveredCell].coverage,0.f,1.f)*100.f);
            float segmentStart=0.f;
            for(size_t index=0;index<segmentCount&&segmentStart<100.f;++index)
            {
                const float amount=std::clamp(segments[index].percentage,
                    0.f,100.f-segmentStart);
                const float segmentEnd=segmentStart+amount;
                const float overlap=std::max(0.f,
                    std::min(segmentEnd,static_cast<float>(hoveredCell+1))-
                    std::max(segmentStart,static_cast<float>(hoveredCell)));
                if(overlap>0.f)
                {
                    const EditorUiColor& value=segments[index].color;
                    ImGui::ColorButton("##cellLegend",
                        {value.r,value.g,value.b,value.a},
                        ImGuiColorEditFlags_NoTooltip|ImGuiColorEditFlags_NoDragDrop,
                        {9.f,9.f});
                    ImGui::SameLine();
                    ImGui::Text("%s  %.2f%% overall (%.0f%% of cell)",
                        segments[index].label?segments[index].label:"Usage",
                        overlap,overlap*100.f);
                }
                segmentStart=segmentEnd;
            }
            ImGui::Separator();
        }
        float total=0.f;
        for(size_t index=0;index<segmentCount;++index)
        {
            total+=std::max(0.f,segments[index].percentage);
            const EditorUiColor& value=segments[index].color;
            ImGui::ColorButton("##legend",{value.r,value.g,value.b,value.a},
                ImGuiColorEditFlags_NoTooltip|ImGuiColorEditFlags_NoDragDrop,{9.f,9.f});
            ImGui::SameLine();
            ImGui::Text("%s  %.1f%%",segments[index].label?segments[index].label:"Usage",
                segments[index].percentage);
        }
        if(segmentCount>1)ImGui::TextDisabled("Combined: %.1f%%",std::min(total,100.f));
        ImGui::TextDisabled("10 x 10  |  1%% per cell");
        ImGui::EndTooltip();
    }
}
float ImGuiEditorUi::FrameRate() const{return ImGui::GetIO().Framerate;}
void ImGuiEditorUi::UsageHistory(const char* id,const float* values,size_t count,
    float minimum,float maximum,EditorUiColor color,float requestedWidth,float height)
{
    const float width=requestedWidth>0.f?requestedWidth:ImGui::GetContentRegionAvail().x;
    const ImVec2 origin=ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id,{width,height});
    ImDrawList* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,{origin.x+width,origin.y+height},ImGui::GetColorU32(ImGuiCol_FrameBg),3.f);
    const ImU32 grid=ImGui::GetColorU32(ImGuiCol_Border);
    for(int line=1;line<4;++line)
    {
        const float y=origin.y+height*line/4.f;
        draw->AddLine({origin.x,y},{origin.x+width,y},grid);
    }
    const ImU32 plot=ImGui::ColorConvertFloat4ToU32({color.r,color.g,color.b,color.a});
    if(values&&count>1&&maximum>minimum)
    {
        for(size_t index=1;index<count;++index)
        {
            const float first=std::clamp((values[index-1]-minimum)/(maximum-minimum),0.f,1.f);
            const float second=std::clamp((values[index]-minimum)/(maximum-minimum),0.f,1.f);
            const float x0=origin.x+width*static_cast<float>(index-1)/static_cast<float>(count-1);
            const float x1=origin.x+width*static_cast<float>(index)/static_cast<float>(count-1);
            draw->AddLine({x0,origin.y+height*(1.f-first)},
                {x1,origin.y+height*(1.f-second)},plot,2.f);
        }
    }
    draw->AddRect(origin,{origin.x+width,origin.y+height},grid,3.f);
    if(ImGui::IsItemHovered()&&values&&count)
    {
        const float localX=std::clamp(ImGui::GetIO().MousePos.x-origin.x,0.f,width);
        const size_t sample=count>1?std::min(count-1,static_cast<size_t>(std::round(
            localX/width*static_cast<float>(count-1)))):0;
        const float x=count>1?origin.x+width*static_cast<float>(sample)/
            static_cast<float>(count-1):origin.x+width;
        const float normalized=maximum>minimum?std::clamp(
            (values[sample]-minimum)/(maximum-minimum),0.f,1.f):0.f;
        const float y=origin.y+height*(1.f-normalized);
        draw->AddLine({x,origin.y},{x,origin.y+height},
            ImGui::GetColorU32(ImGuiCol_TextDisabled),1.f);
        draw->AddCircleFilled({x,y},4.f,plot);
        draw->AddCircle({x,y},4.f,ImGui::GetColorU32(ImGuiCol_Text),0,1.5f);
        ImGui::BeginTooltip();
        ImGui::Text("Sample %zu of %zu",sample+1,count);
        ImGui::Text("Value %.2f",values[sample]);
        ImGui::TextDisabled("Current %.2f  |  Range %.0f - %.0f",
            values[count-1],minimum,maximum);
        ImGui::EndTooltip();
    }
}
void ImGuiEditorUi::DrawImage(void*tex,float w,float h){ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(tex)),{w,h});}
void ImGuiEditorUi::DrawCircularImage(void* tex,float diameter,EditorUiColor border)
{
    const ImVec2 minimum=ImGui::GetCursorScreenPos();
    const ImVec2 maximum{minimum.x+diameter,minimum.y+diameter};
    ImGui::InvisibleButton("##circularImage",{diameter,diameter});
    ImDrawList* draw=ImGui::GetWindowDrawList();
    const ImTextureID texture=static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(tex));
    draw->AddImageRounded(texture,minimum,maximum,{0,0},{1,1},IM_COL32_WHITE,
        diameter*0.5f,ImDrawFlags_RoundCornersAll);
    draw->AddCircle({minimum.x+diameter*0.5f,minimum.y+diameter*0.5f},
        diameter*0.5f-0.5f,ImGui::ColorConvertFloat4ToU32(
            {border.r,border.g,border.b,border.a}),0,1.25f);
}
EditorUiViewportInput ImGuiEditorUi::Viewport(void* texture,float aspect,EditorUiColor bg)
{
    EditorUiViewportInput out;
    const ImVec2 contentOrigin = ImGui::GetCursorPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.x <= 1.f || available.y <= 1.f) return out;

    ImVec2 size = available;
    ImVec2 position{0.f, 0.f};
    if (aspect > 0.f)
    {
        const float availableAspect = available.x / available.y;
        if (availableAspect > aspect)
        {
            size.x = available.y * aspect;
            position.x = (available.x - size.x) * 0.5f;
        }
        else
        {
            size.y = available.x / aspect;
            position.y = (available.y - size.y) * 0.5f;
        }
    }
    if (size.x < available.x || size.y < available.y)
    {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(cursor,
            {cursor.x + available.x, cursor.y + available.y},
            ImGui::GetColorU32({bg.r, bg.g, bg.b, bg.a}));
    }

    out.available = {size.x, size.y};
    ImGui::SetCursorPos({contentOrigin.x + position.x,
        contentOrigin.y + position.y});
    ImGui::Image(static_cast<ImTextureID>(
        reinterpret_cast<uintptr_t>(texture)), size);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    out.mousePosInViewport = {mouse.x - minimum.x, mouse.y - minimum.y};
    m_viewportScreenMin = {minimum.x, minimum.y};
    m_viewportScreenMax = {maximum.x, maximum.y};

    out.hovered = ImGui::IsItemHovered(
        ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    out.rawLeftClicked = out.hovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left, false);
    out.leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    out.leftReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const bool anyMouseDown = out.leftDown ||
        ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
        ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    const bool mousePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    if (out.hovered && mousePressed)
        m_capturedViewportTexture = texture;
    const bool ownsDrag = m_capturedViewportTexture == texture;
    out.viewportDragActive = ownsDrag;

    if (out.hovered || ownsDrag)
    {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        out.mouseDelta = {delta.x, delta.y};
        auto& bindings = EditorKeyBindings::Get();
        out.mouseWheel = out.hovered
            ? bindings.Wheel(EditorCommand::ViewportZoom) : 0.f;
        out.rightDown = bindings.Down(EditorCommand::ViewportPan);
        out.middleDown = bindings.Down(EditorCommand::ViewportOrbit);
        out.zoomDragDown = bindings.Down(EditorCommand::ViewportZoom);
        out.leftClicked = out.hovered &&
            bindings.Pressed(EditorCommand::ViewportSelect);
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    {
        auto& bindings = EditorKeyBindings::Get();
        const float keyPanStep = 500.f * ImGui::GetIO().DeltaTime;
        if (bindings.Down(EditorCommand::ViewportMoveLeft))
            out.keyPanDX += keyPanStep;
        if (bindings.Down(EditorCommand::ViewportMoveRight))
            out.keyPanDX -= keyPanStep;
        if (bindings.Down(EditorCommand::ViewportMoveForward))
            out.keyDolly += keyPanStep;
        if (bindings.Down(EditorCommand::ViewportMoveBackward))
            out.keyDolly -= keyPanStep;
    }
    if (!anyMouseDown)
        m_capturedViewportTexture = nullptr;
    return out;
}
namespace
{
ImU32 ViewportColor(EditorUiColor color)
{
    return ImGui::ColorConvertFloat4ToU32({color.r,color.g,color.b,color.a});
}
}
void ImGuiEditorUi::DrawViewportLine(EditorUiVec2 a,EditorUiVec2 b,EditorUiColor color,float thickness)
{
    ImDrawList* draw=ImGui::GetWindowDrawList();draw->PushClipRect({m_viewportScreenMin.x,m_viewportScreenMin.y},{m_viewportScreenMax.x,m_viewportScreenMax.y},true);draw->AddLine({m_viewportScreenMin.x+a.x,m_viewportScreenMin.y+a.y},{m_viewportScreenMin.x+b.x,m_viewportScreenMin.y+b.y},ViewportColor(color),thickness);draw->PopClipRect();
}
void ImGuiEditorUi::DrawViewportTriangle(EditorUiVec2 a,EditorUiVec2 b,EditorUiVec2 c,EditorUiColor color)
{
    ImDrawList* draw=ImGui::GetWindowDrawList();draw->PushClipRect({m_viewportScreenMin.x,m_viewportScreenMin.y},{m_viewportScreenMax.x,m_viewportScreenMax.y},true);draw->AddTriangleFilled({m_viewportScreenMin.x+a.x,m_viewportScreenMin.y+a.y},{m_viewportScreenMin.x+b.x,m_viewportScreenMin.y+b.y},{m_viewportScreenMin.x+c.x,m_viewportScreenMin.y+c.y},ViewportColor(color));draw->PopClipRect();
}
void ImGuiEditorUi::DrawViewportCircle(EditorUiVec2 center,float radius,EditorUiColor color,bool filled,float thickness)
{
    ImDrawList* draw=ImGui::GetWindowDrawList();draw->PushClipRect({m_viewportScreenMin.x,m_viewportScreenMin.y},{m_viewportScreenMax.x,m_viewportScreenMax.y},true);const ImVec2 point{m_viewportScreenMin.x+center.x,m_viewportScreenMin.y+center.y};if(filled)draw->AddCircleFilled(point,radius,ViewportColor(color));else draw->AddCircle(point,radius,ViewportColor(color),0,thickness);draw->PopClipRect();
}
void ImGuiEditorUi::DrawViewportText(EditorUiVec2 position,const char* text,EditorUiColor color)
{
    ImDrawList* draw=ImGui::GetWindowDrawList();draw->PushClipRect({m_viewportScreenMin.x,m_viewportScreenMin.y},{m_viewportScreenMax.x,m_viewportScreenMax.y},true);draw->AddText({m_viewportScreenMin.x+position.x,m_viewportScreenMin.y+position.y},ViewportColor(color),text?text:"");draw->PopClipRect();
}
EditorUiUvMapResult ImGuiEditorUi::UvMapEditor(const char* id, float* uvPairs,
    size_t vertexCount, const uint32_t* indices, size_t indexCount,
    int* selectedVertex, float size)
{
    EditorUiUvMapResult result;
    if (!uvPairs || !selectedVertex || vertexCount == 0)
        return result;
    size = std::max(size, 96.f);
    ImGui::PushID(id ? id : "UVMap");
    ImGui::TextUnformatted("UV Map (U/V in 0..1; drag the selected vertex)");
    const ImVec2 canvasSize(size, size);
    ImGui::InvisibleButton("##UVMapCanvas", canvasSize);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const auto pointFor = [&](size_t vertex)
    {
        const float u = uvPairs[vertex * 2];
        const float v = uvPairs[vertex * 2 + 1];
        return ImVec2(minimum.x + std::clamp(u, 0.f, 1.f) * size,
            minimum.y + (1.f - std::clamp(v, 0.f, 1.f)) * size);
    };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(minimum, maximum, true);
    draw->AddRectFilled(minimum, maximum, ImGui::GetColorU32(ImGuiCol_FrameBg));
    const ImU32 gridColor = ImGui::GetColorU32(ImGuiCol_Border);
    for (int division = 0; division <= 10; ++division)
    {
        const float fraction = static_cast<float>(division) / 10.f;
        const float x = minimum.x + fraction * size;
        const float y = minimum.y + fraction * size;
        draw->AddLine({ x, minimum.y }, { x, maximum.y }, gridColor);
        draw->AddLine({ minimum.x, y }, { maximum.x, y }, gridColor);
    }
    draw->AddRect(minimum, maximum, ImGui::GetColorU32(ImGuiCol_Border));
    const size_t drawVertexCount = std::min<size_t>(vertexCount, 100000);
    const size_t elementCount = indices && indexCount
        ? indexCount : drawVertexCount;
    const size_t triangleCount = std::min<size_t>(elementCount / 3, 100000);
    const ImU32 edgeColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const auto drawTriangle = [&](uint32_t first, uint32_t second, uint32_t third)
    {
        if (first >= drawVertexCount || second >= drawVertexCount ||
            third >= drawVertexCount)
            return;
        draw->AddLine(pointFor(first), pointFor(second), edgeColor);
        draw->AddLine(pointFor(second), pointFor(third), edgeColor);
        draw->AddLine(pointFor(third), pointFor(first), edgeColor);
    };
    for (size_t triangle = 0; triangle < triangleCount; ++triangle)
    {
        const size_t base = triangle * 3;
        drawTriangle(indices && indexCount ? indices[base]
                : static_cast<uint32_t>(base),
            indices && indexCount ? indices[base + 1]
                : static_cast<uint32_t>(base + 1),
            indices && indexCount ? indices[base + 2]
                : static_cast<uint32_t>(base + 2));
    }
    const size_t selectableCount = std::min<size_t>(vertexCount, 100000);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        float nearestDistanceSquared = 100.f;
        int nearest = -1;
        for (size_t vertex = 0; vertex < selectableCount; ++vertex)
        {
            const ImVec2 point = pointFor(vertex);
            const float dx = point.x - mouse.x;
            const float dy = point.y - mouse.y;
            const float distanceSquared = dx * dx + dy * dy;
            if (distanceSquared <= nearestDistanceSquared)
            {
                nearestDistanceSquared = distanceSquared;
                nearest = static_cast<int>(vertex);
            }
        }
        if (nearest >= 0 && nearest != *selectedVertex)
        {
            *selectedVertex = nearest;
            result.selectionChanged = true;
        }
    }
    if (*selectedVertex >= 0 &&
        static_cast<size_t>(*selectedVertex) < selectableCount)
    {
        if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            ImGui::IsItemActive())
        {
            const float u = std::clamp((mouse.x - minimum.x) / size, 0.f, 1.f);
            const float v = std::clamp(1.f - (mouse.y - minimum.y) / size,
                0.f, 1.f);
            float& oldU = uvPairs[static_cast<size_t>(*selectedVertex) * 2];
            float& oldV = uvPairs[static_cast<size_t>(*selectedVertex) * 2 + 1];
            if (oldU != u || oldV != v)
            {
                oldU = u;
                oldV = v;
                result.coordinatesChanged = true;
            }
        }
        draw->AddCircleFilled(pointFor(static_cast<size_t>(*selectedVertex)),
            5.f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
    }
    draw->PopClipRect();
    ImGui::PopID();
    return result;
}
void ImGuiEditorUi::FocusWindow(const char*t){ImGui::SetWindowFocus(t);}
void ImGuiEditorUi::DockWindowToArea(const char* title,EditorPanelDockArea area)
{
    if(!title||!title[0]||area==EditorPanelDockArea::None)return;

    auto nodeForWindow=[&](const char* name)->ImGuiID{
        ImGuiWindow* window=ImGui::FindWindowByName(name);
        return window&&window->DockNode?window->DockNode->ID:0;
    };
    auto nodeForViewType=[&](const char* baseName)->ImGuiID{
        if(ImGuiID node=nodeForWindow(baseName))return node;
        for(int slot=2;slot<=32;++slot){
            const std::string numbered=std::string(baseName)+" "+std::to_string(slot);
            if(ImGuiID node=nodeForWindow(numbered.c_str()))return node;
        }
        return 0;
    };

    ImGuiID targetNode=0;
    switch(area)
    {
    case EditorPanelDockArea::MainDocument:
        targetNode=nodeForViewType("Scene");
        if(!targetNode)targetNode=nodeForViewType("Game");
        break;
    case EditorPanelDockArea::LeftSidebar:
        targetNode=nodeForWindow("Hierarchy");
        if(!targetNode)targetNode=nodeForWindow("Assets");
        break;
    case EditorPanelDockArea::RightSidebar:
        targetNode=nodeForWindow("Properties");
        break;
    case EditorPanelDockArea::BottomPanel:
        targetNode=nodeForWindow("Console");
        if(!targetNode)targetNode=nodeForWindow("Problems");
        if(!targetNode)targetNode=nodeForWindow("Terminal");
        break;
    default:
        break;
    }

    if(targetNode)
        ImGui::DockBuilderDockWindow(title,targetNode);
}
}
