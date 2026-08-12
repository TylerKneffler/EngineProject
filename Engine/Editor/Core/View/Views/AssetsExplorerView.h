#pragma once
#include "pch.h"
#include <functional>
#include "View/IEditorPanel.h"

class Object;

// ---------------------------------------------------------------------------
// AssetsExplorerView
//
// Displays a hierarchical file tree of the Assets directory. Scene and prefab
// assets open inside the editor; other files use the system application.
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
    std::function<void(const std::string&)> OnPrefabRequested;
    std::function<void(const std::string&)> OnSelectionChanged;
    std::function<void(Object*, const std::string&)> OnPrefabCreated;

    void SetSelectedPath(const std::string& path) { m_selectedPath = path; }
    const std::string& GetSelectedPath() const { return m_selectedPath; }

private:
    // Recursively draws the directory tree starting from the given path
    // Returns true if any item in the tree was double-clicked
    bool DrawDirectoryTree(IEditorUi& ui, const std::string& path);

    // Opens a file with the system's default application (unless it's a scene file)
    void OpenFile(const std::string& filePath);
    bool AcceptSceneObject(IEditorUi& ui, const std::string& directory);
    void SelectPath(const std::string& path);

    std::string m_assetsPath;
    std::string m_selectedPath;
};
