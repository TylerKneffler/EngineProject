#pragma once
#include "pch.h"
#include "Core/ProjectLoader.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>
namespace Engine::Editor
{
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
    void Init(const Engine::Model::ProjectSettings& settings, const std::string& projFilePath);

    // Draw the preferences window, returns true if window is still open
    void DrawWindow(IEditorUi& ui, bool& isOpen);

    // Check if preferences window should be shown
    bool IsOpen() const { return m_isOpen; }
    void SetOpen(bool open) { m_isOpen = open; }

    // Get the modified settings
    Engine::Model::ProjectSettings GetSettings() const { return m_settings; }

    // Save settings back to the project file
    bool SaveSettings();

    // Callback fired when any setting is changed in the UI
    std::function<void()> OnSettingsChanged;

    // Spatial diagnostics are serialized by the active scene, but exposing
    // the switch here makes it readily available from Project Preferences.
    void SetSpatialDebugVisuals(bool enabled) { m_spatialDebugVisuals = enabled; }
    std::function<void(bool)> OnSpatialDebugVisualsChanged;

private:
    void DrawMetadataSection(IEditorUi& ui);
    void DrawPathsSection(IEditorUi& ui);
    void DrawRenderingSection(IEditorUi& ui);
    void DrawEditorSection(IEditorUi& ui);
    void DrawKeybindsSection(IEditorUi& ui);
    void DrawDiagnosticsSection(IEditorUi& ui);
    void DrawAspectRatioSection(IEditorUi& ui);
    void DrawExportSection(IEditorUi& ui);
    void StartPortableExport();
    void UpdatePortableExport();
    void StartVideoExport();
    void UpdateVideoExport();
    void NotifyChanged() { if (OnSettingsChanged) OnSettingsChanged(); }

    bool m_isOpen = false;
    bool m_spatialDebugVisuals = false;
    std::string m_projFilePath;
    Engine::Model::ProjectSettings m_settings;

    // Temporary buffers for string editing
    char m_projectNameBuf[256] = {};
    char m_assetsPathBuf[512] = {};
    char m_defaultSceneBuf[512] = {};
    std::string m_saveStatus;
    std::string m_keybindStatus;
    bool m_keybindStatusSucceeded = false;
    std::string m_themeStatus;
    bool m_themeStatusSucceeded = false;
    bool m_lastSaveSucceeded = false;
    bool m_exporting = false;
    bool m_exportSucceeded = false;
    std::string m_exportStatus;
    std::shared_ptr<std::atomic<float>> m_exportProgress;
    std::shared_ptr<std::atomic<int>> m_exportStage;
    std::future<std::pair<bool, std::string>> m_exportFuture;

    char m_videoSceneBuf[512] = {};
    char m_videoOutputBuf[512] = "VideoExports/cinematic.mp4";
    char m_videoCodecBuf[128] = {};
    char m_videoPresetBuf[64] = "medium";
    char m_videoFfmpegBuf[512] = "ffmpeg.exe";
    uint32_t m_videoWidth = 1920;
    uint32_t m_videoHeight = 1080;
    uint32_t m_videoFps = 60;
    uint32_t m_videoQuality = 75;
    float m_videoDuration = 0.f;
    float m_videoTimeout = 120.f;
    int m_videoFormat = 0;
    bool m_videoExporting = false;
    bool m_videoExportSucceeded = false;
    std::string m_videoExportStatus;
    std::future<std::pair<bool, std::string>> m_videoExportFuture;
};
}
