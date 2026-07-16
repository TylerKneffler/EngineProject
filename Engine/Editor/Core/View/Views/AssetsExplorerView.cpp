#include "AssetsExplorerView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <filesystem>
#include <shellapi.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void AssetsExplorerView::Init(const std::string& assetsPath)
{
    m_assetsPath = assetsPath;
}

// ---------------------------------------------------------------------------
// DrawPanel
// ---------------------------------------------------------------------------
void AssetsExplorerView::DrawPanel(IEditorUi& ui)
{
    ui.BeginWindow(m_title.c_str(), &m_open);

    if (m_assetsPath.empty())
    {
        ui.DisabledLabel("Assets path not initialized");
        ui.EndWindow();
        return;
    }

    if (!fs::exists(m_assetsPath))
    {
        std::string error = "Assets path does not exist: " + m_assetsPath;
        ui.ColoredLabel(error.c_str(), {1,0,0,1});
        ui.EndWindow();
        return;
    }

    DrawDirectoryTree(ui, m_assetsPath);

    ui.EndWindow();
}

// ---------------------------------------------------------------------------
// DrawDirectoryTree
// ---------------------------------------------------------------------------
bool AssetsExplorerView::DrawDirectoryTree(IEditorUi& ui, const std::string& path)
{
    try
    {
        std::vector<fs::directory_entry> entries;
        for (const auto& entry : fs::directory_iterator(path))
        {
            // Repository metadata such as .gitkeep is not project content.
            const std::string entryName = entry.path().filename().string();
            if (!entryName.empty() && entryName.front() == '.')
                continue;
            entries.push_back(entry);
        }

        // Sort alphabetically
        std::sort(entries.begin(), entries.end(),
                  [](const fs::directory_entry& a, const fs::directory_entry& b)
                  {
                      return a.path().filename().string() < b.path().filename().string();
                  });

        for (const auto& entry : entries)
        {
            if (entry.is_directory())
            {
                std::string dirName = entry.path().filename().string();
                if (ui.TreeNode(entry.path().c_str(), dirName.c_str(), false, false, true))
                {
                    if (DrawDirectoryTree(ui, entry.path().string()))
                    {
                        ui.TreePop();
                        return true;
                    }
                    ui.TreePop();
                }
            }
            else
            {
                std::string fileName = entry.path().filename().string();
                if (ui.Selectable(fileName.c_str(), false, true))
                {
                    if (ui.IsItemDoubleClicked())
                    {
                        OpenFile(entry.path().string());
                        return true;
                    }
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        std::string error = "Error reading directory: " + std::string(e.what());
        ui.ColoredLabel(error.c_str(), {1,0,0,1});
    }

    return false;
}

// ---------------------------------------------------------------------------
// OpenFile
// ---------------------------------------------------------------------------
void AssetsExplorerView::OpenFile(const std::string& filePath)
{
    // Check if this is a scene file
    std::string extension = fs::path(filePath).extension().string();
    if (extension == ".scene" || extension == ".Scene")
    {
        // Trigger the scene load callback
        if (OnSceneRequested)
        {
            OnSceneRequested(filePath);
        }
        return;
    }

    // Convert to wide string for Windows API
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), (int)filePath.length(), NULL, 0);
    std::wstring wideFilePath(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), (int)filePath.length(), &wideFilePath[0], size_needed);

    // Use ShellExecute to open the file with the default application
    ShellExecuteW(NULL, L"open", wideFilePath.c_str(), NULL, NULL, SW_SHOW);
}
