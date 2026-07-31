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
void NuklearEditorUi::Layout(float h){
    if(m_sameLine){
        if(m_sameLineCount==0)nk_layout_row_begin(CTX,NK_DYNAMIC,h,2);
        nk_layout_row_push(CTX,m_sameLineCount==0?0.7f:0.3f);
        m_sameLineCount++;
    }else{
        if(m_sameLineCount>0){nk_layout_row_end(CTX);m_sameLineCount=0;}
        nk_layout_row_dynamic(CTX,h,1);
    }
}
void NuklearEditorUi::RecordNextWidgetBounds(){const struct nk_rect b=nk_widget_bounds(CTX);m_lastItemX=b.x;m_lastItemY=b.y;m_lastItemW=b.w;m_lastItemH=b.h;}
void NuklearEditorUi::SetNextWindowRect(float x,float y,float w,float h){m_window.SetNextRect(x,y,w,h);}
bool NuklearEditorUi::BeginWindow(const char* title,bool* open,bool){return m_window.Begin(title,open);}
void NuklearEditorUi::EndWindow(){m_window.End();}
bool NuklearEditorUi::Button(const char*l,float,float h){Layout(h>0?h:28);m_lastClicked=!m_disabled&&nk_button_label(CTX,l);if(m_lastClicked)m_sameLine=false;return m_lastClicked;}
void NuklearEditorUi::Label(const char*t){Layout();nk_label(CTX,t,NK_TEXT_LEFT);m_sameLine=false;} void NuklearEditorUi::DisabledLabel(const char*t){Label(t);} void NuklearEditorUi::ColoredLabel(const char*t,EditorUiColor c){Layout();nk_label_colored(CTX,t,NK_TEXT_LEFT,nk_rgba_f(c.r,c.g,c.b,c.a));m_sameLine=false;}
void NuklearEditorUi::SameLine(){m_sameLine=true;} void NuklearEditorUi::Separator(){m_sameLine=false;if(m_sameLineCount>0){nk_layout_row_end(CTX);m_sameLineCount=0;}Layout(3);nk_rule_horizontal(CTX,nk_rgb(80,80,80),nk_true);} void NuklearEditorUi::Spacing(){m_sameLine=false;if(m_sameLineCount>0){nk_layout_row_end(CTX);m_sameLineCount=0;}Layout(8);nk_spacing(CTX,1);}
bool NuklearEditorUi::Checkbox(const char*l,bool*v){Layout();int a=*v;m_lastClicked=nk_checkbox_label(CTX,l,&a)!=0;*v=a!=0;return m_lastClicked;}
bool NuklearEditorUi::InputText(const char*l,char*b,size_t s){Label(l);Layout();int len=static_cast<int>(strlen(b));nk_flags r=nk_edit_string(CTX,NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,b,&len,static_cast<int>(s)-1,nk_filter_default);b[len]=0;m_sameLine=false;return (r&NK_EDIT_COMMITED)!=0;}
bool NuklearEditorUi::DragFloat(const char*l,float*v,float step,float lo,float hi){Layout();float before=*v;nk_property_float(CTX,l,lo==hi?-100000.f:lo,v,lo==hi?100000.f:hi,step,step*.1f);m_sameLine=false;return before!=*v;}
bool NuklearEditorUi::DragFloat3(const char*l,float*v,float s,float lo,float hi){
    Layout();
    nk_layout_row_begin(CTX,NK_DYNAMIC,24,4);
    nk_layout_row_push(CTX,0.25f);nk_label(CTX,l,NK_TEXT_LEFT);
    nk_layout_row_push(CTX,0.25f);float x=v[0];nk_property_float(CTX,"X",lo==hi?-100000.f:lo,&v[0],lo==hi?100000.f:hi,s,s*.1f);
    nk_layout_row_push(CTX,0.25f);float y=v[1];nk_property_float(CTX,"Y",lo==hi?-100000.f:lo,&v[1],lo==hi?100000.f:hi,s,s*.1f);
    nk_layout_row_push(CTX,0.25f);float z=v[2];nk_property_float(CTX,"Z",lo==hi?-100000.f:lo,&v[2],lo==hi?100000.f:hi,s,s*.1f);
    nk_layout_row_end(CTX);
    m_sameLine=false;
    return x!=v[0]||y!=v[1]||z!=v[2];
}
bool NuklearEditorUi::ColorEdit3(const char*l,float*c){
    Layout();
    struct nk_colorf color={c[0],c[1],c[2],1.f};
    nk_layout_row_begin(CTX,NK_DYNAMIC,120,2);
    nk_layout_row_push(CTX,0.3f);nk_label(CTX,l,NK_TEXT_LEFT);
    nk_layout_row_push(CTX,0.7f);
    if(nk_combo_begin_color(CTX,nk_rgb_cf(color),nk_vec2(200,400))){
        nk_layout_row_dynamic(CTX,120,1);
        color=nk_color_picker(CTX,color,NK_RGB);
        nk_layout_row_dynamic(CTX,24,1);
        color.r=nk_propertyf(CTX,"R:",0,color.r,1.f,0.01f,0.005f);
        color.g=nk_propertyf(CTX,"G:",0,color.g,1.f,0.01f,0.005f);
        color.b=nk_propertyf(CTX,"B:",0,color.b,1.f,0.01f,0.005f);
        nk_combo_end(CTX);
    }
    nk_layout_row_end(CTX);
    m_sameLine=false;
    bool changed=(c[0]!=color.r||c[1]!=color.g||c[2]!=color.b);
    c[0]=color.r;c[1]=color.g;c[2]=color.b;
    return changed;
} 
bool NuklearEditorUi::ColorEdit4(const char*l,float*c){
    bool r=ColorEdit3(l,c);
    return DragFloat("Alpha",c+3,.01f,0,1)||r;
}
bool NuklearEditorUi::SliderInt(const char*l,int*v,int a,int b){Layout();int old=*v;nk_property_int(CTX,l,a,v,b,1,1);return old!=*v;} bool NuklearEditorUi::InputUInt(const char*l,uint32_t*v){int n=static_cast<int>(*v);bool c=SliderInt(l,&n,0,100000);*v=static_cast<uint32_t>(n);return c;}
void NuklearEditorUi::ValueLabel(const char*l,const char*v){std::string s=std::string(l)+": "+v;Label(s.c_str());}
bool NuklearEditorUi::CollapsingHeader(const char* label,bool defaultOpen)
{
    const std::string key=m_window.CurrentWindow()+'\x1f'+label;
    auto [state,inserted]=m_collapsingHeaders.emplace(key,defaultOpen);
    (void)inserted;
    const std::string display=std::string(state->second?"[-] ":"[+] ")+label;
    Layout();
    RecordNextWidgetBounds();
    m_lastClicked=!m_disabled&&nk_button_label(CTX,display.c_str())!=0;
    if(m_lastClicked) state->second=!state->second;
    return state->second;
}
bool NuklearEditorUi::TreeNode(const void* id,const char* label,bool selected,bool leaf,bool defaultOpen)
{
    Layout();
    RecordNextWidgetBounds();
    if(leaf)
    {
        int active=selected?1:0;
        m_lastClicked=nk_selectable_label(CTX,label,NK_TEXT_LEFT,&active)!=0;
        return false;
    }
    std::string display=selected?std::string("> ")+label:label;
    const bool open=nk_tree_push_hashed(CTX,NK_TREE_TAB,display.c_str(),
        defaultOpen?NK_MAXIMIZED:NK_MINIMIZED,
        reinterpret_cast<const char*>(&id),sizeof(id),0)!=0;
    const struct nk_rect bounds=nk_rect(m_lastItemX,m_lastItemY,m_lastItemW,m_lastItemH);
    m_lastClicked=nk_input_is_mouse_click_in_rect(&CTX->input,NK_BUTTON_LEFT,bounds)!=0;
    return open;
}
void NuklearEditorUi::TreePop(){nk_tree_pop(CTX);}
bool NuklearEditorUi::Selectable(const char*l,bool s,bool){Layout();RecordNextWidgetBounds();int selected=s;m_lastClicked=nk_selectable_label(CTX,l,NK_TEXT_LEFT,&selected)!=0;return m_lastClicked;}
bool NuklearEditorUi::BeginChild(const char*i){return nk_group_begin(CTX,i,NK_WINDOW_BORDER|NK_WINDOW_SCROLL_AUTO_HIDE)!=0;} void NuklearEditorUi::EndChild(){nk_group_end(CTX);}
bool NuklearEditorUi::IsItemHovered()const{const struct nk_rect b=nk_rect(m_lastItemX,m_lastItemY,m_lastItemW,m_lastItemH);return nk_input_is_mouse_hovering_rect(&CTX->input,b)!=0;} bool NuklearEditorUi::IsItemClicked()const{return m_lastClicked;} bool NuklearEditorUi::IsItemDoubleClicked()const{const struct nk_rect b=nk_rect(m_lastItemX,m_lastItemY,m_lastItemW,m_lastItemH);return nk_input_is_mouse_click_in_rect(&CTX->input,NK_BUTTON_DOUBLE,b)!=0;}
bool NuklearEditorUi::IsWindowBackgroundClicked()const{return false;} void NuklearEditorUi::SetClipboardText(const char*t){if(OpenClipboard(nullptr)){EmptyClipboard();size_t n=strlen(t)+1;HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,n);if(h){memcpy(GlobalLock(h),t,n);GlobalUnlock(h);SetClipboardData(CF_TEXT,h);}CloseClipboard();}} void NuklearEditorUi::ScrollToBottom(){}
bool NuklearEditorUi::BeginDragDropSource(){return IsItemHovered()&&nk_input_is_mouse_down(&CTX->input,NK_BUTTON_LEFT)!=0;}
void NuklearEditorUi::SetDragDropPayload(const char*t,const void*d,size_t s){m_dragPayloadType=t?t:"";const auto*b=static_cast<const unsigned char*>(d);m_dragPayload.assign(b,b+s);}
void NuklearEditorUi::EndDragDropSource(){}
bool NuklearEditorUi::BeginDragDropTarget(){m_dragPayloadDelivered=false;return !m_dragPayload.empty()&&IsItemHovered();}
const void* NuklearEditorUi::AcceptDragDropPayload(const char*t,size_t*s){if(m_dragPayloadType!=(t?t:"")||!nk_input_is_mouse_released(&CTX->input,NK_BUTTON_LEFT))return nullptr;if(s)*s=m_dragPayload.size();m_dragPayloadDelivered=true;return m_dragPayload.data();}
void NuklearEditorUi::EndDragDropTarget(){if(m_dragPayloadDelivered){m_dragPayload.clear();m_dragPayloadType.clear();m_dragPayloadDelivered=false;}}
bool NuklearEditorUi::BeginTabBar(const char*){return true;}void NuklearEditorUi::EndTabBar(){}bool NuklearEditorUi::BeginTab(const char*l){return TreeNode(l,l,false,false,false);}void NuklearEditorUi::EndTab(){TreePop();}
void NuklearEditorUi::BeginDisabled(bool d){m_disabled=d;}void NuklearEditorUi::EndDisabled(){m_disabled=false;}
bool NuklearEditorUi::Combo(const char*l,int*s,const char*const*i,int c){Layout();int old=*s;*s=nk_combo(CTX,i,c,*s,24,nk_vec2(240,200));m_sameLine=false;return old!=*s;}
void NuklearEditorUi::Tooltip(const char*t){if(IsItemHovered()&&t&&t[0])nk_tooltip(CTX,t);}
void NuklearEditorUi::Progress(float f,const char*){Layout();nk_size v=static_cast<nk_size>(f*1000);nk_progress(CTX,&v,1000,NK_FIXED);m_sameLine=false;}
void NuklearEditorUi::DrawImage(void*tex,float w,float h){Layout(h);nk_image(CTX,nk_image_ptr(tex));m_sameLine=false;}
EditorUiViewportInput NuklearEditorUi::Viewport(void*tex,float aspect,EditorUiColor)
{EditorUiViewportInput o;struct nk_rect r=nk_window_get_content_region(CTX);float w=r.w,h=r.h;if(aspect>0){if(w/h>aspect)w=h*aspect;else h=w/aspect;}Layout(h);RecordNextWidgetBounds();nk_image(CTX,nk_image_ptr(tex));const struct nk_rect b=nk_rect(m_lastItemX,m_lastItemY,m_lastItemW,m_lastItemH);o.available={w,h};o.hovered=nk_input_is_mouse_hovering_rect(&CTX->input,b)!=0;o.mouseDelta={CTX->input.mouse.delta.x,CTX->input.mouse.delta.y};o.mouseWheel=CTX->input.mouse.scroll_delta.y;o.rightDown=nk_input_is_mouse_down(&CTX->input,NK_BUTTON_RIGHT)!=0;o.middleDown=nk_input_is_mouse_down(&CTX->input,NK_BUTTON_MIDDLE)!=0;o.leftClicked=nk_input_is_mouse_click_in_rect(&CTX->input,NK_BUTTON_LEFT,b)!=0;if(o.hovered){o.mousePosInViewport={CTX->input.mouse.pos.x-b.x,CTX->input.mouse.pos.y-b.y};}return o;}
void NuklearEditorUi::FocusWindow(const char*){}
#undef CTX
