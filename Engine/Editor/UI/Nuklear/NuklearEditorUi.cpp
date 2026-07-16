#include "pch.h"
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_API extern "C"
#include "nuklear.h"
#include "NuklearEditorUi.h"

#define CTX static_cast<nk_context*>(m_context)
void NuklearEditorUi::Layout(float h){nk_layout_row_dynamic(CTX,h,1);}
void NuklearEditorUi::SetNextWindowRect(float x,float y,float w,float h){m_x=x;m_y=y;m_w=w;m_h=h;}
bool NuklearEditorUi::BeginWindow(const char*t,bool*o,bool){if(o&&!*o)return false;return nk_begin(CTX,t,nk_rect(m_x,m_y,m_w,m_h),NK_WINDOW_BORDER|NK_WINDOW_TITLE|NK_WINDOW_SCROLL_AUTO_HIDE)!=0;}
void NuklearEditorUi::EndWindow(){nk_end(CTX);} bool NuklearEditorUi::Button(const char*l,float,float h){Layout(h>0?h:28);m_lastClicked=!m_disabled&&nk_button_label(CTX,l);return m_lastClicked;}
void NuklearEditorUi::Label(const char*t){Layout();nk_label(CTX,t,NK_TEXT_LEFT);} void NuklearEditorUi::DisabledLabel(const char*t){Label(t);} void NuklearEditorUi::ColoredLabel(const char*t,EditorUiColor c){Layout();nk_label_colored(CTX,t,NK_TEXT_LEFT,nk_rgba_f(c.r,c.g,c.b,c.a));}
void NuklearEditorUi::SameLine(){} void NuklearEditorUi::Separator(){Layout(3);nk_rule_horizontal(CTX,nk_rgb(80,80,80),nk_true);} void NuklearEditorUi::Spacing(){Layout(8);nk_spacing(CTX,1);}
bool NuklearEditorUi::Checkbox(const char*l,bool*v){Layout();int a=*v;m_lastClicked=nk_checkbox_label(CTX,l,&a)!=0;*v=a!=0;return m_lastClicked;}
bool NuklearEditorUi::InputText(const char*l,char*b,size_t s){Label(l);Layout();int len=static_cast<int>(strlen(b));nk_flags r=nk_edit_string(CTX,NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,b,&len,static_cast<int>(s)-1,nk_filter_default);b[len]=0;return (r&NK_EDIT_COMMITED)!=0;}
bool NuklearEditorUi::DragFloat(const char*l,float*v,float step,float lo,float hi){Layout();float before=*v;nk_property_float(CTX,l,lo==hi?-100000.f:lo,v,lo==hi?100000.f:hi,step,step*.1f);return before!=*v;}
bool NuklearEditorUi::DragFloat3(const char*l,float*v,float s,float lo,float hi){bool c=false;for(int i=0;i<3;i++){std::string n=std::string(l)+" "+char('X'+i);c|=DragFloat(n.c_str(),v+i,s,lo,hi);}return c;}
bool NuklearEditorUi::ColorEdit3(const char*l,float*c){return DragFloat3(l,c,.01f,0,1);} bool NuklearEditorUi::ColorEdit4(const char*l,float*c){bool r=ColorEdit3(l,c);return DragFloat("Alpha",c+3,.01f,0,1)||r;}
bool NuklearEditorUi::SliderInt(const char*l,int*v,int a,int b){Layout();int old=*v;nk_property_int(CTX,l,a,v,b,1,1);return old!=*v;} bool NuklearEditorUi::InputUInt(const char*l,uint32_t*v){int n=static_cast<int>(*v);bool c=SliderInt(l,&n,0,100000);*v=static_cast<uint32_t>(n);return c;}
void NuklearEditorUi::ValueLabel(const char*l,const char*v){std::string s=std::string(l)+": "+v;Label(s.c_str());}
bool NuklearEditorUi::CollapsingHeader(const char*l,bool d){return TreeNode(l,l,false,false,d);} bool NuklearEditorUi::TreeNode(const void*id,const char*l,bool,bool leaf,bool d){Layout();bool open=nk_tree_push_hashed(CTX,NK_TREE_TAB,l,d?NK_MAXIMIZED:NK_MINIMIZED,reinterpret_cast<const char*>(&id),sizeof(id),0)!=0;if(leaf&&open){nk_tree_pop(CTX);return false;}return open;} void NuklearEditorUi::TreePop(){nk_tree_pop(CTX);}
bool NuklearEditorUi::Selectable(const char*l,bool s,bool){Layout();int selected=s;m_lastClicked=nk_selectable_label(CTX,l,NK_TEXT_LEFT,&selected)!=0;return m_lastClicked;}
bool NuklearEditorUi::BeginChild(const char*i){return nk_group_begin(CTX,i,NK_WINDOW_BORDER|NK_WINDOW_SCROLL_AUTO_HIDE)!=0;} void NuklearEditorUi::EndChild(){nk_group_end(CTX);}
bool NuklearEditorUi::IsItemHovered()const{return nk_widget_is_hovered(CTX)!=0;} bool NuklearEditorUi::IsItemClicked()const{return m_lastClicked;} bool NuklearEditorUi::IsItemDoubleClicked()const{return CTX->input.mouse.buttons[NK_BUTTON_DOUBLE].clicked!=0&&nk_widget_is_hovered(CTX)!=0;}
bool NuklearEditorUi::IsWindowBackgroundClicked()const{return false;} void NuklearEditorUi::SetClipboardText(const char*t){if(OpenClipboard(nullptr)){EmptyClipboard();size_t n=strlen(t)+1;HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,n);if(h){memcpy(GlobalLock(h),t,n);GlobalUnlock(h);SetClipboardData(CF_TEXT,h);}CloseClipboard();}} void NuklearEditorUi::ScrollToBottom(){}
bool NuklearEditorUi::BeginTabBar(const char*){return true;}void NuklearEditorUi::EndTabBar(){}bool NuklearEditorUi::BeginTab(const char*l){return CollapsingHeader(l,false);}void NuklearEditorUi::EndTab(){TreePop();}
void NuklearEditorUi::BeginDisabled(bool d){m_disabled=d;}void NuklearEditorUi::EndDisabled(){m_disabled=false;}
bool NuklearEditorUi::Combo(const char*l,int*s,const char*const*i,int c){Layout();int old=*s;*s=nk_combo(CTX,i,c,*s,24,nk_vec2(240,200));return old!=*s;}void NuklearEditorUi::Tooltip(const char*){}void NuklearEditorUi::Progress(float f,const char*){Layout();nk_size v=static_cast<nk_size>(f*1000);nk_progress(CTX,&v,1000,NK_FIXED);}
EditorUiViewportInput NuklearEditorUi::Viewport(void*tex,float aspect,EditorUiColor)
{EditorUiViewportInput o;struct nk_rect r=nk_window_get_content_region(CTX);float w=r.w,h=r.h;if(aspect>0){if(w/h>aspect)w=h*aspect;else h=w/aspect;}Layout(h);nk_image(CTX,nk_image_ptr(tex));struct nk_rect b=nk_widget_bounds(CTX);o.available={w,h};o.hovered=nk_input_is_mouse_hovering_rect(&CTX->input,b)!=0;o.mouseDelta={CTX->input.mouse.delta.x,CTX->input.mouse.delta.y};o.mouseWheel=CTX->input.mouse.scroll_delta.y;o.rightDown=nk_input_is_mouse_down(&CTX->input,NK_BUTTON_RIGHT)!=0;o.middleDown=nk_input_is_mouse_down(&CTX->input,NK_BUTTON_MIDDLE)!=0;return o;}
void NuklearEditorUi::FocusWindow(const char*){}
#undef CTX
