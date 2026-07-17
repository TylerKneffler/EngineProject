#pragma once
#include "pch.h"
#include "Core/ProjectLoader.h"
#include <atomic>
#include <future>
class IEditorUi;

// ---------------------------------------------------------------------------
// PreferencesView
//
// Defines a package-neutral window for editing project settings loaded from the
// .proj file. Users can modify settings and save them back to the project file.
//
// Usage:
//   preferencesView.Init(settings, projectFilePath);
//   if (preferencesView.IsOpen())
//       preferencesView.DrawWindow();
// ---------------------------------------------------------------------------
class PreferencesView
{
public:
    PreferencesView()  = default;
    ~PreferencesView() = default;

    // Initialize with current project settings and file path
    void Init(const ProjectSettings& settings, const std::string& projFilePath);

    // Draw the preferences window, returns true if window is still open
    void DrawWindow(IEditorUi& ui, bool& isOpen);

    // Check if preferences window should be shown
    bool IsOpen() const { return m_isOpen; }
    void SetOpen(bool open) { m_isOpen = open; }

    // Get the modified settings
    ProjectSettings GetSettings() const { return m_settings; }

    // Save settings back to the project file
    bool SaveSettings();

    // Callback fired when any setting is changed in the UI
    std::function<void()> OnSettingsChanged;

private:
    void DrawMetadataSection(IEditorUi& ui);
    void DrawPathsSection(IEditorUi& ui);
    void DrawRenderingSection(IEditorUi& ui);
    void DrawAspectRatioSection(IEditorUi& ui);
    void DrawExportSection(IEditorUi& ui);
    void StartPortableExport();
    void UpdatePortableExport();
    void NotifyChanged() { if (OnSettingsChanged) OnSettingsChanged(); }

    bool m_isOpen = false;
    std::string m_projFilePath;
    ProjectSettings m_settings;

    // Temporary buffers for string editing
    char m_projectNameBuf[256] = {};
    char m_assetsPathBuf[512] = {};
    char m_defaultSceneBuf[512] = {};
    std::string m_saveStatus;
    bool m_lastSaveSucceeded = false;
    bool m_exporting = false;
    bool m_exportSucceeded = false;
    std::string m_exportStatus;
    std::shared_ptr<std::atomic<float>> m_exportProgress;
    std::shared_ptr<std::atomic<int>> m_exportStage;
    std::future<std::pair<bool, std::string>> m_exportFuture;
};
