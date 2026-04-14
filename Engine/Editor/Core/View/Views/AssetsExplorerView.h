#pragma once
#include "pch.h"

// ---------------------------------------------------------------------------
// AssetsExplorerView
//
// Displays a hierarchical file tree of the Assets directory, allowing users
// to navigate files and folders. Double-clicking a file opens it with the
// system's default application.
//
// Usage:
//   assetsExplorerView.Init(assetsPath);
//   assetsExplorerView.DrawPanel();
// ---------------------------------------------------------------------------
class AssetsExplorerView
{
public:
    AssetsExplorerView()  = default;
    ~AssetsExplorerView() = default;

    // Initializes the view with the path to the Assets directory
    void Init(const std::string& assetsPath);

    // Draws the ImGui panel showing the file tree
    void DrawPanel();

private:
    // Recursively draws the directory tree starting from the given path
    // Returns true if any item in the tree was double-clicked
    bool DrawDirectoryTree(const std::string& path);

    // Opens a file with the system's default application
    void OpenFile(const std::string& filePath);

    std::string m_assetsPath;
};
