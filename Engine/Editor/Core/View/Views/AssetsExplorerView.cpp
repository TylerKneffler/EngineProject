#include "AssetsExplorerView.h"
#include "Engine/Editor/CodeEditorLink.h"
#include "Engine/Editor/Core/View/Templates/Common/AssetPathTemplate.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Engine/Editor/UI/EditorIcons.h"
#include "Engine/Editor/Core/Paint/BrushPreset.h"
#include "Core/AssetRecord.h"
#include "Core/Model/MeshPackAsset.h"
#include "Engine/Editor/Core/Importers/ModelImporter.h"
#include "Engine/Editor/Core/Importers/AssetDataExtractor.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Scene/Scene.h"
#include "../Focus/WindowFocusHandler.h"
#include <filesystem>
#include <fstream>
#include <shellapi.h>
#include <unordered_map>
#include <pugixml.hpp>

namespace Engine::Editor
{
// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
AssetsExplorerView::AssetsExplorerView()
{
    // Assets explorer is an editor UI panel with normal cursor
    SetCursorBehaviorOnFocus(CursorBehaviorOnFocus::Visible);
}

namespace fs = std::filesystem;

namespace
{
constexpr float AssetThumbnailSize = 96.f;

bool IsCompoundModelSource(const fs::path& path)
{
    return AssetDataExtractor::Supports(path);
}

const std::unordered_map<std::string, std::string>& AssetEditorRegistry()
{
    // The key is the lowercased extension. Editor identifiers stay independent
    // from the visible filename and are reusable for every document of a type.
    static const std::unordered_map<std::string, std::string> registry = {
        { ".material", "Material Editor" }, { ".mat", "Material Editor" },
        { ".scene", "Scene Editor" },
        { ".prefab", "Object Editor" },
        { ".png", "Texture Editor" }, { ".jpg", "Texture Editor" },
        { ".jpeg", "Texture Editor" }, { ".bmp", "Texture Editor" },
        { ".tga", "Texture Editor" }, { ".dds", "Texture Editor" },
        { ".ktx2", "Texture Editor" }, { ".hdr", "Texture Editor" },
        { ".exr", "Texture Editor" }, { ".tif", "Texture Editor" },
        { ".tiff", "Texture Editor" }, { ".gif", "Texture Editor" },
        { ".wav", "Audio Editor" }, { ".ogg", "Audio Editor" },
        { ".mp3", "Audio Editor" }, { ".flac", "Audio Editor" },
        { ".aac", "Audio Editor" }, { ".m4a", "Audio Editor" },
        { ".ttf", "Font Editor" }, { ".otf", "Font Editor" },
        { ".spriteanim", "Sprite Animation Editor" },
        { ".spritesheet", "Sprite Sheet Editor" },
        { ".anim", "Animation Editor" }, { ".animation", "Animation Editor" },
        { ".skeleton", "Skeleton Editor" },
        { ".mesh", "Mesh Editor" }, { ".obj", "Mesh Editor" },
        { ".ply", "Mesh Editor" }, { ".stl", "Mesh Editor" },
        { ".fbx", "Model Editor" }, { ".pmx", "Model Editor" },
        { ".gltf", "Model Editor" },
        { ".glb", "Model Editor" }
    };
    return registry;
}

std::string AssetTypeBadge(const fs::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
    if (!extension.empty() && extension.front() == '.')
        extension.erase(extension.begin());
    if (extension == "SCENE") return "SCN";
    if (extension == "PREFAB") return "PFB";
    if (extension == "MATERIAL") return "MAT";
    if (extension == "SPRITEANIM") return "ANIM";
    if (extension == "ANIMPACK") return "PACK";
    if (extension == "SPRITESHEET") return "SPR";
    if (extension == "CPP" || extension == "CC" || extension == "CXX")
        return "C++";
    if (extension.empty()) return Icons::Document;
    return extension.size() <= 5 ? extension : extension.substr(0, 5);
}

class UiIdScope
{
public:
    UiIdScope(IEditorUi& ui, const char* id) : m_ui(ui) { m_ui.PushId(id); }
    ~UiIdScope() { m_ui.PopId(); }
    UiIdScope(const UiIdScope&) = delete;
    UiIdScope& operator=(const UiIdScope&) = delete;
private:
    IEditorUi& m_ui;
};

class UiTreeScope
{
public:
    explicit UiTreeScope(IEditorUi& ui) : m_ui(ui) {}
    ~UiTreeScope() { m_ui.TreePop(); }
    UiTreeScope(const UiTreeScope&) = delete;
    UiTreeScope& operator=(const UiTreeScope&) = delete;
private:
    IEditorUi& m_ui;
};
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void AssetsExplorerView::Init(const std::string& assetsPath,
    Engine::Scene::Scene* scene)
{
    m_assetsPath = fs::path(assetsPath).lexically_normal().string();
    m_currentDirectory = m_assetsPath;
    m_backHistory.clear();
    m_selectedPath.clear();
    m_selectedPaths.clear();
    m_selectionAnchor.clear();
    m_visiblePaths.clear();
    m_previousVisiblePaths.clear();
    m_pendingDeletePaths.clear();
    m_clipboardPaths.clear();
    m_clipboardCut = false;
    m_pendingPasteDirectory.clear();
    m_pendingAssetDropSources.clear();
    m_pendingAssetDropDirectory.clear();
    m_scene = scene;
    m_previewCache.Clear();
    m_locationIndex.Open(m_assetsPath);
    m_externalMoves.clear();
    m_locationCheckPending = false;
    CheckForExternalMoves();
}

void AssetsExplorerView::ExtractModelAssets(const std::string& sourcePath)
{
    const fs::path source(sourcePath);
    if (!fs::is_regular_file(source) || !IsCompoundModelSource(source))
    {
        m_error = "Choose a model, rig, animation pack, or mesh pack.";
        return;
    }
    const auto imported = AssetDataExtractor::Extract(source, source.parent_path());
    if (!imported.success)
    {
        m_error = imported.message;
        return;
    }
    m_error.clear();
    if (OnAssetContentsChanged)
        OnAssetContentsChanged(imported.outputDirectory);
}

void AssetsExplorerView::CheckForExternalMoves()
{
    if (m_assetsPath.empty()) return;
    if (!m_externalMoves.empty())
    {
        m_locationCheckPending = true;
        return;
    }
    m_locationCheckPending = false;
    m_externalMoves = m_locationIndex.Detect();
}

// ---------------------------------------------------------------------------
// DrawPanel
// ---------------------------------------------------------------------------
void AssetsExplorerView::DrawPanel(IEditorUi& ui)
{
    if (m_locationCheckPending && m_externalMoves.empty())
        CheckForExternalMoves();
    if (ui.BeginModal("Asset moves detected", !m_externalMoves.empty()))
    {
        ui.Label("These assets moved outside the editor. Update references?");
        ui.Separator();
        if (ui.BeginChildRegion("##detectedAssetMoves", 260.f))
        {
            for (const auto& move : m_externalMoves)
            {
                const std::string label =
                    std::string(move.directory ? "Folder: " : "File: ") +
                    move.oldPath + "  ->  " + move.newPath;
                ui.Label(label.c_str());
            }
        }
        ui.EndChild();
        ui.Separator();
        if (ui.Button("Update references"))
        {
            for (const auto& move : m_externalMoves)
            {
                const fs::path oldPath = fs::path(m_assetsPath) / move.oldPath;
                const fs::path newPath = fs::path(m_assetsPath) / move.newPath;
                if (!move.directory &&
                    !fs::exists(Engine::Core::AssetRecord::SidecarPath(newPath)))
                {
                    std::error_code metadataError;
                    Engine::Core::AssetRecord::Move(oldPath, newPath,
                        metadataError);
                }
                if (OnAssetRenamed)
                    OnAssetRenamed(oldPath.string(), newPath.string());
            }
            m_locationIndex.Accept();
            m_externalMoves.clear();
            m_locationCheckPending = true;
            ui.CloseModal();
        }
        ui.SameLine();
        if (ui.Button("Ignore changes"))
        {
            m_locationIndex.Accept();
            m_externalMoves.clear();
            m_locationCheckPending = true;
            ui.CloseModal();
        }
        ui.EndModal();
    }
    const bool visible = ui.BeginWindow(m_title.c_str(), &m_open);
    m_hasScreenBounds = m_open && ui.CurrentWindowScreenRect(
        m_screenLeft, m_screenTop, m_screenRight, m_screenBottom);
    if (!visible)
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

    if (m_currentDirectory.empty() || !fs::is_directory(m_currentDirectory))
        m_currentDirectory = m_assetsPath;

    const bool canGoBack = !m_backHistory.empty();
    ui.BeginDisabled(!canGoBack);
    if (ui.Button(Icons::Back) && canGoBack)
        GoBack();
    ui.EndDisabled();
    ui.Tooltip("Back");
    ui.SameLine();
    DrawBreadcrumbs(ui);
    AcceptSceneObject(ui, m_currentDirectory);

    ui.SameLineRight(51.f);
    if (ui.Button(Icons::List))
        m_gridView = false;
    ui.Tooltip("List view");
    ui.SameLine();
    if (ui.Button(Icons::Grid))
        m_gridView = true;
    ui.Tooltip("Thumbnail view");
    ui.SearchInput("##assetSearch", m_search, sizeof(m_search),
        "Search assets...");
    ui.SameLine();
    if (ui.Button(Icons::Refresh))
    {
        m_previewCache.Clear();
        CheckForExternalMoves();
    }
    ui.Tooltip("Refresh Assets and check for moved files and folders");
    ui.SameLine();
    if (ui.Button("New brush")) CreateBrush();
    std::error_code selectedPathError;
    const bool selectedDirectory = !m_selectedPath.empty() &&
        fs::is_directory(fs::path(m_selectedPath), selectedPathError);
    if (m_selectedPaths.size() == 1 && selectedDirectory &&
        fs::path(m_selectedPath) != fs::path(m_assetsPath))
    {
        ui.SameLine();
        if (ui.Button("Rename folder"))
        {
            m_renamePath = m_selectedPath;
            strncpy_s(m_renameName,
                fs::path(m_selectedPath).filename().string().c_str(),
                sizeof(m_renameName));
            m_renamingAsset = true;
            m_focusAssetRename = true;
            m_error.clear();
        }
    }
    if (!m_selectedPaths.empty())
    {
        ui.SameLine();
        if (ui.Button(m_selectedPaths.size() > 1 ? "Delete selected" :
            (selectedDirectory ? "Delete folder" : "Delete asset")))
            QueueDeletePath(m_selectedPath);
    }
    ui.Separator();

    if (!m_error.empty())
        ui.ColoredLabel(m_error.c_str(), { 1.f, 0.3f, 0.3f, 1.f });
    if (m_renamingScript)
    {
        const EditorUiTextEditResult rename = ui.RenameText(
            "##scriptPairName", m_scriptName, sizeof(m_scriptName),
            m_focusScriptName);
        m_focusScriptName = false;
        ui.SameLine();
        ui.DisabledLabel(".h + .cpp");
        if (rename.submitted || rename.deactivated)
            CommitScriptRename();
    }
    if (m_renamingAsset)
    {
        ui.Label("New name");
        const EditorUiTextEditResult rename = ui.RenameText(
            "##assetRename", m_renameName, sizeof(m_renameName),
            m_focusAssetRename);
        m_focusAssetRename = false;
        if (rename.submitted || rename.deactivated)
            CommitAssetRename();
    }
    if (ui.CopyShortcutPressed() && !m_selectedPath.empty())
        CopySelectionToClipboard(m_selectedPath, false);
    if (ui.CutShortcutPressed() && !m_selectedPath.empty())
        CopySelectionToClipboard(m_selectedPath, true);
    if (ui.PasteShortcutPressed())
        PasteClipboard(m_currentDirectory);
    if (ui.DeleteShortcutPressed() && !m_selectedPath.empty())
        QueueDeletePath(m_selectedPath);
    m_previousVisiblePaths = std::move(m_visiblePaths);
    m_visiblePaths.clear();
    DrawCurrentDirectory(ui);
    DeleteQueuedPaths();
    ProcessPendingAssetDrop();
    if (!m_pendingPasteDirectory.empty())
    {
        PasteClipboard(m_pendingPasteDirectory);
        m_pendingPasteDirectory.clear();
    }
    const EditorUiAssetCreateMenuResult create =
        ui.AssetWindowContextMenu(!m_clipboardPaths.empty());
    if (create.folderRequested)
        CreateFolder();
    if (create.scriptRequested)
        CreateScript();
    if (create.pasteRequested)
        PasteClipboard(m_currentDirectory);

    ui.EndWindow();
}

void AssetsExplorerView::DrawCurrentDirectory(IEditorUi& ui)
{
    if (m_gridView)
    {
        DrawGridDirectory(ui);
        return;
    }

    try
    {
        std::string query = m_search;
        std::transform(query.begin(), query.end(), query.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        std::function<void(const fs::path&)> drawDirectory;
        drawDirectory = [&](const fs::path& directoryPath)
        {
            std::vector<fs::directory_entry> entries;
            for (const auto& entry : fs::directory_iterator(directoryPath))
            {
                // Repository metadata such as .gitkeep is not project content.
                const std::string entryName = entry.path().filename().string();
                if ((!entryName.empty() && entryName.front() == '.') ||
                    entry.path().extension() == ".meta")
                    continue;
                std::string searchable = entryName;
                std::transform(searchable.begin(), searchable.end(),
                    searchable.begin(), [](unsigned char c)
                    {
                        return static_cast<char>(std::tolower(c));
                    });
                if (!query.empty() && searchable.find(query) == std::string::npos)
                    continue;
                entries.push_back(entry);
            }

            // Directories first, then alphabetical within each kind.
            std::sort(entries.begin(), entries.end(),
                [](const fs::directory_entry& a, const fs::directory_entry& b)
                {
                    if (a.is_directory() != b.is_directory())
                        return a.is_directory();
                    return a.path().filename().string() <
                        b.path().filename().string();
                });

            for (const auto& entry : entries)
            {
                const std::string entryPath = entry.path().string();
                m_visiblePaths.push_back(entryPath);
                const bool selected = std::find(m_selectedPaths.begin(),
                    m_selectedPaths.end(), entryPath) != m_selectedPaths.end();
                const bool directory = entry.is_directory();
                const bool expandable = directory && !entry.is_symlink();
                const std::string label = directory
                    ? std::string(Icons::Folder) + " " +
                        entry.path().filename().string()
                    : std::string(Icons::Document) + " " +
                        entry.path().filename().string();

                UiIdScope idScope(ui, entryPath.c_str());

                bool expanded = false;
                bool activated = false;
                if (directory)
                {
                    // The path PushId makes the constant node ID stable and unique.
                    expanded = ui.TreeNode(reinterpret_cast<const void*>(1),
                        label.c_str(), selected, !expandable);
                    activated = ui.IsItemClicked();
                }
                else
                {
                    activated = ui.Selectable(label.c_str(), selected, true);
                }

                const bool doubleClicked = ui.IsItemDoubleClicked();
                if (activated)
                    SelectPath(entryPath, ui.IsMultiSelectModifierDown(),
                        ui.IsRangeSelectModifierDown());
                if (doubleClicked)
                {
                    if (directory)
                        EnterDirectory(entryPath);
                    else
                        OpenFile(entryPath);
                }

                const EditorUiAssetItemMenuResult menu =
                    ui.AssetItemContextMenu(entryPath.c_str(),
                        entry.path().extension() == ".rig" &&
                            !fs::exists(entry.path().parent_path() /
                                (entry.path().stem().string() + ".meshpack")),
                        entry.path().extension() == ".rig" &&
                            fs::exists(entry.path().parent_path() /
                                (entry.path().stem().string() + ".meshpack")),
                        directory, !m_clipboardPaths.empty(),
                        !directory && IsCompoundModelSource(entry.path()));
                if (menu.copyRequested)
                    CopySelectionToClipboard(entryPath, false);
                if (menu.cutRequested)
                    CopySelectionToClipboard(entryPath, true);
                if (menu.pasteRequested)
                    m_pendingPasteDirectory = entryPath;
                if (menu.packMeshesRequested || menu.extractMeshesRequested)
                {
                    std::string error;
                    const bool ok = menu.packMeshesRequested
                        ? Engine::Model::MeshPackAsset::PackRig(entry.path(), error)
                        : Engine::Model::MeshPackAsset::ExtractRig(entry.path(), error);
                    m_error = ok ? std::string() : error;
                    if (ok && OnAssetContentsChanged) OnAssetContentsChanged(entryPath);
                }
                if (menu.extractModelAssetsRequested)
                    ExtractModelAssets(entryPath);
                if (menu.renameRequested)
                {
                    m_renamePath = entryPath;
                    strncpy_s(m_renameName,
                        entry.path().filename().string().c_str(),
                        sizeof(m_renameName));
                    m_renamingAsset = true;
                    m_focusAssetRename = true;
                    m_error.clear();
                }
                bool deleted = false;
                if (menu.deleteRequested)
                {
                    QueueDeletePath(entryPath);
                    deleted = true;
                }

                if (!deleted && ui.BeginDragDropSource())
                {
                    ui.SetDragDropPayload("ENGINE_ASSET_PATH",
                        entryPath.c_str(), entryPath.size() + 1);
                    ui.Label(label.c_str());
                    ui.EndDragDropSource();
                }

                if (!deleted && directory)
                    AcceptSceneObject(ui, entryPath);

                if (expandable && expanded)
                {
                    UiTreeScope treeScope(ui);
                    if (!deleted && !doubleClicked)
                        drawDirectory(entry.path());
                }
            }
        };

        drawDirectory(m_currentDirectory);
    }
    catch (const std::exception& e)
    {
        m_error = "Error reading directory: " + std::string(e.what());
    }
}

void AssetsExplorerView::EnterDirectory(const std::string& path)
{
    NavigateToDirectory(path, true);
}

void AssetsExplorerView::DrawGridDirectory(IEditorUi& ui)
{
    try
    {
        std::string query = m_search;
        std::transform(query.begin(), query.end(), query.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        std::vector<fs::directory_entry> entries;
        for (const auto& entry : fs::directory_iterator(m_currentDirectory))
        {
            const std::string name = entry.path().filename().string();
            if ((!name.empty() && name.front() == '.') ||
                entry.path().extension() == ".meta")
                continue;
            std::string searchable = name;
            std::transform(searchable.begin(), searchable.end(),
                searchable.begin(), [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
                });
            if (query.empty() || searchable.find(query) != std::string::npos)
                entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end(),
            [](const fs::directory_entry& first,
                const fs::directory_entry& second)
            {
                if (first.is_directory() != second.is_directory())
                    return first.is_directory();
                return first.path().filename().string() <
                    second.path().filename().string();
            });

        const float tileSize = AssetThumbnailSize;
        const int columns = std::max(1, static_cast<int>(
            ui.AvailableContentWidth() / (tileSize + 18.f)));
        int column = 0;
        for (const auto& entry : entries)
        {
            const std::string entryPath = entry.path().string();
            m_visiblePaths.push_back(entryPath);
            const bool directory = entry.is_directory();
            void* preview = nullptr;
            if (!directory && m_scene && AssetPreviewCache::Supports(entryPath))
                preview = m_previewCache.Get(entryPath,
                    m_scene->GetGraphicsProvider());
            const std::string fallback = directory
                ? std::string(Icons::Folder) : AssetTypeBadge(entry.path());

            UiIdScope idScope(ui, entryPath.c_str());
            const EditorUiAssetTileResult tile = ui.AssetTile(
                entry.path().filename().string().c_str(),
                fallback.c_str(), preview,
                std::find(m_selectedPaths.begin(), m_selectedPaths.end(),
                    entryPath) != m_selectedPaths.end(), tileSize);
            if (tile.clicked)
                SelectPath(entryPath, ui.IsMultiSelectModifierDown(),
                    ui.IsRangeSelectModifierDown());
            if (tile.doubleClicked)
            {
                if (directory)
                    EnterDirectory(entryPath);
                else
                    OpenFile(entryPath);
            }

            const EditorUiAssetItemMenuResult menu =
                ui.AssetItemContextMenu(entryPath.c_str(),
                    entry.path().extension() == ".rig" &&
                        !fs::exists(entry.path().parent_path() /
                            (entry.path().stem().string() + ".meshpack")),
                    entry.path().extension() == ".rig" &&
                        fs::exists(entry.path().parent_path() /
                            (entry.path().stem().string() + ".meshpack")),
                    directory, !m_clipboardPaths.empty(),
                    !directory && IsCompoundModelSource(entry.path()));
            if (menu.copyRequested)
                CopySelectionToClipboard(entryPath, false);
            if (menu.cutRequested)
                CopySelectionToClipboard(entryPath, true);
            if (menu.pasteRequested)
                m_pendingPasteDirectory = entryPath;
            if (menu.packMeshesRequested || menu.extractMeshesRequested)
            {
                std::string error;
                const bool ok = menu.packMeshesRequested
                    ? Engine::Model::MeshPackAsset::PackRig(entry.path(), error)
                    : Engine::Model::MeshPackAsset::ExtractRig(entry.path(), error);
                m_error = ok ? std::string() : error;
                if (ok && OnAssetContentsChanged) OnAssetContentsChanged(entryPath);
            }
            if (menu.extractModelAssetsRequested)
                ExtractModelAssets(entryPath);
            if (menu.renameRequested)
            {
                m_renamePath = entryPath;
                strncpy_s(m_renameName,
                    entry.path().filename().string().c_str(),
                    sizeof(m_renameName));
                m_renamingAsset = true;
                m_focusAssetRename = true;
                m_error.clear();
            }
            bool deleted = false;
            if (menu.deleteRequested)
            {
                QueueDeletePath(entryPath);
                deleted = true;
            }

            if (!deleted && ui.BeginDragDropSource())
            {
                ui.SetDragDropPayload("ENGINE_ASSET_PATH",
                    entryPath.c_str(), entryPath.size() + 1);
                ui.Label(entry.path().filename().string().c_str());
                ui.EndDragDropSource();
            }
            if (!deleted && directory)
                AcceptSceneObject(ui, entryPath);

            ++column;
            if (column < columns)
                ui.SameLine();
            else
                column = 0;
        }
    }
    catch (const std::exception& exception)
    {
        m_error = "Error reading directory: " + std::string(exception.what());
    }
}

void AssetsExplorerView::NavigateToDirectory(const std::string& path,
    bool addToHistory)
{
    std::error_code error;
    const fs::path root = fs::weakly_canonical(m_assetsPath, error);
    const fs::path destination = fs::weakly_canonical(path, error);
    if (error || !fs::is_directory(destination))
        return;
    const fs::path relative = destination.lexically_relative(root);
    if (relative.empty() || (!relative.empty() && *relative.begin() == ".."))
        return;
    const fs::path current = fs::weakly_canonical(m_currentDirectory, error);
    if (error || current == destination)
        return;
    if (addToHistory && fs::is_directory(current))
        m_backHistory.push_back(current.string());
    m_currentDirectory = destination.string();
    SetSelectedPath({});
    m_error.clear();
    if (OnSelectionChanged)
        OnSelectionChanged({});
}

void AssetsExplorerView::GoBack()
{
    while (!m_backHistory.empty())
    {
        const std::string destination = m_backHistory.back();
        m_backHistory.pop_back();
        const std::string before = m_currentDirectory;
        NavigateToDirectory(destination, false);
        if (m_currentDirectory != before)
            return;
    }
}

void AssetsExplorerView::DrawBreadcrumbs(IEditorUi& ui)
{
    std::error_code error;
    const fs::path root = fs::weakly_canonical(m_assetsPath, error);
    const fs::path current = fs::weakly_canonical(m_currentDirectory, error);
    if (error)
    {
        ui.ColoredLabel("/Assets", { 0.35f, 0.7f, 1.f, 1.f });
        return;
    }

    const auto drawSegment = [&](const char* label, const fs::path& folder)
    {
        ui.PushId(folder.string().c_str());
        const EditorUiBreadcrumbResult result = ui.Breadcrumb(label, nullptr, 0);
        ui.PopId();
        if (result.clicked)
            EnterDirectory(folder.string());
    };

    fs::path destination = root;
    drawSegment("Assets", destination);

    const fs::path relative = current.lexically_relative(root);
    if (relative.empty() || relative == ".")
        return;
    for (const fs::path& segment : relative)
    {
        destination /= segment;
        ui.SameLine();
        ui.DisabledLabel("/");
        ui.SameLine();
        const std::string destinationString = destination.string();
        drawSegment(segment.string().c_str(), destinationString);
    }
}

void AssetsExplorerView::CreateFolder()
{
    fs::path destination = fs::path(m_currentDirectory) / "New Folder";
    for (unsigned suffix = 2; fs::exists(destination); ++suffix)
        destination = fs::path(m_currentDirectory) /
            ("New Folder " + std::to_string(suffix));
    std::error_code error;
    if (!fs::create_directory(destination, error) || error)
    {
        m_error = "Could not create folder: " + error.message();
        return;
    }
    m_error.clear();
    SelectPath(destination.string());
}

namespace
{
std::string ScriptClassName(const std::string& fileName)
{
    std::string result;
    bool capitalize = true;
    for (const unsigned char character : fileName)
    {
        if (!std::isalnum(character) && character != '_')
        {
            capitalize = true;
            continue;
        }
        char value = static_cast<char>(character);
        if (capitalize && std::isalpha(character))
            value = static_cast<char>(std::toupper(character));
        result.push_back(value);
        capitalize = false;
    }
    if (result.empty()) result = "NewScript";
    if (std::isdigit(static_cast<unsigned char>(result.front())))
        result.insert(0, "Script");
    return result;
}

std::string CleanScriptFileName(std::string name)
{
    const std::string invalid = "<>:\"/\\|?*";
    for (char& character : name)
        if (invalid.find(character) != std::string::npos)
            character = '_';
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    const size_t first = name.find_first_not_of(' ');
    if (first != std::string::npos) name.erase(0, first);
    else name.clear();
    if (name.size() > 4 && name.substr(name.size() - 4) == ".cpp")
        name.resize(name.size() - 4);
    else if (name.size() > 2 && name.substr(name.size() - 2) == ".h")
        name.resize(name.size() - 2);
    return name.empty() ? "New Script" : name;
}

std::string CleanAssetFileName(std::string name)
{
    const std::string invalid = "<>:\"/\\|?*";
    for (char& character : name)
        if (invalid.find(character) != std::string::npos)
            character = '_';
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    const size_t first = name.find_first_not_of(' ');
    if (first != std::string::npos)
        name.erase(0, first);
    else
        name.clear();
    return name;
}

bool IsPathWithin(const fs::path& root, const fs::path& candidate)
{
    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty())
        return false;
    return *relative.begin() != "..";
}

void ReplaceAll(std::string& text, const std::string& from,
    const std::string& to)
{
    if (from.empty()) return;
    size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos)
    {
        text.replace(position, from.size(), to);
        position += to.size();
    }
}
}

void AssetsExplorerView::CreateBrush()
{
    fs::path path = fs::path(m_currentDirectory) / "New Brush.brush";
    for (unsigned suffix = 2; fs::exists(path); ++suffix)
        path = fs::path(m_currentDirectory) /
            ("New Brush " + std::to_string(suffix) + ".brush");
    Paint::BrushPreset preset;
    preset.name = path.stem().string();
    if (!Paint::SaveBrushPreset(path, preset, m_error)) return;
    SelectPath(path.string());
    if (OnAssetContentsChanged) OnAssetContentsChanged(path.string());
}

void AssetsExplorerView::CreateScript()
{
    std::string base = "New Script";
    fs::path basePath = fs::path(m_currentDirectory) / base;
    for (unsigned suffix = 2;
        fs::exists(basePath.string() + ".h") ||
        fs::exists(basePath.string() + ".cpp"); ++suffix)
    {
        base = "New Script " + std::to_string(suffix);
        basePath = fs::path(m_currentDirectory) / base;
    }

    const std::string className = ScriptClassName(base);
    std::ofstream header(basePath.string() + ".h");
    std::ofstream source(basePath.string() + ".cpp");
    if (!header || !source)
    {
        m_error = "Could not create script files.";
        return;
    }
    header << "#pragma once\n"
        "#include \"Core/Script.h\"\n\n"
        "class " << className << " : public Engine::Core::Script\n"
        "{\npublic:\n    " << className << "();\n\n"
        "    void Start() override;\n"
        "    void Update() override;\n};\n";
    source << "#include \"" << base << ".h\"\n"
        "#include \"Core/Serialization/SceneSerializer.h\"\n\n"
        << className << "::" << className << "()\n{\n"
        "    SetTypeName(COMPONENT_TYPE_NAME(" << className << "));\n}\n\n"
        "namespace\n{\nstruct " << className << "Registration\n{\n"
        "    " << className << "Registration()\n    {\n"
        "        Engine::Serialization::RegisterComponentType<" << className
        << ">(\"" << className << "\");\n"
        "    }\n};\n\n" << className << "Registration g_" << className
        << "Registration;\n}\n\n"
        "void " << className << "::Start()\n{\n}\n\n"
        "void " << className << "::Update()\n{\n}\n";
    header.close();
    source.close();

    m_scriptBasePath = basePath.string();
    strncpy_s(m_scriptName, base.c_str(), sizeof(m_scriptName));
    m_renamingScript = true;
    m_focusScriptName = true;
    m_error.clear();
}

void AssetsExplorerView::CommitScriptRename()
{
    const std::string requested = CleanScriptFileName(m_scriptName);
    const fs::path oldBase(m_scriptBasePath);
    std::string finalName = requested;
    fs::path newBase = oldBase.parent_path() / finalName;
    for (unsigned suffix = 2; newBase != oldBase &&
        (fs::exists(newBase.string() + ".h") ||
         fs::exists(newBase.string() + ".cpp")); ++suffix)
    {
        finalName = requested + " " + std::to_string(suffix);
        newBase = oldBase.parent_path() / finalName;
    }

    if (newBase != oldBase)
    {
        std::error_code error;
        fs::rename(oldBase.string() + ".h", newBase.string() + ".h", error);
        if (!error)
            fs::rename(oldBase.string() + ".cpp", newBase.string() + ".cpp", error);
        if (error)
        {
            std::error_code rollback;
            if (fs::exists(newBase.string() + ".h"))
                fs::rename(newBase.string() + ".h", oldBase.string() + ".h", rollback);
            m_error = "Could not rename script pair: " + error.message();
            m_renamingScript = false;
            return;
        }

        std::ifstream input(newBase.string() + ".cpp");
        std::string contents((std::istreambuf_iterator<char>(input)), {});
        input.close();
        const std::string oldClass = ScriptClassName(oldBase.filename().string());
        const std::string newClass = ScriptClassName(finalName);
        ReplaceAll(contents, oldBase.filename().string() + ".h",
            finalName + ".h");
        ReplaceAll(contents, oldClass, newClass);
        std::ofstream output(newBase.string() + ".cpp", std::ios::trunc);
        output << contents;

        std::ifstream headerInput(newBase.string() + ".h");
        contents.assign(std::istreambuf_iterator<char>(headerInput), {});
        headerInput.close();
        ReplaceAll(contents, oldClass, newClass);
        std::ofstream headerOutput(newBase.string() + ".h", std::ios::trunc);
        headerOutput << contents;

        if (OnAssetRenamed)
        {
            OnAssetRenamed(oldBase.string() + ".h", newBase.string() + ".h");
            OnAssetRenamed(oldBase.string() + ".cpp", newBase.string() + ".cpp");
        }
        if (OnAssetContentsChanged)
        {
            OnAssetContentsChanged(newBase.string() + ".h");
            OnAssetContentsChanged(newBase.string() + ".cpp");
        }
    }

    m_scriptBasePath = newBase.string();
    m_renamingScript = false;
    m_error.clear();
    SelectPath(newBase.string() + ".cpp");
}

void AssetsExplorerView::SelectPath(const std::string& path, bool multi,
    bool range)
{
    if (range && !m_selectionAnchor.empty())
    {
        const auto selectRange = [&](const std::vector<std::string>& ordered)
        {
            const auto anchor = std::find(ordered.begin(), ordered.end(),
                m_selectionAnchor);
            const auto clicked = std::find(ordered.begin(), ordered.end(), path);
            if (anchor == ordered.end() || clicked == ordered.end()) return false;
            const auto first = anchor < clicked ? anchor : clicked;
            const auto last = anchor < clicked ? clicked : anchor;
            m_selectedPaths.assign(first, std::next(last));
            if (m_selectedPaths.back() != path)
            {
                m_selectedPaths.erase(std::find(m_selectedPaths.begin(),
                    m_selectedPaths.end(), path));
                m_selectedPaths.push_back(path);
            }
            return true;
        };
        if (!selectRange(m_visiblePaths) &&
            !selectRange(m_previousVisiblePaths))
            m_selectedPaths.assign(1, path);
        m_selectedPath = path;
    }
    else if (multi)
    {
        const auto selected = std::find(m_selectedPaths.begin(),
            m_selectedPaths.end(), path);
        if (selected == m_selectedPaths.end())
            m_selectedPaths.push_back(path);
        else
            m_selectedPaths.erase(selected);
        m_selectedPath = m_selectedPaths.empty() ? std::string() :
            m_selectedPaths.back();
        m_selectionAnchor = path;
    }
    else
    {
        SetSelectedPath(path);
    }
    if (OnSelectionChanged)
        OnSelectionChanged(m_selectedPath);
}

void AssetsExplorerView::CommitAssetRename()
{
    if (!m_renamingAsset || m_renamePath.empty())
        return;

    std::error_code error;
    const fs::path oldPath = fs::weakly_canonical(m_renamePath, error);
    if (error || !fs::exists(oldPath))
    {
        m_error = "Asset no longer exists.";
        m_renamingAsset = false;
        m_renamePath.clear();
        return;
    }

    const std::string requested = CleanAssetFileName(m_renameName);
    if (requested.empty() || requested == "." || requested == ".." ||
        oldPath == fs::path(m_assetsPath))
    {
        m_error = "Filename is empty or invalid.";
        m_renamingAsset = false;
        m_renamePath.clear();
        return;
    }

    fs::path destination = oldPath.parent_path() / requested;
    std::string uniqueName = requested;
    for (unsigned suffix = 2; destination != oldPath && fs::exists(destination);
        ++suffix)
    {
        uniqueName = requested + " " + std::to_string(suffix);
        destination = oldPath.parent_path() / uniqueName;
    }

    if (destination != oldPath)
    {
        fs::rename(oldPath, destination, error);
        if (error)
        {
            m_error = "Could not rename asset: " + error.message();
            m_renamingAsset = false;
            m_renamePath.clear();
            return;
        }

        if (!fs::is_directory(destination, error))
        {
            std::error_code recordError;
            if (!Engine::Core::AssetRecord::Move(oldPath, destination, recordError))
            {
                std::error_code rollbackError;
                fs::rename(destination, oldPath, rollbackError);
                m_error = "Could not move asset metadata: " +
                    recordError.message();
                m_renamingAsset = false;
                m_renamePath.clear();
                return;
            }
        }

        const std::string oldPathString = oldPath.string();
        const std::string destinationString = destination.string();
        if (OnAssetRenamed)
            OnAssetRenamed(oldPathString, destinationString);
        m_locationIndex.RecordKnownMove(oldPath, destination);
        AssetPreviewCache::MovePersistentPreview(oldPathString,
            destinationString);
        SelectPath(destinationString);
    }

    m_error.clear();
    m_renamingAsset = false;
    m_renamePath.clear();
}

bool AssetsExplorerView::MoveAssetPath(const std::string& sourcePath,
    const std::string& destinationDirectory, std::string* movedPath)
{
    std::error_code error;
    const fs::path source = fs::weakly_canonical(sourcePath, error);
    const fs::path destinationRoot = fs::weakly_canonical(destinationDirectory,
        error);
    const fs::path assetsRoot = fs::weakly_canonical(m_assetsPath, error);
    if (error || !fs::exists(source) || !fs::is_directory(destinationRoot) ||
        !IsPathWithin(assetsRoot, source) ||
        (destinationRoot != assetsRoot &&
            !IsPathWithin(assetsRoot, destinationRoot)))
        return false;

    if (fs::is_directory(source))
    {
        const fs::path relative = destinationRoot.lexically_relative(source);
        if (relative.empty() || (!relative.empty() && *relative.begin() != ".."))
        {
            m_error = "Cannot move a folder into itself.";
            return false;
        }
    }

    fs::path destination = destinationRoot / source.filename();
    for (unsigned suffix = 2; fs::exists(destination); ++suffix)
    {
        destination = destinationRoot / (source.stem().string() + " " +
            std::to_string(suffix) + source.extension().string());
    }

    fs::rename(source, destination, error);
    if (error)
    {
        m_error = "Could not move asset: " + error.message();
        return false;
    }

    if (!fs::is_directory(destination))
    {
        std::error_code recordError;
        if (!Engine::Core::AssetRecord::Move(source, destination, recordError))
        {
            std::error_code rollbackError;
            fs::rename(destination, source, rollbackError);
            m_error = "Could not move asset metadata: " +
                recordError.message();
            return false;
        }
    }

    const std::string sourceString = source.string();
    const std::string destinationString = destination.string();
    if (OnAssetRenamed)
        OnAssetRenamed(sourceString, destinationString);
    m_locationIndex.RecordKnownMove(source, destination);

    if (m_selectedPath == sourceString)
        SelectPath(destinationString);

    if (movedPath)
        *movedPath = destinationString;
    m_error.clear();
    return true;
}

bool AssetsExplorerView::CopyExternalPath(const std::string& sourcePath,
    const std::string& destinationDirectory, std::string* copiedPath)
{
    if (m_assetsPath.empty())
    {
        m_error = "Assets path is not initialized.";
        return false;
    }
    std::error_code error;
    const fs::path source = fs::weakly_canonical(sourcePath, error);
    if (error || !fs::exists(source))
    {
        m_error = "Dropped file or folder no longer exists.";
        return false;
    }
    const fs::path assetsRoot = fs::weakly_canonical(m_assetsPath, error);
    const fs::path destinationRoot = fs::weakly_canonical(
        destinationDirectory.empty() ? m_currentDirectory : destinationDirectory,
        error);
    if (error || !fs::is_directory(destinationRoot) ||
        (destinationRoot != assetsRoot &&
            !IsPathWithin(assetsRoot, destinationRoot)))
    {
        m_error = "Drop destination must be inside Assets.";
        return false;
    }
    const bool directory = fs::is_directory(source, error);
    if (error || (!directory && !fs::is_regular_file(source, error)))
    {
        m_error = "Only files and folders can be copied into Assets.";
        return false;
    }
    if (directory && (destinationRoot == source ||
        IsPathWithin(source, destinationRoot)))
    {
        m_error = "Cannot copy a folder into itself.";
        return false;
    }

    fs::path destination = destinationRoot / source.filename();
    const std::string stem = destination.stem().string();
    const std::string extension = destination.extension().string();
    for (unsigned suffix = 2; fs::exists(destination); ++suffix)
        destination = destinationRoot /
            (stem + " " + std::to_string(suffix) + extension);

    bool createdDestination = false;
    try
    {
        const auto copyFile = [&](const fs::path& from, const fs::path& to)
        {
            if (!fs::copy_file(from, to))
                throw std::runtime_error("Destination already exists");
            if (to == destination) createdDestination = true;
            if (from.extension() == ".meta" ||
                (!from.filename().string().empty() &&
                    from.filename().string().front() == '.'))
                return;
            std::optional<Engine::Core::AssetRecord> record;
            try { record = Engine::Core::AssetRecord::Load(from); }
            catch (...) { /* Copy the file even if its sidecar is invalid. */ }
            Engine::Core::AssetRecord::Ensure(to,
                record ? fs::path(record->sourcePath) : from,
                record ? record->importSettings :
                    Engine::Model::AssetImportSettings{});
        };

        if (directory)
        {
            if (!fs::create_directory(destination))
                throw std::runtime_error("Destination already exists");
            createdDestination = true;
            for (fs::recursive_directory_iterator it(source,
                    fs::directory_options::none), end;
                it != end; ++it)
            {
                const fs::directory_entry& entry = *it;
                if (entry.is_symlink())
                {
                    it.disable_recursion_pending();
                    continue;
                }
                const fs::path relative = entry.path().lexically_relative(source);
                const fs::path target = destination / relative;
                if (entry.is_directory())
                    fs::create_directories(target);
                else if (entry.is_regular_file())
                {
                    if (entry.path().filename() == ".assetlocations.json")
                        continue;
                    if (entry.path().extension() == ".meta")
                    {
                        const std::string path = entry.path().string();
                        if (fs::exists(fs::path(path.substr(0, path.size() - 5))))
                            continue;
                    }
                    copyFile(entry.path(), target);
                }
            }
        }
        else
            copyFile(source, destination);
    }
    catch (const std::exception& exception)
    {
        if (createdDestination && IsPathWithin(assetsRoot, destination))
        {
            std::error_code cleanupError;
            fs::remove_all(destination, cleanupError);
        }
        m_error = "Could not copy dropped asset: " +
            std::string(exception.what());
        return false;
    }

    m_error.clear();
    SelectPath(destination.string());
    if (copiedPath) *copiedPath = destination.string();
    if (OnAssetContentsChanged)
        OnAssetContentsChanged(destination.string());
    CheckForExternalMoves();
    return true;
}

void AssetsExplorerView::CopySelectionToClipboard(const std::string& path,
    bool cut)
{
    const bool inSelection = std::find(m_selectedPaths.begin(),
        m_selectedPaths.end(), path) != m_selectedPaths.end();
    const std::vector<std::string> requested = inSelection
        ? m_selectedPaths : std::vector<std::string>{ path };
    m_clipboardPaths.clear();
    for (const std::string& item : requested)
    {
        const fs::path candidate = fs::path(item).lexically_normal();
        const bool nested = std::any_of(requested.begin(), requested.end(),
            [&candidate](const std::string& other)
            { return fs::path(other) != candidate &&
                IsPathWithin(fs::path(other), candidate); });
        if (!nested && std::find(m_clipboardPaths.begin(),
            m_clipboardPaths.end(), candidate.string()) == m_clipboardPaths.end())
            m_clipboardPaths.push_back(candidate.string());
    }
    m_clipboardCut = cut;
}

void AssetsExplorerView::PasteClipboard(const std::string& directory)
{
    if (m_clipboardPaths.empty()) return;
    for (const std::string& source : m_clipboardPaths)
        if (fs::is_directory(source) &&
            (fs::path(directory) == fs::path(source) ||
                IsPathWithin(source, directory)))
        {
            m_error = "Cannot paste a folder into itself.";
            return;
        }
    std::vector<std::string> pasted;
    std::vector<std::string> remaining;
    std::string firstError;
    for (const std::string& source : m_clipboardPaths)
    {
        std::string destination;
        bool success = false;
        if (m_clipboardCut && fs::path(source).parent_path() == fs::path(directory))
        {
            destination = source;
            success = true;
        }
        else if (m_clipboardCut)
            success = MoveAssetPath(source, directory, &destination);
        else
            success = CopyExternalPath(source, directory, &destination);
        if (success)
            pasted.push_back(destination);
        else
        {
            remaining.push_back(source);
            if (firstError.empty()) firstError = m_error;
        }
    }
    if (m_clipboardCut)
        m_clipboardPaths = std::move(remaining);
    if (!pasted.empty())
    {
        m_selectedPaths = std::move(pasted);
        m_selectedPath = m_selectedPaths.back();
        m_selectionAnchor = m_selectedPath;
        if (OnSelectionChanged) OnSelectionChanged(m_selectedPath);
    }
    if (!firstError.empty()) m_error = firstError;
}

void AssetsExplorerView::ProcessPendingAssetDrop()
{
    if (m_pendingAssetDropSources.empty()) return;
    const std::vector<std::string> sources =
        std::move(m_pendingAssetDropSources);
    m_pendingAssetDropSources.clear();
    const std::string directory = std::move(m_pendingAssetDropDirectory);
    m_pendingAssetDropDirectory.clear();
    for (const std::string& source : sources)
        if (fs::is_directory(source) &&
            (fs::path(directory) == fs::path(source) ||
                IsPathWithin(source, directory)))
        {
            m_error = "Cannot move a folder into itself.";
            return;
        }
    std::vector<std::string> movedPaths;
    for (const std::string& source : sources)
    {
        const bool nested = std::any_of(sources.begin(), sources.end(),
            [&source](const std::string& other)
            { return source != other && IsPathWithin(other, source); });
        if (nested) continue;
        std::string extension = fs::path(source).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        std::error_code sourceError;
        if (fs::is_regular_file(source, sourceError) &&
            (extension == ".vmd" || extension == ".fbx" ||
            extension == ".pmx" || extension == ".gltf" ||
            extension == ".glb" || extension == ".obj" ||
            extension == ".spriteanim") && OnObjectImportRequested)
            OnObjectImportRequested(source, directory);
        else
        {
            std::string moved;
            if (MoveAssetPath(source, directory, &moved))
                movedPaths.push_back(std::move(moved));
        }
    }
    if (!movedPaths.empty())
    {
        m_selectedPaths = std::move(movedPaths);
        m_selectedPath = m_selectedPaths.back();
        m_selectionAnchor = m_selectedPath;
        if (OnSelectionChanged) OnSelectionChanged(m_selectedPath);
    }
}

void AssetsExplorerView::QueueDeletePath(const std::string& path)
{
    if (path.empty()) return;
    const bool selected = std::find(m_selectedPaths.begin(),
        m_selectedPaths.end(), path) != m_selectedPaths.end();
    if (selected)
        m_pendingDeletePaths.insert(m_pendingDeletePaths.end(),
            m_selectedPaths.begin(), m_selectedPaths.end());
    else
        m_pendingDeletePaths.push_back(path);
}

void AssetsExplorerView::DeleteQueuedPaths()
{
    if (m_pendingDeletePaths.empty()) return;
    std::vector<fs::path> roots;
    for (const std::string& pending : m_pendingDeletePaths)
    {
        const fs::path candidate = fs::path(pending).lexically_normal();
        if (std::find(roots.begin(), roots.end(), candidate) == roots.end())
            roots.push_back(candidate);
    }
    m_pendingDeletePaths.clear();
    for (const fs::path& root : roots)
    {
        const bool covered = std::any_of(roots.begin(), roots.end(),
            [&root](const fs::path& other)
            { return other != root && IsPathWithin(other, root); });
        if (!covered) DeleteAssetPath(root.string());
    }
}

bool AssetsExplorerView::DeleteAssetPath(const std::string& path)
{
    std::error_code error;
    const fs::path candidate = fs::weakly_canonical(path, error);
    const fs::path assetsRoot = fs::weakly_canonical(m_assetsPath, error);
    if (error || !fs::exists(candidate) || !IsPathWithin(assetsRoot, candidate))
        return false;
    if (candidate == assetsRoot)
    {
        m_error = "Cannot delete the root Assets folder.";
        return false;
    }

    if (fs::is_directory(candidate))
    {
        fs::remove_all(candidate, error);
    }
    else
    {
        AssetPreviewCache::RemovePersistentPreview(candidate.string());
        fs::remove(candidate, error);
        if (!error)
            fs::remove(Engine::Core::AssetRecord::SidecarPath(candidate), error);
    }
    if (error)
    {
        m_error = "Could not delete asset: " + error.message();
        return false;
    }

    const auto removed = [&candidate](const std::string& selected)
    {
        const fs::path selectedPath = fs::path(selected).lexically_normal();
        return selectedPath == candidate || IsPathWithin(candidate, selectedPath);
    };
    m_selectedPaths.erase(std::remove_if(m_selectedPaths.begin(),
        m_selectedPaths.end(), removed), m_selectedPaths.end());
    if (!m_selectionAnchor.empty() && removed(m_selectionAnchor))
        m_selectionAnchor.clear();
    if (!m_selectedPath.empty() && removed(m_selectedPath))
    {
        m_selectedPath = m_selectedPaths.empty() ? std::string() :
            m_selectedPaths.back();
        if (OnSelectionChanged) OnSelectionChanged(m_selectedPath);
    }
    if (fs::is_directory(assetsRoot) &&
        (fs::path(m_currentDirectory) == candidate ||
            IsPathWithin(candidate, fs::path(m_currentDirectory))))
        m_currentDirectory = candidate.parent_path().string();
    m_backHistory.erase(std::remove_if(m_backHistory.begin(),
        m_backHistory.end(), removed), m_backHistory.end());
    if (OnAssetContentsChanged)
        OnAssetContentsChanged(candidate.string());
    m_error.clear();
    return true;
}

bool AssetsExplorerView::AcceptSceneObject(IEditorUi& ui, const std::string& directory)
{
    if (!ui.BeginDragDropTarget())
        return false;

    bool created = false;
    size_t size = 0;
    if (const void* assetData = ui.AcceptDragDropPayload("ENGINE_ASSET_PATH", &size))
    {
        const auto* sourcePath = static_cast<const char*>(assetData);
        if (sourcePath && size > 1)
        {
            const std::string source(sourcePath);
            const bool selected = std::find(m_selectedPaths.begin(),
                m_selectedPaths.end(), source) != m_selectedPaths.end();
            m_pendingAssetDropSources = selected
                ? m_selectedPaths : std::vector<std::string>{ source };
            m_pendingAssetDropDirectory = directory;
        }
    }

    const void* data = ui.AcceptDragDropPayload("ENGINE_SCENE_OBJECT", &size);
    if (data && size == sizeof(Engine::Core::Object*))
    {
        Engine::Core::Object* object = *static_cast<Engine::Core::Object* const*>(data);
        Engine::Core::Object* prefabRoot = object ? object->GetPrefabInstanceRoot() : nullptr;
        if (prefabRoot && prefabRoot->Prefab)
        {
            SetSelectedPath(prefabRoot->Prefab->GetPath());
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

            if (Engine::Serialization::SceneSerializer::SavePrefab(*object, destination.string()))
            {
                Engine::Core::AssetRecord::Ensure(destination, destination,
                    { { "importer", std::string("native") } });
                object->SetPrefab(destination.string());
                SetSelectedPath(destination.string());
                created = true;
                if (OnPrefabCreated)
                    OnPrefabCreated(object, destination.string());
            }
        }
    }
    ui.EndDragDropTarget();
    return created;
}

// ---------------------------------------------------------------------------
// OpenFile
// ---------------------------------------------------------------------------
void AssetsExplorerView::OpenFile(const std::string& filePath)
{
    const std::string extension =
        Engine::Editor::LowerAssetExtension(filePath);
    if (IsCodeFile(filePath))
    {
        if (OpenInCodeEditor(filePath, m_codeEditor))
        {
            if (OnCodeFileOpened) OnCodeFileOpened(filePath);
            return;
        }
        // Keep the system file association available when no editor is selected.
    }
    if (extension == ".xml")
    {
        pugi::xml_document document;
        if (document.load_file(filePath.c_str()))
        {
            const std::string root = document.document_element().name();
            if (root == "Scene" && OnAssetDocumentRequested)
            {
                OnAssetDocumentRequested(filePath, "Scene Editor");
                return;
            }
            if (root == "Prefab" && OnAssetDocumentRequested)
            {
                OnAssetDocumentRequested(filePath, "Object Editor");
                return;
            }
        }
        // XML without a recognized engine document root remains available to
        // the operating-system file association.
    }
    const auto editor = AssetEditorRegistry().find(extension);
    if (editor != AssetEditorRegistry().end() && OnAssetDocumentRequested)
    {
        OnAssetDocumentRequested(filePath, editor->second);
        return;
    }

    // Use ShellExecute to open the file with the default application
    const std::wstring wideFilePath = fs::absolute(fs::path(filePath)).wstring();
    ShellExecuteW(nullptr, L"open", wideFilePath.c_str(), nullptr, nullptr,
        SW_SHOWNORMAL);
}
}
