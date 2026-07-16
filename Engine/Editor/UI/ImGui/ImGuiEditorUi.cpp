#include "pch.h"
#include "ImGuiEditorUi.h"
#include "imgui.h"

void ImGuiEditorUi::SetNextWindowRect(float x,float y,float w,float h){ ImGui::SetNextWindowPos({x,y},ImGuiCond_FirstUseEver); ImGui::SetNextWindowSize({w,h},ImGuiCond_FirstUseEver); }
bool ImGuiEditorUi::BeginWindow(const char* t,bool* o,bool p){ if(p) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0}); bool r=ImGui::Begin(t,o); if(p) ImGui::PopStyleVar(); return r; }
void ImGuiEditorUi::EndWindow(){ImGui::End();}
bool ImGuiEditorUi::Button(const char* l,float w,float h){return ImGui::Button(l,{w,h});}
void ImGuiEditorUi::Label(const char* t){ImGui::TextUnformatted(t);}
void ImGuiEditorUi::DisabledLabel(const char* t){ImGui::TextDisabled("%s",t);}
void ImGuiEditorUi::ColoredLabel(const char* t,EditorUiColor c){ImGui::TextColored({c.r,c.g,c.b,c.a},"%s",t);}
void ImGuiEditorUi::SameLine(){ImGui::SameLine();} void ImGuiEditorUi::Separator(){ImGui::Separator();} void ImGuiEditorUi::Spacing(){ImGui::Spacing();}
bool ImGuiEditorUi::Checkbox(const char*l,bool*v){return ImGui::Checkbox(l,v);} bool ImGuiEditorUi::InputText(const char*l,char*b,size_t s){return ImGui::InputText(l,b,s);}
bool ImGuiEditorUi::DragFloat(const char*l,float*v,float s,float a,float b){return ImGui::DragFloat(l,v,s,a,b);}
bool ImGuiEditorUi::DragFloat3(const char*l,float*v,float s,float a,float b){return ImGui::DragFloat3(l,v,s,a,b);}
bool ImGuiEditorUi::ColorEdit3(const char*l,float*v){return ImGui::ColorEdit3(l,v);} bool ImGuiEditorUi::ColorEdit4(const char*l,float*v){return ImGui::ColorEdit4(l,v);}
bool ImGuiEditorUi::SliderInt(const char*l,int*v,int a,int b){return ImGui::SliderInt(l,v,a,b);} bool ImGuiEditorUi::InputUInt(const char*l,uint32_t*v){return ImGui::InputScalar(l,ImGuiDataType_U32,v);}
void ImGuiEditorUi::ValueLabel(const char*l,const char*v){ImGui::LabelText(l,"%s",v);}
bool ImGuiEditorUi::CollapsingHeader(const char*l,bool d){return ImGui::CollapsingHeader(l,d?ImGuiTreeNodeFlags_DefaultOpen:0);}
bool ImGuiEditorUi::TreeNode(const void*id,const char*l,bool s,bool leaf,bool d){ImGuiTreeNodeFlags f=ImGuiTreeNodeFlags_OpenOnArrow|ImGuiTreeNodeFlags_SpanAvailWidth|(s?ImGuiTreeNodeFlags_Selected:0)|(d?ImGuiTreeNodeFlags_DefaultOpen:0);if(leaf)f|=ImGuiTreeNodeFlags_Leaf|ImGuiTreeNodeFlags_NoTreePushOnOpen;return ImGui::TreeNodeEx(id,f,"%s",l);}
void ImGuiEditorUi::TreePop(){ImGui::TreePop();}
bool ImGuiEditorUi::Selectable(const char*l,bool s,bool d){return ImGui::Selectable(l,s,d?ImGuiSelectableFlags_AllowDoubleClick:0);}
bool ImGuiEditorUi::BeginChild(const char*i){return ImGui::BeginChild(i,{0,0},false,ImGuiWindowFlags_HorizontalScrollbar);} void ImGuiEditorUi::EndChild(){ImGui::EndChild();}
bool ImGuiEditorUi::IsItemHovered()const{return ImGui::IsItemHovered();} bool ImGuiEditorUi::IsItemClicked()const{return ImGui::IsItemClicked()&&!ImGui::IsItemToggledOpen();}
bool ImGuiEditorUi::IsItemDoubleClicked()const{return ImGui::IsItemHovered()&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);}
bool ImGuiEditorUi::IsWindowBackgroundClicked()const{return ImGui::IsMouseClicked(ImGuiMouseButton_Left)&&ImGui::IsWindowHovered()&&!ImGui::IsAnyItemHovered();}
void ImGuiEditorUi::SetClipboardText(const char*t){ImGui::SetClipboardText(t);} void ImGuiEditorUi::ScrollToBottom(){ImGui::SetScrollHereY(1.f);}
bool ImGuiEditorUi::BeginTabBar(const char*i){return ImGui::BeginTabBar(i);} void ImGuiEditorUi::EndTabBar(){ImGui::EndTabBar();}
bool ImGuiEditorUi::BeginTab(const char*l){return ImGui::BeginTabItem(l);} void ImGuiEditorUi::EndTab(){ImGui::EndTabItem();}
void ImGuiEditorUi::BeginDisabled(bool d){ImGui::BeginDisabled(d);} void ImGuiEditorUi::EndDisabled(){ImGui::EndDisabled();}
bool ImGuiEditorUi::Combo(const char*l,int*s,const char*const*i,int c){return ImGui::Combo(l,s,i,c);}
void ImGuiEditorUi::Tooltip(const char*t){if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%s",t);}
void ImGuiEditorUi::Progress(float f,const char*o){ImGui::ProgressBar(f,{-1,0},o);}
EditorUiViewportInput ImGuiEditorUi::Viewport(void* texture,float aspect,EditorUiColor bg)
{
    EditorUiViewportInput out; ImVec2 a=ImGui::GetContentRegionAvail(); if(a.x<=1||a.y<=1)return out;
    ImVec2 size=a,pos{0,0}; if(aspect>0){float aa=a.x/a.y;if(aa>aspect){size.x=a.y*aspect;pos.x=(a.x-size.x)*.5f;}else{size.y=a.x/aspect;pos.y=(a.y-size.y)*.5f;}}
    if(size.x<a.x||size.y<a.y){ImVec2 p=ImGui::GetCursorScreenPos();ImGui::GetWindowDrawList()->AddRectFilled(p,{p.x+a.x,p.y+a.y},ImGui::GetColorU32({bg.r,bg.g,bg.b,bg.a}));}
    out.available={size.x,size.y}; ImGui::SetCursorPos(pos); ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)),size);
    out.hovered=ImGui::IsItemHovered();if(out.hovered){auto d=ImGui::GetIO().MouseDelta;out.mouseDelta={d.x,d.y};out.mouseWheel=ImGui::GetIO().MouseWheel;out.rightDown=ImGui::IsMouseDown(ImGuiMouseButton_Right);out.middleDown=ImGui::IsMouseDown(ImGuiMouseButton_Middle);}return out;
}
void ImGuiEditorUi::FocusWindow(const char*t){ImGui::SetWindowFocus(t);}
