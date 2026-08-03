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

#include "Engine/Editor/UI/Nuklear/Menus/NuklearToolbar.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/Core/View/ViewFactory.h"

void NuklearToolbar::Draw(nk_context& context, float width,
    EditorState& state, PlayState playState, GameBuildManager* buildManager,
    const OpenPanelCommand& openPanel) const
{
    constexpr float toolbarHeight = 76.f;
    const struct nk_rect bounds = nk_rect(0.f, 0.f, width, toolbarHeight);
    nk_window_set_bounds(&context, "Editor Toolbar", bounds);
    if (nk_begin(&context, "Editor Toolbar", bounds,
        NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR))
    {
        const bool isBusy = playState == PlayState::Building ||
                            playState == PlayState::Playing ||
                            playState == PlayState::Paused;

        nk_menubar_begin(&context);
        nk_layout_row_begin(&context, NK_STATIC, 24.f, 2);
        nk_layout_row_push(&context, 55.f);
        if (nk_menu_begin_label(
            &context, "File", NK_TEXT_LEFT, nk_vec2(285.f, 255.f)))
        {
            nk_layout_row_dynamic(&context, 25.f, 1);
            if (nk_menu_item_label(&context, "Save All        Ctrl+S", NK_TEXT_LEFT))
                state.SaveScene();

            if (isBusy) nk_widget_disable_begin(&context);
            if (nk_menu_item_label(&context, "Build           Ctrl+B", NK_TEXT_LEFT) &&
                buildManager)
                buildManager->StartBuild(PostBuildAction::Nothing);
            if (nk_menu_item_label(
                &context, "Build and Run in Editor", NK_TEXT_LEFT) && buildManager)
                buildManager->StartBuild(PostBuildAction::PlayInEditor);
            if (nk_menu_item_label(
                &context, "Build and Run Standalone", NK_TEXT_LEFT) && buildManager)
                buildManager->StartBuild(PostBuildAction::LaunchStandalone);
            if (isBusy) nk_widget_disable_end(&context);

            if (nk_menu_item_label(&context, "Project Preferences", NK_TEXT_LEFT))
                state.SetShowPreferences(true);
            if (nk_menu_item_label(&context, "Exit", NK_TEXT_LEFT))
                PostQuitMessage(0);
            nk_menu_end(&context);
        }

        nk_layout_row_push(&context, 60.f);
        if (nk_menu_begin_label(
            &context, "Views", NK_TEXT_LEFT, nk_vec2(210.f, 190.f)))
        {
            nk_layout_row_dynamic(&context, 25.f, 1);
            ViewFactory* factory = state.GetViewFactory();
            const bool no3D = !factory || !factory->CanCreate3DView();
            if (no3D) nk_widget_disable_begin(&context);
            if (nk_menu_item_label(&context, "Scene", NK_TEXT_LEFT)) openPanel("Scene");
            if (nk_menu_item_label(&context, "Game", NK_TEXT_LEFT)) openPanel("Game");
            if (no3D) nk_widget_disable_end(&context);
            if (nk_menu_item_label(&context, "Hierarchy", NK_TEXT_LEFT)) openPanel("Hierarchy");
            if (nk_menu_item_label(&context, "Properties", NK_TEXT_LEFT)) openPanel("Properties");
            if (nk_menu_item_label(&context, "Assets", NK_TEXT_LEFT)) openPanel("Assets");
            if (nk_menu_item_label(&context, "Console", NK_TEXT_LEFT)) openPanel("Console");
            nk_menu_end(&context);
        }

        nk_layout_row_end(&context);
        nk_menubar_end(&context);

        nk_layout_row_dynamic(&context, 30.f, 5);
        if (nk_button_label(&context, "Save")) state.SaveScene();
        if (isBusy) nk_widget_disable_begin(&context);
        if (nk_button_label(&context, "Build") && buildManager)
            buildManager->StartBuild(PostBuildAction::Nothing);
        if (isBusy) nk_widget_disable_end(&context);

        if (playState == PlayState::Stopped || playState == PlayState::BuildFailed)
        {
            if (nk_button_label(&context, "Play") && buildManager)
                buildManager->PlayInEditor();
        }
        else if (nk_button_label(&context, "Stop") && buildManager)
            buildManager->Stop();

        if (playState == PlayState::Playing)
        {
            if (nk_button_label(&context, "Pause") && buildManager)
                buildManager->Pause();
        }
        else if (playState == PlayState::Paused)
        {
            if (nk_button_label(&context, "Resume") && buildManager)
                buildManager->Resume();
        }
        nk_label(&context, "UI: Nuklear", NK_TEXT_RIGHT);
    }
    nk_end(&context);
}
