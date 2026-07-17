#pragma once
#include "pch.h"
#include <functional>
#include "View/IEditorPanel.h"

// ---------------------------------------------------------------------------
// AssetsExplorerView
//
// Displays a hierarchical file tree of the Assets directory, allowing users
// to navigate files and folders. Double-clicking a file opens it with the
// system's default application. Scene files can be loaded via callback.
//
// Usage:
//   assetsExplorerView.Init(assetsPath);
//   assetsExplorerView.OnSceneRequested = [](const std::string& path) { /* load scene */ };
//   assetsExplorerView.DrawPanel();
// ---------------------------------------------------------------------------
class AssetsExplorerView : public IEditorPanel
{
public:
    AssetsExplorerView()  = default;
    ~AssetsExplorerView() = default;

    // Initializes the view with the path to the Assets directory
    void Init(const std::string& assetsPath);

    // Defines the package-neutral asset file tree.
    void DrawPanel(IEditorUi& ui) override;

    // Callback when a scene file is requested to load
    std::function<void(const std::string&)> OnSceneRequested;

private:
    // Recursively draws the directory tree starting from the given path
    // Returns true if any item in the tree was double-clicked
    bool DrawDirectoryTree(IEditorUi& ui, const std::string& path);

    // Opens a file with the system's default application (unless it's a scene file)
    void OpenFile(const std::string& filePath);

    std::string m_assetsPath;
};
