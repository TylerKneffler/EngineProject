#include "pch.h"
#include "Component.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Serialization/Json.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <map>
#include <filesystem>
#include <unordered_set>
#include <Windows.h>
#include <commdlg.h>

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

namespace Engine::Core
{

// State tracking for property editing
static std::map<std::string, std::string> s_editingProperty;  // componentPtr+key -> "editing"
struct ReferenceSearchState
{
    bool open = false;
    char query[256]{};
    std::vector<std::string> assets;
};
static std::map<std::string, ReferenceSearchState> s_referenceSearch;
static std::string s_editorAssetDirectory;
static bool IsCompatiblePathAsset(const std::string& property,
    const std::string& path);

void Component::SetEditorAssetDirectory(const std::string& path)
{
    s_editorAssetDirectory = path;
    s_referenceSearch.clear();
}

void Component::ClearEditorReferenceSearches()
{
    s_referenceSearch.clear();
}

static std::string LowerText(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character)
        { return static_cast<char>(std::tolower(character)); });
    return value;
}

static std::string ObjectHierarchyPath(const Object* object)
{
    std::vector<std::string> names;
    for (const Object* current = object; current; current = current->Parent)
        names.push_back(current->name.empty() ? "(unnamed)" : current->name);
    std::string path;
    for (auto it = names.rbegin(); it != names.rend(); ++it)
    {
        if (!path.empty()) path += " / ";
        path += *it;
    }
    return path;
}

static std::string ComponentReferenceLabel(const Component* component)
{
    if (!component || !component->Owner) return "(missing component)";
    const std::string owner = ObjectHierarchyPath(component->Owner);
    if (component == &component->Owner->transform)
        return owner;
    int index = 0;
    for (const Component* candidate : component->Owner->Components)
    {
        if (candidate == component) break;
        if (candidate && candidate->GetTypeName() == component->GetTypeName())
            ++index;
    }
    return owner + " / " + component->GetTypeName() +
        (index ? " #" + std::to_string(index + 1) : "");
}

static std::vector<std::string> FindAssetReferences(const std::string& key)
{
    namespace fs = std::filesystem;
    std::vector<std::string> assets;
    std::unordered_set<std::string> visitedRoots;
    std::error_code engineError;
    const fs::path engineRoot = fs::weakly_canonical(
        fs::path(ENGINE_ASSETS_PATH), engineError);
    const fs::path roots[] = { s_editorAssetDirectory, "Assets",
        ENGINE_ASSETS_PATH };
    for (const fs::path& root : roots)
    {
        if (root.empty()) continue;
        std::error_code error;
        const fs::path absolute = fs::weakly_canonical(root, error);
        if (error || !fs::is_directory(absolute, error) ||
            !visitedRoots.insert(LowerText(absolute.string())).second)
            continue;
        fs::recursive_directory_iterator it(absolute,
            fs::directory_options::skip_permission_denied, error), end;
        for (; !error && it != end && assets.size() < 30000u;
            it.increment(error))
        {
            if (it->is_regular_file(error) &&
                IsCompatiblePathAsset(key, it->path().string()))
            {
                const fs::path relative = it->path().lexically_relative(
                    absolute);
                fs::path stored = it->path();
                if (!engineError && absolute == engineRoot)
                    stored = fs::path("Assets") / relative;
                else if (!root.is_absolute())
                    stored = root / relative;
                else
                {
                    std::error_code relativeError;
                    const fs::path fromWorkingDirectory = fs::relative(
                        it->path(), fs::current_path(), relativeError);
                    if (!relativeError && !fromWorkingDirectory.empty() &&
                        *fromWorkingDirectory.begin() != "..")
                        stored = fromWorkingDirectory;
                }
                assets.push_back(stored.generic_string());
            }
            error.clear();
        }
    }
    std::sort(assets.begin(), assets.end());
    assets.erase(std::unique(assets.begin(), assets.end()), assets.end());
    return assets;
}

bool Component::DrawAssetPathPicker(::Engine::Editor::IEditorUi& ui,
    const void* context, const char* fieldKey, const char* label,
    const char* assetKind, std::string& path)
{
    const std::string stateKey = std::to_string(
        reinterpret_cast<uintptr_t>(context)) + "_" + fieldKey;
    ReferenceSearchState& search = s_referenceSearch[stateKey];
    const std::string filter = assetKind ? assetKind : fieldKey;
    bool changed = false;
    ui.PushId(fieldKey);
    ui.ValueLabel(label, path.empty() ? "(none)" : path.c_str());
    if (ui.IsItemHovered() && !path.empty())
        ui.Tooltip(path.c_str());
    if (ui.BeginDragDropTarget())
    {
        size_t size = 0;
        const void* data = ui.AcceptDragDropPayload("ENGINE_ASSET_PATH", &size);
        if (data && size > 0)
        {
            const char* bytes = static_cast<const char*>(data);
            size_t length = 0;
            while (length < size && bytes[length] != '\0') ++length;
            const std::string dropped(bytes, length);
            if (IsCompatiblePathAsset(filter, dropped))
            {
                path = dropped;
                changed = true;
            }
        }
        ui.EndDragDropTarget();
    }
    ui.SameLineRight(52.f);
    if (ui.Button("...##assetSearch", 24.f))
    {
        search.open = !search.open;
        if (search.open)
            search.assets = FindAssetReferences(filter);
        else
            search.assets.clear();
    }
    if (ui.IsItemHovered()) ui.Tooltip("Search assets");
    ui.SameLine();
    ui.BeginDisabled(path.empty());
    if (ui.Button("x##clearAsset", 20.f))
    {
        path.clear();
        changed = true;
    }
    ui.EndDisabled();
    if (ui.IsItemHovered()) ui.Tooltip("Clear path");
    if (search.open)
    {
        ui.InputText("Find asset", search.query, sizeof(search.query));
        const std::string query = LowerText(search.query);
        size_t matches = 0;
        for (const std::string& asset : search.assets)
        {
            if (!query.empty() && LowerText(asset).find(query) ==
                std::string::npos)
                continue;
            ++matches;
            if (matches > 30u) continue;
            ui.PushId(asset.c_str());
            if (ui.Selectable(asset.c_str(), asset == path))
            {
                path = asset;
                search.open = false;
                search.assets.clear();
                changed = true;
                ui.PopId();
                break;
            }
            ui.PopId();
        }
        if (matches > 30u)
            ui.DisabledLabel("More assets match. Refine the search.");
        else if (matches == 0u)
            ui.DisabledLabel("No matching assets in the project or engine folders.");
    }
    ui.PopId();
    return changed;
}

bool Component::DrawReferenceProperty(::Engine::Editor::IEditorUi& ui,
    const char* fieldKey, const char* label, ComponentReference& reference,
    const Component* defaultTarget, const char* defaultDescription)
{
    const std::string key = fieldKey ? fieldKey : label;
    const std::string stateKey = std::to_string(
        reinterpret_cast<uintptr_t>(this)) + "_" + key;
    ReferenceSearchState& search = s_referenceSearch[stateKey];
    Component* resolved = reference.IsAssigned()
        ? ResolveComponentReferenceRaw(Owner, reference) : nullptr;
    if (!reference.IsAssigned() && !defaultTarget &&
        !defaultDescription && Owner)
    {
        if (reference.expectedType == "Transform")
            defaultTarget = &Owner->transform;
        else
            for (Component* candidate : Owner->Components)
                if (candidate && candidate->GetTypeName() ==
                    reference.expectedType)
                {
                    defaultTarget = candidate;
                    break;
                }
    }
    const std::string targetLabel = resolved
        ? ComponentReferenceLabel(resolved)
        : reference.IsAssigned()
            ? "(missing) " + reference.objectName + " / " +
                reference.componentType
            : defaultDescription
                ? "(default) " + std::string(defaultDescription)
                : defaultTarget
                ? "(default) " + ComponentReferenceLabel(defaultTarget)
                : "(default: " + (reference.expectedType.empty()
                ? std::string("component") : reference.expectedType) + ")";
    bool changed = false;
    ui.PushId(key.c_str());
    ui.ValueLabel(label, targetLabel.c_str());
    if (ui.IsItemHovered() && reference.IsAssigned())
        ui.Tooltip(targetLabel.c_str());
    if (ui.BeginDragDropTarget())
    {
        size_t size = 0;
        const void* data = ui.AcceptDragDropPayload(
            "ENGINE_COMPONENT_REORDER", &size);
        if (data && size == sizeof(Component*))
        {
            auto* component = *static_cast<Component* const*>(data);
            if (component && Owner && component->Owner &&
                component->Owner->GetScene() == Owner->GetScene() &&
                (reference.expectedType.empty() ||
                    component->GetTypeName() == reference.expectedType))
            {
                reference = CaptureComponentReference(component,
                    reference.expectedType);
                changed = true;
            }
        }
        if (reference.expectedType == "Transform")
        {
            size = 0;
            data = ui.AcceptDragDropPayload("ENGINE_SCENE_OBJECT", &size);
            if (data && size == sizeof(Object*))
            {
                auto* object = *static_cast<Object* const*>(data);
                if (object && Owner && object->GetScene() == Owner->GetScene())
                {
                    reference = CaptureComponentReference(
                        &object->transform, "Transform");
                    changed = true;
                }
            }
        }
        ui.EndDragDropTarget();
    }
    ui.SameLineRight(52.f);
    if (ui.Button("...##sceneSearch", 24.f))
        search.open = !search.open;
    if (ui.IsItemHovered()) ui.Tooltip("Search scene");
    if (reference.IsAssigned())
    {
        ui.SameLine();
        if (ui.Button("x##clearReference", 20.f))
        {
            reference.Clear();
            changed = true;
        }
        if (ui.IsItemHovered()) ui.Tooltip("Clear reference");
    }
    if (search.open)
    {
        ui.InputText("Find object or component", search.query,
            sizeof(search.query));
        if (Owner && Owner->GetScene())
        {
            const std::string query = LowerText(search.query);
            size_t matches = 0;
            bool selected = false;
            for (const auto& object : Owner->GetScene()->GetObjects())
            {
                std::vector<Component*> candidates;
                if (reference.expectedType == "Transform")
                    candidates.push_back(&object->transform);
                else
                    for (Component* candidate : object->Components)
                        if (candidate && (reference.expectedType.empty() ||
                            candidate->GetTypeName() == reference.expectedType))
                            candidates.push_back(candidate);
                for (Component* candidate : candidates)
                {
                    const std::string candidateLabel =
                        ComponentReferenceLabel(candidate);
                    if (!query.empty() && LowerText(candidateLabel).find(query)
                        == std::string::npos)
                        continue;
                    ++matches;
                    if (matches > 30u) continue;
                    ui.PushId(candidate);
                    if (ui.Selectable(candidateLabel.c_str(),
                        candidate == resolved))
                    {
                        reference = CaptureComponentReference(candidate,
                            reference.expectedType);
                        search.open = false;
                        changed = selected = true;
                    }
                    ui.PopId();
                    if (selected) break;
                }
                if (selected) break;
            }
            if (matches > 30u)
                ui.DisabledLabel("More scene targets match. Refine the search.");
            else if (matches == 0u)
                ui.DisabledLabel("No matching scene targets.");
        }
        else
            ui.DisabledLabel("This component is not in a scene.");
    }
    ui.PopId();
    return changed;
}

static std::string LowerExtension(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

static bool IsCompatiblePathAsset(const std::string& property,
    const std::string& path)
{
    std::string name = property;
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const std::string extension = LowerExtension(path);
    if (name.find("font") != std::string::npos)
        return extension == ".ttf" || extension == ".otf";
    if (name.find("audio") != std::string::npos)
        return extension == ".wav" || extension == ".ogg" || extension == ".mp3";
    if (name.find("mesh") != std::string::npos)
        return extension == ".mesh" || extension == ".obj" ||
            extension == ".gltf" || extension == ".glb" || extension == ".fbx";
    if (name.find("texture") != std::string::npos)
        return extension == ".png" || extension == ".jpg" ||
            extension == ".jpeg" || extension == ".bmp" ||
            extension == ".dds" || extension == ".tga" ||
            extension == ".hdr" || extension == ".exr" || extension == ".ktx2";
    if (name.find("scene") != std::string::npos)
        return extension == ".scene";
    if (name.find("prefab") != std::string::npos)
        return extension == ".prefab";
    if (name.find("material") != std::string::npos)
        return extension == ".material" || extension == ".mat";
    return true;
}

static std::string HumanizePropertyName(const std::string& name)
{
    std::string result;
    result.reserve(name.size() + 8);
    for (std::size_t index = 0; index < name.size(); ++index)
    {
        const unsigned char current = static_cast<unsigned char>(name[index]);
        if (current == '_' || current == '-')
        {
            if (!result.empty() && result.back() != ' ')
                result.push_back(' ');
            continue;
        }
        const bool upper = std::isupper(current) != 0;
        const bool previousLowerOrDigit = index > 0 &&
            (std::islower(static_cast<unsigned char>(name[index - 1])) != 0 ||
             std::isdigit(static_cast<unsigned char>(name[index - 1])) != 0);
        const bool acronymBoundary = upper && index > 0 && index + 1 < name.size() &&
            std::isupper(static_cast<unsigned char>(name[index - 1])) != 0 &&
            std::islower(static_cast<unsigned char>(name[index + 1])) != 0;
        if (upper && (previousLowerOrDigit || acronymBoundary) &&
            !result.empty() && result.back() != ' ')
            result.push_back(' ');
        result.push_back(static_cast<char>(current));
    }
    if (!result.empty())
        result[0] = static_cast<char>(std::toupper(
            static_cast<unsigned char>(result[0])));
    return result;
}

// ---------------------------------------------------------------------------
// Component::DrawProperties — Generic interactive property editor
//
// Automatically creates UI controls for all properties returned by Serialize().
// Properties marked with PROPERTY(Inspector) in headers should be included in
// Serialize() to appear here.
//
// Supported types:
//   - float/int: DragFloat with smart range detection based on property name
//   - bool: Checkbox
//   - vec3: ColorEdit3 for colors, DragFloat3 for positions/rotations/etc.
//   - string: InputText for editing
//   - file paths: Asset selection with "Browse..." button, image preview for textures
//      (detected by property names containing: file, path, texture)
//
// File path properties:
//   - Shows image preview for image files (png, jpg, etc.) when not editing
//   - Click to edit, shows InputText field and Browse button
//   - Browse button opens Windows file picker dialog
//   - Apply/Cancel buttons to confirm or discard changes
//
// Override in derived classes only when you need specialized UI controls
// or logic beyond this automatic property editing.
// ---------------------------------------------------------------------------
bool Component::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    // Get current serialized state
    JsonValue originalData = Serialize();
    
    // Skip if no serialization data
    if (originalData.IsNull() || !originalData.IsObject())
    {
        ui.DisabledLabel("(No properties)");
        return false;
    }
    
    // Create a mutable copy for editing
    JsonValue editedData = originalData;
    bool modified = false;
    std::vector<std::string> modifiedKeys;
    const auto markModified = [&](const std::string& key)
    {
        modified = true;
        if (std::find(modifiedKeys.begin(), modifiedKeys.end(), key) ==
            modifiedKeys.end())
            modifiedKeys.push_back(key);
    };
    std::string currentGroup;
    bool currentGroupOpen = true;
    bool groupIndented = false;
    
    // Display interactive controls for each property
    for (size_t i = 0; i < originalData.ObjectSize(); ++i)
    {
        const std::string& key = originalData.ObjectKey(i);
        const JsonValue& value = originalData.ObjectValue(i);
        
        // Skip the "type" field (it's the component name)
        if (key == "type") continue;

        const auto editorMetadata = std::find_if(m_editorFieldMetadata.begin(),
            m_editorFieldMetadata.end(), [&key](const EditorFieldMetadata& field)
            {
                return field.name == key;
            });
        const std::string group = editorMetadata != m_editorFieldMetadata.end()
            ? editorMetadata->group : std::string{};
        if (group != currentGroup)
        {
            if (groupIndented)
            {
                ui.Unindent(12.f);
                groupIndented = false;
            }
            currentGroup = group;
            currentGroupOpen = group.empty() || ui.PropertyGroupHeader(
                group.c_str(), editorMetadata == m_editorFieldMetadata.end() ||
                    editorMetadata->groupDefaultOpen);
            if (currentGroupOpen && !group.empty())
            {
                ui.Indent(12.f);
                groupIndented = true;
            }
        }
        if (!currentGroupOpen)
            continue;

        const std::string displayName = HumanizePropertyName(key);
        const bool mixedValue = IsEditorValueMixed(key, value);
        
        // Create appropriate control based on value type
        if (value.IsString())
        {
            const std::string stringValue = mixedValue ? "-" : value.AsString();
            
            // Detect file/path properties by name heuristics
            bool isFilePath = (key.find("file") != std::string::npos || key.find("File") != std::string::npos ||
                              key.find("path") != std::string::npos || key.find("Path") != std::string::npos ||
                              key.find("texture") != std::string::npos || key.find("Texture") != std::string::npos);
            
            if (isFilePath)
            {
                // Texture/file rows reuse hidden widget labels such as
                // "##path", "Browse...", "Apply", and "Cancel". Scope the
                // complete row by its serialized property name so multiple
                // visible material textures never share an ImGui ID.
                ui.PushId(key.c_str());

                // State key for tracking if this property is being edited
                std::string stateKey = std::to_string(reinterpret_cast<uintptr_t>(this)) + "_" + key;
                bool isEditing = (s_editingProperty.find(stateKey) != s_editingProperty.end());
                
                // Check if this is an image file for preview
                bool isImageFile = false;
                bool isEnvironmentImage = false;
                if (!stringValue.empty())
                {
                    std::string ext = stringValue.substr(stringValue.find_last_of('.') + 1);
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    isEnvironmentImage = ext == "hdr" || ext == "exr";
                    isImageFile = (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" ||
                                  ext == "tga" || ext == "dds" || isEnvironmentImage);
                }
                
                // Show preview image if available and not editing
                if (!isEditing && isImageFile && !stringValue.empty())
                {
                    ui.Label(displayName.c_str());
                    
                    // Try to load and display texture preview
                    void* textureHandle = nullptr;
                    std::shared_ptr<Engine::Components::Texture> previewTexture = nullptr;
                    
                    // Get graphics provider through the component's owner object and scene
                    IGraphicsProvider* graphicsProvider = nullptr;
                    if (Owner && Owner->OwnerScene)
                    {
                        graphicsProvider = Owner->OwnerScene->GetGraphicsProvider();
                    }
                    
                    if (graphicsProvider)
                    {
                        // Texture::Acquire uses the centralized long-term
                        // resource cache, shared with materials and previews.
                        previewTexture = Engine::Components::Texture::Acquire(
                            stringValue, !isEnvironmentImage);
                        if (previewTexture)
                            previewTexture->Load();
                        
                        // Ensure texture is prepared for GPU
                        if (previewTexture && previewTexture->HasPixels())
                        {
                            if (!previewTexture->GetGraphicsTexture())
                            {
                                previewTexture->Prepare(graphicsProvider);
                            }
                            
                            if (previewTexture->GetGraphicsTexture())
                            {
                                textureHandle = previewTexture->GetGraphicsTexture()->GetNativeHandle();
                            }
                        }
                    }
                    
                    // Display image preview or fallback text
                    if (textureHandle)
                    {
                        // Show small rectangular preview (128 width, maintain aspect ratio)
                        const float previewWidth = 128.0f;
                        float aspectRatio = 1.0f;
                        if (previewTexture && previewTexture->GetHeight() > 0)
                        {
                            aspectRatio = static_cast<float>(previewTexture->GetWidth()) / 
                                         static_cast<float>(previewTexture->GetHeight());
                        }
                        const float previewHeight = previewWidth / aspectRatio;
                        
                        if (isEnvironmentImage)
                            ui.DrawCircularImage(textureHandle, previewWidth);
                        else
                            ui.DrawImage(textureHandle, previewWidth, previewHeight);
                        
                        // When image is clicked, enter editing mode
                        if (ui.IsItemClicked())
                        {
                            s_editingProperty[stateKey] = stringValue;
                        }
                    }
                    else
                    {
                        // Fallback: show placeholder text
                        ui.DisabledLabel("[Image Preview Unavailable]");
                    }
                    
                    // Always show the file path below the preview
                    ui.DisabledLabel(stringValue.c_str());
                    
                    // When path text is clicked, enter editing mode
                    if (ui.IsItemClicked())
                    {
                        s_editingProperty[stateKey] = stringValue;
                    }
                }
                else
                {
                    // Show editable text field
                    ui.Label(displayName.c_str());
                    
                    // Get current edit value
                    std::string editValue = isEditing ? s_editingProperty[stateKey] : stringValue;
                    char buffer[512];
                    strncpy_s(buffer, sizeof(buffer), editValue.c_str(), _TRUNCATE);
                    
                    if (ui.InputText("##path", buffer, sizeof(buffer)))
                    {
                        s_editingProperty[stateKey] = std::string(buffer);
                    }
                    
                    ui.SameLine();
                    if (ui.Button("Browse...", 80.f, 0.f))
                    {
                        // Open file picker dialog
                        wchar_t filename[MAX_PATH] = {};
                        wchar_t initialDir[MAX_PATH] = {};

                        std::filesystem::path initialPath;
                        if (!editValue.empty())
                        {
                            const std::filesystem::path valuePath(editValue);
                            if (std::filesystem::exists(valuePath))
                                initialPath = valuePath.parent_path();
                        }
                        if (initialPath.empty())
                        {
                            const std::filesystem::path projectAssets("Assets");
                            if (std::filesystem::is_directory(projectAssets))
                                initialPath = std::filesystem::absolute(projectAssets);
                        }
                        if (initialPath.empty())
                        {
                            const std::filesystem::path engineAssets(ENGINE_ASSETS_PATH);
                            if (std::filesystem::is_directory(engineAssets))
                                initialPath = std::filesystem::absolute(engineAssets);
                        }
                        if (!initialPath.empty())
                        {
                            const std::wstring wideInit = initialPath.wstring();
                            wcsncpy_s(initialDir, wideInit.c_str(), _TRUNCATE);
                        }
                        
                        OPENFILENAMEW ofn{};
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = nullptr;
                        const bool fontProperty = key.find("font") != std::string::npos ||
                            key.find("Font") != std::string::npos;
                        ofn.lpstrFilter = fontProperty
                            ? L"Fonts (*.ttf;*.otf)\0*.ttf;*.otf\0All Files (*.*)\0*.*\0\0"
                            : L"All Files\0*.*\0Images\0*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2\0Models\0*.obj;*.gltf;*.glb;*.fbx\0Audio\0*.wav;*.ogg;*.mp3\0Fonts\0*.ttf;*.otf\0\0";
                        ofn.lpstrFile = filename;
                        ofn.nMaxFile = MAX_PATH;
                        ofn.lpstrInitialDir = initialDir[0] ? initialDir : nullptr;
                        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                        
                        if (GetOpenFileNameW(&ofn))
                        {
                            // Convert to narrow string and make relative if possible
                            char narrowPath[MAX_PATH];
                            WideCharToMultiByte(CP_UTF8, 0, filename, -1, narrowPath, MAX_PATH, nullptr, nullptr);
                            s_editingProperty[stateKey] = std::string(narrowPath);
                        }
                    }
                    
                    // Apply/Cancel buttons
                    if (isEditing)
                    {
                        ui.SameLine();
                        if (ui.Button("Apply", 60.f, 0.f))
                        {
                            editedData.Set(key, JsonValue(s_editingProperty[stateKey]));
                            markModified(key);
                            s_editingProperty.erase(stateKey);
                        }
                        
                        ui.SameLine();
                        if (ui.Button("Cancel", 60.f, 0.f))
                        {
                            s_editingProperty.erase(stateKey);
                        }
                    }
                    else
                    {
                        // Show current value when not editing
                        ui.DisabledLabel(stringValue.empty() ? "(none)" : stringValue.c_str());
                        
                        // Start editing on click
                        if (ui.IsItemClicked())
                        {
                            s_editingProperty[stateKey] = stringValue;
                        }
                    }
                }

                // Asset-browser files can be assigned directly to serialized
                // path properties. Font fields accept only TTF/OTF so an
                // unrelated drop cannot silently replace the active typeface.
                if (!isEditing && ui.BeginDragDropTarget())
                {
                    size_t payloadSize = 0;
                    const void* payload = ui.AcceptDragDropPayload(
                        "ENGINE_ASSET_PATH", &payloadSize);
                    if (payload && payloadSize > 0)
                    {
                        const char* bytes = static_cast<const char*>(payload);
                        size_t length = 0;
                        while (length < payloadSize && bytes[length] != '\0')
                            ++length;
                        const std::string droppedPath(bytes, length);
                        if (IsCompatiblePathAsset(key, droppedPath))
                        {
                            editedData.Set(key, JsonValue(droppedPath));
                            markModified(key);
                        }
                    }
                    ui.EndDragDropTarget();
                }

                // Mesh-backed physics properties are asset references, but
                // Mesh component headers are much more convenient drag
                // sources than finding the same file again in the browser.
                // Store the dropped component's portable file path rather
                // than a raw pointer so scenes remain safe and serializable.
                if (!isEditing && (key == "meshPath" || key == "MeshPath") &&
                    ui.BeginDragDropTarget())
                {
                    size_t payloadSize = 0;
                    const void* payload = ui.AcceptDragDropPayload(
                        "ENGINE_COMPONENT_REORDER", &payloadSize);
                    if (payload && payloadSize == sizeof(Component*))
                    {
                        Component* component = *static_cast<Component* const*>(payload);
                        if (auto* droppedMesh = dynamic_cast<Engine::Components::Mesh*>(component))
                        {
                            editedData.Set(key, JsonValue(droppedMesh->GetFilePath()));
                            markModified(key);
                        }
                    }
                    ui.EndDragDropTarget();
                }

                ReferenceSearchState& assetSearch = s_referenceSearch[stateKey];
                ui.SameLineRight(52.f);
                if (ui.Button("...##assetSearch", 24.f))
                {
                    assetSearch.open = !assetSearch.open;
                    if (assetSearch.open)
                        assetSearch.assets = FindAssetReferences(key);
                    else
                        assetSearch.assets.clear();
                }
                if (ui.IsItemHovered()) ui.Tooltip("Search assets");
                ui.SameLine();
                ui.BeginDisabled(stringValue.empty());
                if (ui.Button("x##clearAsset", 20.f))
                {
                    editedData.Set(key, JsonValue(std::string{}));
                    markModified(key);
                    s_editingProperty.erase(stateKey);
                }
                ui.EndDisabled();
                if (ui.IsItemHovered()) ui.Tooltip("Clear path");
                if (assetSearch.open)
                {
                    ui.InputText("Find asset", assetSearch.query,
                        sizeof(assetSearch.query));
                    const std::string query = LowerText(assetSearch.query);
                    size_t matches = 0;
                    for (const std::string& asset : assetSearch.assets)
                    {
                        if (!query.empty() &&
                            LowerText(asset).find(query) == std::string::npos)
                            continue;
                        ++matches;
                        if (matches > 30u) continue;
                        ui.PushId(asset.c_str());
                        if (ui.Selectable(asset.c_str()))
                        {
                            editedData.Set(key, JsonValue(asset));
                            markModified(key);
                            s_editingProperty.erase(stateKey);
                            assetSearch.open = false;
                            assetSearch.assets.clear();
                            ui.PopId();
                            break;
                        }
                        ui.PopId();
                    }
                    if (matches > 30u)
                        ui.DisabledLabel("More assets match. Refine the search.");
                    else if (matches == 0u)
                        ui.DisabledLabel("No matching assets in the project or engine folders.");
                }

                ui.PopId();
            }
            else
            {
                // Regular string property - make editable
                char buffer[256];
                strncpy_s(buffer, sizeof(buffer), stringValue.c_str(), _TRUNCATE);
                ui.SetNextItemMixedValue(mixedValue);
                if (ui.InputText(displayName.c_str(), buffer, sizeof(buffer)))
                {
                    editedData.Set(key, JsonValue(std::string(buffer)));
                    markModified(key);
                }
            }
        }
        else if (value.IsNumber())
        {
            float floatVal = value.AsFloat();
            ui.SetNextItemMixedValue(mixedValue);
            
            // Heuristics for appropriate ranges
            if (key == "metallicFactor" || key == "roughnessFactor" ||
                key == "baseColorAlpha" || key == "alphaCutoff" ||
                key == "occlusionStrength" || key == "alpha" ||
                key == "fillAmount")
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.01f, 0.f, 1.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    markModified(key);
                }
            }
            else if (key == "normalScale")
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.01f, 0.f, 2.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    markModified(key);
                }
            }
            else if (key == "heightScale")
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.001f, 0.f, 0.2f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    markModified(key);
                }
            }
            else if (key == "heightMinSteps" || key == "heightMaxSteps")
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 1.f, 4.f, 64.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    markModified(key);
                }
            }
            else if (key.find("fov") != std::string::npos || key.find("FOV") != std::string::npos)
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.5f, 1.f, 179.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    markModified(key);
                }
            }
            else if (key.find("near") != std::string::npos || key.find("Near") != std::string::npos)
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.001f, 0.001f, 10.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    markModified(key);
                }
            }
            else if (key.find("far") != std::string::npos || key.find("Far") != std::string::npos)
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 1.f, 1.f, 10000.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    markModified(key);
                }
            }
            else if (key.find("shininess") != std::string::npos)
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.5f, 1.f, 256.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    markModified(key);
                }
            }
            else if (key.find("speed") != std::string::npos)
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.5f, -360.f, 360.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    markModified(key);
                }
            }
            else
            {
                // Generic float with reasonable defaults
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.01f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    markModified(key);
                }
            }
        }
        else if (value.IsBool())
        {
            bool boolVal = value.AsBool();
            ui.SetNextItemMixedValue(mixedValue);
            if (ui.Checkbox(displayName.c_str(), &boolVal))
            {
                editedData.Set(key, JsonValue(boolVal));
                markModified(key);
            }
        }
        else if (value.IsArray() && value.ArraySize() == 3)
        {
            // Vec3 property
            float vec3[3] = {
                value.ArrayAt(0).AsFloat(),
                value.ArrayAt(1).AsFloat(),
                value.ArrayAt(2).AsFloat()
            };
            
            bool changed = false;
            ui.SetNextItemMixedValue(mixedValue);
            
            // Detect color properties (names containing color/diffuse/ambient/specular/emissive)
            if (key.find("color") != std::string::npos || key.find("Color") != std::string::npos ||
                key.find("diffuse") != std::string::npos || key.find("ambient") != std::string::npos ||
                key.find("specular") != std::string::npos || key.find("emissive") != std::string::npos)
            {
                changed = ui.ColorEdit3(displayName.c_str(), vec3);
            }
            else if (key.find("rotation") != std::string::npos || key.find("Rotation") != std::string::npos)
            {
                // Rotation is in radians, convert to degrees for display
                float degrees[3] = {
                    vec3[0] * 57.2957795f,  // rad to deg
                    vec3[1] * 57.2957795f,
                    vec3[2] * 57.2957795f
                };
                if (ui.DragFloat3(displayName.c_str(), degrees, 0.5f))
                {
                    vec3[0] = degrees[0] * 0.0174532925f;  // deg to rad
                    vec3[1] = degrees[1] * 0.0174532925f;
                    vec3[2] = degrees[2] * 0.0174532925f;
                    changed = true;
                }
            }
            else if (key.find("scale") != std::string::npos || key.find("Scale") != std::string::npos)
            {
                changed = ui.DragFloat3(displayName.c_str(), vec3, 0.01f, 0.001f, 1000.f);
            }
            else if (key.find("axis") != std::string::npos || key.find("Axis") != std::string::npos)
            {
                changed = ui.DragFloat3(displayName.c_str(), vec3, 0.01f, -1.f, 1.f);
            }
            else
            {
                // Generic vec3
                changed = ui.DragFloat3(displayName.c_str(), vec3, 0.01f);
            }
            
            if (changed)
            {
                JsonValue newVec = JsonValue::MakeArray()
                    .Push(JsonValue(vec3[0]))
                    .Push(JsonValue(vec3[1]))
                    .Push(JsonValue(vec3[2]));
                editedData.Set(key, newVec);
                markModified(key);
            }
        }
        else if (value.IsArray())
        {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "[%zu items]", value.ArraySize());
            ui.SetNextItemMixedValue(mixedValue);
            ui.ValueLabel(displayName.c_str(), buffer);
        }
        else if (value.IsObject())
        {
            if (value.Has("componentType") && value.Has("expectedType"))
            {
                ComponentReference reference;
                FromJson(value, reference);
                ui.SetNextItemMixedValue(mixedValue);
                if (DrawReferenceProperty(ui, key.c_str(),
                    displayName.c_str(), reference))
                {
                    editedData.Set(key, ToJson(reference));
                    markModified(key);
                }
            }
            else
            {
                ui.SetNextItemMixedValue(mixedValue);
                ui.ValueLabel(displayName.c_str(), "{object}");
            }
        }
    }
    
    if (groupIndented)
        ui.Unindent(12.f);

    // If any property was modified, deserialize the edited data back to the component
    if (modified)
    {
        Deserialize(editedData);
        if (m_multiEditTargets)
        {
            for (Component* target : *m_multiEditTargets)
            {
                if (!target || target == this)
                    continue;
                JsonValue targetData = target->Serialize();
                for (const std::string& key : modifiedKeys)
                    targetData.Set(key, editedData[key]);
                target->Deserialize(targetData);
            }
        }
    }
    return modified;
}

bool Component::DrawPropertiesMulti(::Engine::Editor::IEditorUi& ui,
    const std::vector<Component*>& targets)
{
    m_multiEditTargets = &targets;
    const bool changed = Component::DrawProperties(ui);
    m_multiEditTargets = nullptr;
    return changed;
}

bool Component::IsEditorValueMixed(const std::string& key,
    const JsonValue& value) const
{
    if (!m_multiEditTargets || m_multiEditTargets->size() < 2)
        return false;
    const std::string serialized = Engine::Serialization::JsonWrite(value);
    for (const Component* target : *m_multiEditTargets)
    {
        if (!target)
            return true;
        const JsonValue data = target->Serialize();
        if (!data.Has(key) ||
            Engine::Serialization::JsonWrite(data[key]) != serialized)
            return true;
    }
    return false;
}

Component::JsonValue Component::Serialize() const
{
    JsonValue data = JsonValue::MakeObject();
    data.Set("type", JsonValue(GetTypeName()));
    const JsonValue fields = SerializeFields();
    for (std::size_t i = 0; i < fields.ObjectSize(); ++i)
        data.Set(fields.ObjectKey(i), fields.ObjectValue(i));
    return data;
}

Component::JsonValue Component::SerializeFields() const
{
    JsonValue data = JsonValue::MakeObject();
    for (const auto& field : m_serializedFields)
        data.Set(field.name, field.write());
    return data;
}

void Component::Deserialize(const JsonValue& v)
{
    for (const auto& field : m_serializedFields)
        if (v.Has(field.name))
            field.read(v[field.name]);
    MarkConfigurationDirty();
}

Object* Component::FindObjectInChildrenByName(const std::string& objectName,
    bool includeSelf) const
{
    return Owner
        ? Owner->FindObjectInChildrenByName(objectName, includeSelf)
        : nullptr;
}

Object* Component::FindObjectInSceneByName(const std::string& objectName) const
{
    return Owner ? Owner->FindObjectInSceneByName(objectName) : nullptr;
}

Component* Component::GetComponentOnObjectNamedInScene(
    const std::string& objectName,
    const std::string& componentTypeName) const
{
    Object* object = FindObjectInSceneByName(objectName);
    if (!object)
        return nullptr;
    for (Component* component : object->Components)
    {
        if (component && component->GetTypeName() == componentTypeName)
            return component;
    }
    return nullptr;
}

}
