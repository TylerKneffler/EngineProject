#include "AssetsExplorerView.h"
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
void AssetsExplorerView::DrawPanel()
{
    ImGui::Begin(m_title.c_str(), &m_open);

    if (m_assetsPath.empty())
    {
        ImGui::TextDisabled("Assets path not initialized");
        ImGui::End();
        return;
    }

    if (!fs::exists(m_assetsPath))
    {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Assets path does not exist: %s", m_assetsPath.c_str());
        ImGui::End();
        return;
    }

    DrawDirectoryTree(m_assetsPath);

    ImGui::End();
}

// ---------------------------------------------------------------------------
// DrawDirectoryTree
// ---------------------------------------------------------------------------
bool AssetsExplorerView::DrawDirectoryTree(const std::string& path)
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
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen;

                if (ImGui::TreeNodeEx(dirName.c_str(), flags))
                {
                    if (DrawDirectoryTree(entry.path().string()))
                    {
                        ImGui::TreePop();
                        return true;
                    }
                    ImGui::TreePop();
                }
            }
            else
            {
                std::string fileName = entry.path().filename().string();
                ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick;
                
                if (ImGui::Selectable(fileName.c_str(), false, flags))
                {
                    // Check if this was a double-click
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
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
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error reading directory: %s", e.what());
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
