#include "pch.h"
#include "ImGuiEditorUi.h"
#include "imgui.h"
#include "imgui_internal.h"

void ImGuiEditorUi::SetNextWindowRect(float x,float y,float w,float h){ ImGui::SetNextWindowPos({x,y},ImGuiCond_FirstUseEver); ImGui::SetNextWindowSize({w,h},ImGuiCond_FirstUseEver); }
bool ImGuiEditorUi::BeginWindow(const char* t,bool* o,bool p){ if(p) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0}); bool r=ImGui::Begin(t,o); if(p) ImGui::PopStyleVar(); return r; }
void ImGuiEditorUi::EndWindow(){ImGui::End();}
bool ImGuiEditorUi::IsWindowFocused() const{return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);}
void ImGuiEditorUi::PushId(const void* id){ImGui::PushID(id);}
void ImGuiEditorUi::PushId(const char* id){ImGui::PushID(id);}
void ImGuiEditorUi::PopId(){ImGui::PopID();}
bool ImGuiEditorUi::Button(const char* l,float w,float h){return ImGui::Button(l,{w,h});}
void ImGuiEditorUi::Label(const char* t){ImGui::TextUnformatted(t);}
void ImGuiEditorUi::DisabledLabel(const char* t){ImGui::TextDisabled("%s",t);}
void ImGuiEditorUi::ColoredLabel(const char* t,EditorUiColor c){ImGui::TextColored({c.r,c.g,c.b,c.a},"%s",t);}
void ImGuiEditorUi::BeginTextWrap(){ImGui::PushTextWrapPos(0.f);}
void ImGuiEditorUi::EndTextWrap(){ImGui::PopTextWrapPos();}
void ImGuiEditorUi::SameLine(){ImGui::SameLine();} void ImGuiEditorUi::Separator(){ImGui::Separator();} void ImGuiEditorUi::Spacing(){ImGui::Spacing();}
void ImGuiEditorUi::Indent(float width){ImGui::Indent(width);}
void ImGuiEditorUi::Unindent(float width){ImGui::Unindent(width);}
bool ImGuiEditorUi::Checkbox(const char*l,bool*v){return ImGui::Checkbox(l,v);} bool ImGuiEditorUi::InputText(const char*l,char*b,size_t s){if(l&&l[0]=='#'&&l[1]=='#')ImGui::SetNextItemWidth(-FLT_MIN);return ImGui::InputText(l,b,s);}
bool ImGuiEditorUi::InputTextSubmit(const char*l,char*b,size_t s){if(l&&l[0]=='#'&&l[1]=='#')ImGui::SetNextItemWidth(-FLT_MIN);return ImGui::InputText(l,b,s,ImGuiInputTextFlags_EnterReturnsTrue);}
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
bool ImGuiEditorUi::DragFloat(const char*l,float*v,float s,float a,float b){return ImGui::DragFloat(l,v,s,a,b);}
bool ImGuiEditorUi::DragFloat3(const char*l,float*v,float s,float a,float b){return ImGui::DragFloat3(l,v,s,a,b);}
bool ImGuiEditorUi::ColorEdit3(const char*l,float*v){return ImGui::ColorEdit3(l,v);} bool ImGuiEditorUi::ColorEdit4(const char*l,float*v){return ImGui::ColorEdit4(l,v);}
bool ImGuiEditorUi::SliderInt(const char*l,int*v,int a,int b){return ImGui::SliderInt(l,v,a,b);} bool ImGuiEditorUi::InputUInt(const char*l,uint32_t*v){return ImGui::InputScalar(l,ImGuiDataType_U32,v);}
void ImGuiEditorUi::ValueLabel(const char*l,const char*v){ImGui::LabelText(l,"%s",v);}
bool ImGuiEditorUi::CollapsingHeader(const char*l,bool d){return ImGui::CollapsingHeader(l,d?ImGuiTreeNodeFlags_DefaultOpen:0);}
bool ImGuiEditorUi::TreeNode(const void*id,const char*l,bool s,bool leaf,bool d){ImGuiTreeNodeFlags f=ImGuiTreeNodeFlags_OpenOnArrow|ImGuiTreeNodeFlags_SpanAvailWidth|(s?ImGuiTreeNodeFlags_Selected:0)|(d?ImGuiTreeNodeFlags_DefaultOpen:0);if(leaf)f|=ImGuiTreeNodeFlags_Leaf|ImGuiTreeNodeFlags_NoTreePushOnOpen;return ImGui::TreeNodeEx(id,f,"%s",l);}
void ImGuiEditorUi::TreePop(){ImGui::TreePop();}
EditorUiObjectRowResult ImGuiEditorUi::ObjectTreeRow(const void* id,char* name,size_t size,bool* enabled,bool selected,bool leaf,bool lockName,bool enabledInHierarchy,int hierarchyDepth,bool lastSibling,uint64_t ancestorGuideMask)
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
        ImGui::SetNextItemOpen(openState->second,ImGuiCond_Always);
    ImGuiTreeNodeFlags flags=ImGuiTreeNodeFlags_OpenOnArrow|
        ImGuiTreeNodeFlags_SpanAvailWidth|ImGuiTreeNodeFlags_FramePadding|
        ImGuiTreeNodeFlags_AllowOverlap|
        (selected?ImGuiTreeNodeFlags_Selected:0);
    if(leaf)flags|=ImGuiTreeNodeFlags_Leaf|ImGuiTreeNodeFlags_NoTreePushOnOpen;
    ImGui::SetNextItemAllowOverlap();
    result.open=ImGui::TreeNodeEx("##object",flags,"");
    if(hasChildren)
        openState->second=result.open;
    const ImVec2 rowMinimum=ImGui::GetItemRectMin();
    const ImVec2 rowMaximum=ImGui::GetItemRectMax();
    const float hierarchyIndent=std::max(ImGui::GetStyle().IndentSpacing,1.f);
    const float branchX=rowMinimum.x-hierarchyIndent*.5f;
    const float rowCenterY=(rowMinimum.y+rowMaximum.y)*.5f;
    ImVec4 guideColor=ImGui::GetStyleColorVec4(ImGuiCol_Separator);
    guideColor.w*=.7f;
    ImDrawList* hierarchyDraw=ImGui::GetWindowDrawList();
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
    result.enabledChanged=ImGui::Checkbox("##enabled",enabled);
    result.clicked=result.clicked||ImGui::IsItemClicked();
    ImGui::SameLine(0.f,4.f);
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
    ImGui::Dummy({std::max(ImGui::GetContentRegionAvail().x,1.f),ImGui::GetFrameHeight()});
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
    ImGui::TextUnformatted("Object");
    ImGui::Separator();
    const float available=ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(available>110.f?available-100.f:available*0.65f);
    ImGui::BeginDisabled(lockName);
    result.nameChanged=ImGui::InputText("##name",name,size);
    ImGui::EndDisabled();
    ImGui::SameLine();
    result.enabledChanged=ImGui::Checkbox("Enabled",enabled);
    ImGui::PopID();
    return result;
}
bool ImGuiEditorUi::Selectable(const char*l,bool s,bool d){return ImGui::Selectable(l,s,d?ImGuiSelectableFlags_AllowDoubleClick:0);}
EditorUiContextMenuResult ImGuiEditorUi::ContextMenu(const void* id,const char* addLabel,const char* deleteLabel,bool objectCreationMenu,const char* unpackLabel)
{
    EditorUiContextMenuResult result;
    const bool hasAdd=addLabel&&addLabel[0];
    const bool hasDelete=deleteLabel&&deleteLabel[0];
    const bool hasUnpack=unpackLabel&&unpackLabel[0];
    if(!hasAdd&&!hasDelete&&!hasUnpack)return result;
    ImGui::PushID(id);
    if(ImGui::BeginPopupContextItem("##context")){
        if(hasAdd){
            if(objectCreationMenu){
                if(ImGui::BeginMenu(addLabel)){
                    result.addRequested=ImGui::MenuItem("Empty");
                    result.addCubeRequested=ImGui::MenuItem("Cube");
                    result.addSpriteRequested=ImGui::MenuItem("Sprite");
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
        ImGui::BeginDisabled(!hasOverrides);
        result.applyRequested=ImGui::MenuItem("Apply Overrides to Prefab");
        result.applyAllRequested=ImGui::MenuItem("Apply All to Prefab (Including Transform)");
        result.revertRequested=ImGui::MenuItem("Revert Overrides");
        ImGui::EndDisabled();
        ImGui::Separator();
        result.unpackRequested=ImGui::MenuItem("Unpack Prefab");
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return result;
}
bool ImGuiEditorUi::BeginChild(const char*i){return ImGui::BeginChild(i,{0,0},false,ImGuiWindowFlags_HorizontalScrollbar);} void ImGuiEditorUi::EndChild(){ImGui::EndChild();}
bool ImGuiEditorUi::IsItemHovered()const{return ImGui::IsItemHovered();} bool ImGuiEditorUi::IsItemClicked()const{return ImGui::IsItemClicked()&&!ImGui::IsItemToggledOpen();}
bool ImGuiEditorUi::IsItemDoubleClicked()const{return ImGui::IsItemHovered()&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);}
bool ImGuiEditorUi::IsWindowBackgroundClicked()const{return ImGui::IsMouseClicked(ImGuiMouseButton_Left)&&ImGui::IsWindowHovered()&&!ImGui::IsAnyItemHovered();}
bool ImGuiEditorUi::CopyShortcutPressed()const{return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)&&ImGui::GetIO().KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_C,false);}
bool ImGuiEditorUi::PasteShortcutPressed()const{return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)&&ImGui::GetIO().KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_V,false);}
bool ImGuiEditorUi::BeginDragDropSource(){
    const ImVec2 minimum=ImGui::GetItemRectMin(),maximum=ImGui::GetItemRectMax();
    ImDrawList* rowDrawList=ImGui::GetWindowDrawList();
    if(!ImGui::BeginDragDropSource())return false;
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
void ImGuiEditorUi::BeginDisabled(bool d){ImGui::BeginDisabled(d);} void ImGuiEditorUi::EndDisabled(){ImGui::EndDisabled();}
bool ImGuiEditorUi::Combo(const char*l,int*s,const char*const*i,int c){return ImGui::Combo(l,s,i,c);}
void ImGuiEditorUi::Tooltip(const char*t){if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%s",t);}
void ImGuiEditorUi::Progress(float f,const char*o){ImGui::ProgressBar(f,{-1,0},o);}
void ImGuiEditorUi::DrawImage(void*tex,float w,float h){ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(tex)),{w,h});}
EditorUiViewportInput ImGuiEditorUi::Viewport(void* texture,float aspect,EditorUiColor bg)
{
    EditorUiViewportInput out; ImVec2 a=ImGui::GetContentRegionAvail(); if(a.x<=1||a.y<=1)return out;
    ImVec2 size=a,pos{0,0}; if(aspect>0){float aa=a.x/a.y;if(aa>aspect){size.x=a.y*aspect;pos.x=(a.x-size.x)*.5f;}else{size.y=a.x/aspect;pos.y=(a.y-size.y)*.5f;}}
    if(size.x<a.x||size.y<a.y){ImVec2 p=ImGui::GetCursorScreenPos();ImGui::GetWindowDrawList()->AddRectFilled(p,{p.x+a.x,p.y+a.y},ImGui::GetColorU32({bg.r,bg.g,bg.b,bg.a}));}
    out.available={size.x,size.y}; ImGui::SetCursorPos(pos); ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)),size);
    const ImVec2 min=ImGui::GetItemRectMin();const ImVec2 max=ImGui::GetItemRectMax();const ImVec2 mp=ImGui::GetIO().MousePos;out.mousePosInViewport={mp.x-min.x,mp.y-min.y};
    m_viewportScreenMin={min.x,min.y};m_viewportScreenMax={max.x,max.y};
    out.leftDown=ImGui::IsMouseDown(ImGuiMouseButton_Left);out.leftReleased=ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    out.hovered=ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);if(out.hovered||out.leftDown){auto d=ImGui::GetIO().MouseDelta;out.mouseDelta={d.x,d.y};}if(out.hovered){out.mouseWheel=ImGui::GetIO().MouseWheel;out.rightDown=ImGui::IsMouseDown(ImGuiMouseButton_Right);out.middleDown=ImGui::IsMouseDown(ImGuiMouseButton_Middle);out.leftClicked=ImGui::IsMouseClicked(ImGuiMouseButton_Left);}return out;
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
void ImGuiEditorUi::FocusWindow(const char*t){ImGui::SetWindowFocus(t);}
