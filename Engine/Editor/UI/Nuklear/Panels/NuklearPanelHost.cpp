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

#include "Engine/Editor/UI/Nuklear/Panels/NuklearPanelHost.h"
#include "Engine/Editor/UI/Nuklear/NuklearEditorUi.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/Core/View/IEditorPanel.h"
#include "Engine/Editor/Core/View/View.h"
#include "Engine/Editor/Core/View/ViewFactory.h"
#include "Engine/Editor/Core/View/Views/AssetsExplorerView.h"
#include "Engine/Editor/Core/View/Views/ConsoleView.h"
#include "Engine/Editor/Core/View/Views/HierarchyView.h"
#include "Engine/Editor/Core/View/Views/PreferencesView.h"
#include "Engine/Editor/Core/View/Views/PropertiesView.h"

#include <algorithm>
#include <vector>

namespace
{
void EnsureActive(IEditorPanel*& active, const std::vector<IEditorPanel*>& tabs)
{
    if (std::find(tabs.begin(), tabs.end(), active) == tabs.end())
        active = tabs.empty() ? nullptr : tabs.front();
}

void DrawTabs(nk_context& context, const char* hostName,
    float x, float y, float width,
    const std::vector<IEditorPanel*>& tabs, IEditorPanel*& active)
{
    if (tabs.empty()) return;
    const struct nk_rect bounds = nk_rect(x, y, width, 32.f);
    nk_window_set_bounds(&context, hostName, bounds);
    if (nk_begin(&context, hostName, bounds,
        NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR))
    {
        nk_layout_row_dynamic(&context, 24.f, static_cast<int>(tabs.size()));
        for (IEditorPanel* panel : tabs)
        {
            int selected = panel == active ? 1 : 0;
            if (nk_selectable_label(
                &context, panel->GetTitle().c_str(), NK_TEXT_CENTERED, &selected))
                active = panel;
        }
    }
    nk_end(&context);
}
}

void NuklearPanelHost::OpenPanel(EditorState& state, const char* type)
{
    ViewFactory* factory = state.GetViewFactory();
    if (!factory) return;

    if (ViewFactory::IsSingleton(type))
    {
        const std::string titlePrefix = std::string(type) + " ";
        for (auto& existing : state.GetPanels())
        {
            if (!existing || existing->GetTitle().rfind(titlePrefix, 0) != 0)
                continue;
            existing->SetOpen(true);
            if (dynamic_cast<HierarchyView*>(existing.get()) ||
                dynamic_cast<AssetsExplorerView*>(existing.get()))
                m_activeLeftTab = existing.get();
            return;
        }
    }

    auto panel = factory->Create(type);
    if (!panel) return;
    IEditorPanel* created = panel.get();
    state.GetPanels().push_back(std::move(panel));
    if (created->NeedsRender())
        m_activeCenterTab = created;
    else if (dynamic_cast<HierarchyView*>(created) ||
             dynamic_cast<AssetsExplorerView*>(created))
        m_activeLeftTab = created;
}

void NuklearPanelHost::DrawDockedPanels(nk_context& context, NuklearEditorUi& ui,
    EditorState& state, const NuklearDockGeometry& geometry)
{
    std::vector<IEditorPanel*> leftTabs;
    std::vector<IEditorPanel*> centerTabs;
    IEditorPanel* propertiesPanel = nullptr;
    IEditorPanel* consolePanel = nullptr;

    for (auto& panel : state.GetPanels())
    {
        if (!panel || !panel->IsOpen()) continue;
        if (dynamic_cast<PropertiesView*>(panel.get()))
            propertiesPanel = panel.get();
        else if (dynamic_cast<ConsoleView*>(panel.get()))
            consolePanel = panel.get();
        else if (panel->NeedsRender())
            centerTabs.push_back(panel.get());
        else
            leftTabs.push_back(panel.get());
    }

    EnsureActive(m_activeLeftTab, leftTabs);
    EnsureActive(m_activeCenterTab, centerTabs);
    DrawTabs(context, "Left Dock Tabs", 0.f, geometry.toolbarHeight,
        geometry.left, leftTabs, m_activeLeftTab);
    DrawTabs(context, "Center Dock Tabs", geometry.CenterX(), geometry.toolbarHeight,
        geometry.center, centerTabs, m_activeCenterTab);

    constexpr float tabHeight = 32.f;
    if (m_activeLeftTab)
    {
        ui.SetNextWindowRect(0.f, geometry.toolbarHeight + tabHeight,
            geometry.left, geometry.workspace - tabHeight);
        m_activeLeftTab->DrawPanel(ui);
    }
    if (m_activeCenterTab)
    {
        ui.SetNextWindowRect(geometry.CenterX(), geometry.toolbarHeight + tabHeight,
            geometry.center, geometry.workspace - tabHeight);
        m_activeCenterTab->DrawPanel(ui);
    }
    if (propertiesPanel)
    {
        ui.SetNextWindowRect(geometry.RightX(), geometry.toolbarHeight,
            geometry.right, geometry.workspace);
        propertiesPanel->DrawPanel(ui);
    }
    if (consolePanel)
    {
        ui.SetNextWindowRect(0.f,
            geometry.toolbarHeight + geometry.workspace + geometry.splitterSize,
            geometry.width, geometry.console);
        consolePanel->DrawPanel(ui);
    }
}

void NuklearPanelHost::DrawPreferences(NuklearEditorUi& ui, EditorState& state,
    float width, float height) const
{
    bool showPreferences = state.IsShowingPreferences();
    if (!showPreferences || !state.GetPreferences()) return;
    ui.SetNextWindowRect((width - 600.f) * .5f, 60.f, 600.f,
        std::max(400.f, height - 120.f));
    state.GetPreferences()->DrawWindow(ui, showPreferences);
    state.SetShowPreferences(showPreferences);
}

void NuklearPanelHost::CleanupClosedPanels(EditorState& state)
{
    auto& panels = state.GetPanels();
    ViewFactory* factory = state.GetViewFactory();
    for (auto it = panels.begin(); it != panels.end();)
    {
        IEditorPanel* panel = it->get();
        if (!panel || panel->IsOpen() || !panel->NeedsRender())
        {
            ++it;
            continue;
        }
        if (m_activeLeftTab == panel) m_activeLeftTab = nullptr;
        if (m_activeCenterTab == panel) m_activeCenterTab = nullptr;
        if (auto* view = dynamic_cast<View*>(panel); view && factory)
            factory->FreeSrvSlot(view->GetSrvSlotIndex());
        if (factory) factory->NotifyPanelRemoved(panel);
        it = panels.erase(it);
    }

    // Utility singletons remain owned so EditorState's stable pointers stay valid.
    if (m_activeLeftTab && !m_activeLeftTab->IsOpen()) m_activeLeftTab = nullptr;
    if (m_activeCenterTab && !m_activeCenterTab->IsOpen()) m_activeCenterTab = nullptr;
}
