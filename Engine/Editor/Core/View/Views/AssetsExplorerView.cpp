#include "AssetsExplorerView.h"
#include "Engine/Editor/Core/View/Templates/Common/AssetPathTemplate.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Engine/Editor/UI/EditorIcons.h"
#include "Core/AssetRecord.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Scene/Scene.h"
#include "../Focus/WindowFocusHandler.h"
#include <filesystem>
#include <fstream>
#include <shellapi.h>

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

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void AssetsExplorerView::Init(const std::string& assetsPath,
    Engine::Scene::Scene* scene)
{
    m_assetsPath = fs::path(assetsPath).lexically_normal().string();
    m_currentDirectory = m_assetsPath;
    m_backHistory.clear();
    m_scene = scene;
    m_previewCache.Clear();
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

    if (ui.Button(Icons::List))
        m_gridView = false;
    ui.Tooltip("List view");
    ui.SameLine();
    if (ui.Button(Icons::Grid))
        m_gridView = true;
    ui.Tooltip("Thumbnail view");
    if (m_gridView)
    {
        ui.SameLine();
        ui.SliderInt("Thumbnail size", &m_thumbnailSize, 64, 192);
    }
    ui.InputText("##assetSearch", m_search, sizeof(m_search));
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
        const EditorUiTextEditResult rename = ui.RenameText(
            "##assetRename", m_renameName, sizeof(m_renameName),
            m_focusAssetRename);
        m_focusAssetRename = false;
        if (rename.submitted || rename.deactivated)
            CommitAssetRename();
    }
    DrawCurrentDirectory(ui);
    const EditorUiAssetCreateMenuResult create = ui.AssetWindowContextMenu();
    if (create.folderRequested)
        CreateFolder();
    if (create.scriptRequested)
        CreateScript();

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
                const bool selected = m_selectedPath == entryPath;
                const bool directory = entry.is_directory();
                const bool expandable = directory && !entry.is_symlink();
                const std::string label = directory
                    ? std::string(Icons::Folder) + " " +
                        entry.path().filename().string()
                    : std::string(Icons::Document) + " " +
                        entry.path().filename().string();

                ui.PushId(entryPath.c_str());

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
                    if (AssetPreviewCache::Supports(entryPath) && m_scene)
                    {
                        if (void* preview = m_previewCache.Get(
                            entryPath, m_scene->GetGraphicsProvider()))
                        {
                            if (AssetPreviewCache::IsCircularPreview(entryPath))
                                ui.DrawCircularImage(preview, 34.f);
                            else
                                ui.DrawImage(preview, 34.f, 34.f);
                            ui.SameLine();
                        }
                    }
                    activated = ui.Selectable(label.c_str(), selected, true);
                }

                const bool doubleClicked = ui.IsItemDoubleClicked();
                if (activated)
                    SelectPath(entryPath);
                if (doubleClicked)
                {
                    if (directory)
                        EnterDirectory(entryPath);
                    else
                        OpenFile(entryPath);
                }

                const EditorUiAssetItemMenuResult menu =
                    ui.AssetItemContextMenu(entryPath.c_str());
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
                    deleted = DeleteAssetPath(entryPath);

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
                    if (!deleted && !doubleClicked)
                        drawDirectory(entry.path());
                    ui.TreePop();
                }
                ui.PopId();
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

        const float tileSize = static_cast<float>(m_thumbnailSize);
        const int columns = std::max(1, static_cast<int>(
            ui.AvailableContentWidth() / (tileSize + 18.f)));
        int column = 0;
        for (const auto& entry : entries)
        {
            const std::string entryPath = entry.path().string();
            const bool directory = entry.is_directory();
            void* preview = nullptr;
            if (!directory && m_scene && AssetPreviewCache::Supports(entryPath))
                preview = m_previewCache.Get(entryPath,
                    m_scene->GetGraphicsProvider());

            ui.PushId(entryPath.c_str());
            const EditorUiAssetTileResult tile = ui.AssetTile(
                entry.path().filename().string().c_str(),
                directory ? Icons::Folder : Icons::Document, preview,
                m_selectedPath == entryPath, tileSize);
            if (tile.clicked)
                SelectPath(entryPath);
            if (tile.doubleClicked)
            {
                if (directory)
                    EnterDirectory(entryPath);
                else
                    OpenFile(entryPath);
            }

            const EditorUiAssetItemMenuResult menu =
                ui.AssetItemContextMenu(entryPath.c_str());
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
                deleted = DeleteAssetPath(entryPath);

            if (!deleted && ui.BeginDragDropSource())
            {
                ui.SetDragDropPayload("ENGINE_ASSET_PATH",
                    entryPath.c_str(), entryPath.size() + 1);
                ui.Label(entry.path().filename().string().c_str());
                ui.EndDragDropSource();
            }
            if (!deleted && directory)
                AcceptSceneObject(ui, entryPath);
            ui.PopId();

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
    m_selectedPath.clear();
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
        std::vector<fs::path> childFolders;
        std::error_code directoryError;
        for (fs::directory_iterator iterator(folder, directoryError), end;
            iterator != end && !directoryError; iterator.increment(directoryError))
        {
            if (!iterator->is_directory(directoryError))
                continue;
            const std::string name = iterator->path().filename().string();
            if (!name.empty() && name.front() != '.')
                childFolders.push_back(iterator->path());
        }
        std::sort(childFolders.begin(), childFolders.end(),
            [](const fs::path& first, const fs::path& second)
            {
                return first.filename().string() < second.filename().string();
            });
        std::vector<std::string> names;
        std::vector<const char*> items;
        names.reserve(childFolders.size());
        items.reserve(childFolders.size());
        for (const fs::path& child : childFolders)
            names.push_back(child.filename().string());
        for (const std::string& name : names)
            items.push_back(name.c_str());

        ui.PushId(folder.string().c_str());
        const EditorUiBreadcrumbResult result = ui.Breadcrumb(label,
            items.data(), static_cast<int>(items.size()));
        ui.PopId();
        if (result.clicked)
            EnterDirectory(folder.string());
        else if (result.childSelected >= 0 &&
            result.childSelected < static_cast<int>(childFolders.size()))
            EnterDirectory(childFolders[result.childSelected].string());
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
        ui.Label("/");
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

void AssetsExplorerView::SelectPath(const std::string& path)
{
    m_selectedPath = path;
    if (OnSelectionChanged)
        OnSelectionChanged(path);
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
    if (requested.empty() || requested == "." || requested == "..")
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
        !IsPathWithin(assetsRoot, source) || !IsPathWithin(assetsRoot, destinationRoot))
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

    if (m_selectedPath == sourceString)
        SelectPath(destinationString);

    if (movedPath)
        *movedPath = destinationString;
    m_error.clear();
    return true;
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

    if (m_selectedPath == candidate.string())
        SelectPath({});
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
            MoveAssetPath(sourcePath, directory);
    }

    const void* data = ui.AcceptDragDropPayload("ENGINE_SCENE_OBJECT", &size);
    if (data && size == sizeof(Engine::Core::Object*))
    {
        Engine::Core::Object* object = *static_cast<Engine::Core::Object* const*>(data);
        Engine::Core::Object* prefabRoot = object ? object->GetPrefabInstanceRoot() : nullptr;
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

            if (Engine::Serialization::SceneSerializer::SavePrefab(*object, destination.string()))
            {
                Engine::Core::AssetRecord::Ensure(destination, destination,
                    { { "importer", std::string("native") } });
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

// ---------------------------------------------------------------------------
// OpenFile
// ---------------------------------------------------------------------------
void AssetsExplorerView::OpenFile(const std::string& filePath)
{
    // Check if this is a scene file
    const std::string extension =
        Engine::Editor::LowerAssetExtension(filePath);
    if (extension == ".scene" || extension == ".xml")
    {
        // Trigger the scene load callback
        if (OnSceneRequested)
        {
            OnSceneRequested(filePath);
        }
        return;
    }
    if (extension == ".prefab")
    {
        if (OnPrefabRequested)
            OnPrefabRequested(filePath);
        return;
    }

    // Convert to wide string for Windows API
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), (int)filePath.length(), NULL, 0);
    std::wstring wideFilePath(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), (int)filePath.length(), &wideFilePath[0], size_needed);

    // Use ShellExecute to open the file with the default application
    ShellExecuteW(NULL, L"open", wideFilePath.c_str(), NULL, NULL, SW_SHOW);
}
}
