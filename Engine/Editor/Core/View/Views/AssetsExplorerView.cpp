#include "AssetsExplorerView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include <filesystem>
#include <shellapi.h>
#include <commdlg.h>

#pragma comment(lib, "Comdlg32.lib")

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
    if (!ui.BeginWindow(m_title.c_str(), &m_open))
    {
        ui.EndWindow();
        return;
    }

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

    if (ui.Button("Import .obj"))
        ImportObj();
    ui.SameLine();
    if (ui.Button("Import glTF"))
        ImportGltf();

    ui.Selectable("Assets", m_selectedPath == m_assetsPath);
    if (ui.IsItemClicked())
        m_selectedPath = m_assetsPath;
    AcceptSceneObject(ui, m_assetsPath);

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
                const std::string entryPath = entry.path().string();
                uintptr_t idValue = std::hash<std::string>{}(entryPath);
                if (idValue == 0) idValue = 1;
                const void* entryId = reinterpret_cast<const void*>(idValue);
                const bool open = ui.TreeNode(entryId, dirName.c_str(),
                                              m_selectedPath == entryPath, false, true);
                if (ui.IsItemClicked())
                    m_selectedPath = entryPath;
                AcceptSceneObject(ui, entryPath);
                if (open)
                {
                    if (DrawDirectoryTree(ui, entryPath))
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
                const std::string entryPath = entry.path().string();
                if (ui.Selectable(fileName.c_str(), m_selectedPath == entryPath, true))
                {
                    m_selectedPath = entryPath;
                    if (ui.IsItemDoubleClicked())
                    {
                        OpenFile(entryPath);
                        return true;
                    }
                }
                if (ui.BeginDragDropSource())
                {
                    ui.SetDragDropPayload("ENGINE_ASSET_PATH",
                        entryPath.c_str(), entryPath.size() + 1);
                    ui.Label(fileName.c_str());
                    ui.EndDragDropSource();
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

std::string AssetsExplorerView::CurrentDirectory() const
{
    if (m_selectedPath.empty())
        return m_assetsPath;
    const fs::path selected(m_selectedPath);
    return fs::is_directory(selected) ? selected.string() : selected.parent_path().string();
}

bool AssetsExplorerView::AcceptSceneObject(IEditorUi& ui, const std::string& directory)
{
    if (!ui.BeginDragDropTarget())
        return false;

    size_t size = 0;
    const void* data = ui.AcceptDragDropPayload("ENGINE_SCENE_OBJECT", &size);
    bool created = false;
    if (data && size == sizeof(Object*))
    {
        Object* object = *static_cast<Object* const*>(data);
        Object* prefabRoot = object ? object->GetPrefabInstanceRoot() : nullptr;
        if (prefabRoot && prefabRoot->Prefab)
        {
            m_selectedPath = prefabRoot->Prefab->GetPath();
        }
        else if (object)
        {
            std::string base = object->name.empty() ? "Prefab" : object->name;
            for (char& c : base)
                if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
                    c == '\\' || c == '|' || c == '?' || c == '*')
                    c = '_';

            fs::path destination = fs::path(directory) / (base + ".prefab");
            for (unsigned suffix = 2; fs::exists(destination); ++suffix)
                destination = fs::path(directory) /
                    (base + " " + std::to_string(suffix) + ".prefab");

            if (SceneSerializer::SavePrefab(*object, destination.string()))
            {
                object->SetPrefab(destination.string());
                m_selectedPath = destination.string();
                created = true;
                if (OnPrefabCreated)
                    OnPrefabCreated(object, destination.string());
            }
        }
    }
    ui.EndDragDropTarget();
    return created;
}

void AssetsExplorerView::ImportObj()
{
    wchar_t source[MAX_PATH] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = source;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"Wavefront OBJ (*.obj)\0*.obj\0All files\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog))
        return;

    fs::path from(source);
    fs::path destination = fs::path(CurrentDirectory()) / from.filename();
    const std::string stem = destination.stem().string();
    const std::string extension = destination.extension().string();
    for (unsigned suffix = 2; fs::exists(destination); ++suffix)
        destination = destination.parent_path() /
            (stem + " " + std::to_string(suffix) + extension);

    std::error_code error;
    fs::copy_file(from, destination, fs::copy_options::none, error);
    if (!error)
    {
        m_selectedPath = destination.string();
        if (OnAssetImported)
            OnAssetImported(destination.string());
    }
}

void AssetsExplorerView::ImportGltf()
{
    wchar_t source[MAX_PATH] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = source;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter =
        L"glTF Models (*.gltf;*.glb)\0*.gltf;*.glb\0"
        L"glTF JSON (*.gltf)\0*.gltf\0"
        L"Binary glTF (*.glb)\0*.glb\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog) && OnGltfImportRequested)
        OnGltfImportRequested(fs::path(source).string());
}

// ---------------------------------------------------------------------------
// OpenFile
// ---------------------------------------------------------------------------
void AssetsExplorerView::OpenFile(const std::string& filePath)
{
    // Check if this is a scene file
    std::string extension = fs::path(filePath).extension().string();
    if (extension == ".scene" || extension == ".Scene" || extension == ".xml" || extension == ".XML")
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
