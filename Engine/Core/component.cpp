#include "pch.h"
#include "Component.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Serialization/Json.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <map>
#include <filesystem>
#include <Windows.h>
#include <commdlg.h>

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

namespace Engine::Core
{

// State tracking for property editing
static std::map<std::string, std::string> s_editingProperty;  // componentPtr+key -> "editing"
// Cache for texture previews
static std::map<std::string, std::shared_ptr<Engine::Components::Texture>> s_texturePreviewCache;

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
    
    // Display interactive controls for each property
    for (size_t i = 0; i < originalData.ObjectSize(); ++i)
    {
        const std::string& key = originalData.ObjectKey(i);
        const JsonValue& value = originalData.ObjectValue(i);
        
        // Skip the "type" field (it's the component name)
        if (key == "type") continue;
        
        // Format key for display (capitalize first letter, add spaces)
        std::string displayName = key;
        if (!displayName.empty())
        {
            displayName[0] = static_cast<char>(toupper(displayName[0]));
        }
        
        // Create appropriate control based on value type
        if (value.IsString())
        {
            const std::string& stringValue = value.AsString();
            
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
                if (!stringValue.empty())
                {
                    std::string ext = stringValue.substr(stringValue.find_last_of('.') + 1);
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    isImageFile = (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || 
                                  ext == "tga" || ext == "dds" || ext == "hdr");
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
                        // Check cache first
                        auto cachedIt = s_texturePreviewCache.find(stringValue);
                        if (cachedIt != s_texturePreviewCache.end())
                        {
                            previewTexture = cachedIt->second;
                        }
                        else
                        {
                            // Load texture from file
                            previewTexture = Engine::Components::Texture::Acquire(stringValue);
                            if (previewTexture && previewTexture->Load())
                            {
                                s_texturePreviewCache[stringValue] = previewTexture;
                            }
                        }
                        
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
                        ofn.lpstrFilter = L"All Files\0*.*\0Images\0*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2\0Models\0*.obj;*.gltf;*.glb;*.fbx\0Audio\0*.wav;*.ogg;*.mp3\0Fonts\0*.ttf;*.otf\0\0";
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
                            modified = true;
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
                            modified = true;
                        }
                    }
                    ui.EndDragDropTarget();
                }

                ui.PopId();
            }
            else
            {
                // Regular string property - make editable
                char buffer[256];
                strncpy_s(buffer, sizeof(buffer), stringValue.c_str(), _TRUNCATE);
                if (ui.InputText(displayName.c_str(), buffer, sizeof(buffer)))
                {
                    editedData.Set(key, JsonValue(std::string(buffer)));
                    modified = true;
                }
            }
        }
        else if (value.IsNumber())
        {
            float floatVal = value.AsFloat();
            
            // Heuristics for appropriate ranges
            if (key == "metallicFactor" || key == "roughnessFactor" ||
                key == "baseColorAlpha" || key == "alphaCutoff" ||
                key == "occlusionStrength")
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.01f, 0.f, 1.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    modified = true;
                }
            }
            else if (key == "normalScale")
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.01f, 0.f, 2.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    modified = true;
                }
            }
            else if (key == "heightScale")
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.001f, 0.f, 0.2f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    modified = true;
                }
            }
            else if (key == "heightMinSteps" || key == "heightMaxSteps")
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 1.f, 4.f, 64.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    modified = true;
                }
            }
            else if (key.find("fov") != std::string::npos || key.find("FOV") != std::string::npos)
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.5f, 1.f, 179.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    modified = true;
                }
            }
            else if (key.find("near") != std::string::npos || key.find("Near") != std::string::npos)
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.001f, 0.001f, 10.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    modified = true;
                }
            }
            else if (key.find("far") != std::string::npos || key.find("Far") != std::string::npos)
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 1.f, 1.f, 10000.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    modified = true;
                }
            }
            else if (key.find("shininess") != std::string::npos)
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.5f, 1.f, 256.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    modified = true;
                }
            }
            else if (key.find("speed") != std::string::npos)
            {
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.5f, -360.f, 360.f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    modified = true;
                }
            }
            else
            {
                // Generic float with reasonable defaults
                if (ui.DragFloat(displayName.c_str(), &floatVal, 0.01f))
                {
                    editedData.Set(key, JsonValue(floatVal));
                    modified = true;
                }
            }
        }
        else if (value.IsBool())
        {
            bool boolVal = value.AsBool();
            if (ui.Checkbox(displayName.c_str(), &boolVal))
            {
                editedData.Set(key, JsonValue(boolVal));
                modified = true;
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
                modified = true;
            }
        }
        else if (value.IsArray())
        {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "[%zu items]", value.ArraySize());
            ui.ValueLabel(displayName.c_str(), buffer);
        }
        else if (value.IsObject())
        {
            if (value.Has("componentType") && value.Has("expectedType"))
            {
                ComponentReference reference;
                FromJson(value, reference);
                const std::string assignedLabel = reference.IsAssigned()
                    ? reference.objectName + " / " + reference.componentType
                    : std::string("(default: ") +
                        (reference.expectedType.empty() ? "component" : reference.expectedType) + ")";
                ui.ValueLabel(displayName.c_str(), assignedLabel.c_str());
                if (ui.BeginDragDropTarget())
                {
                    size_t payloadSize = 0;
                    const void* payload = ui.AcceptDragDropPayload(
                        "ENGINE_COMPONENT_REORDER", &payloadSize);
                    if (payload && payloadSize == sizeof(Component*))
                    {
                        Component* component = *static_cast<Component* const*>(payload);
                        if (component && (reference.expectedType.empty() ||
                            component->GetTypeName() == reference.expectedType))
                        {
                            editedData.Set(key, ToJson(CaptureComponentReference(
                                component, reference.expectedType)));
                            modified = true;
                        }
                    }
                    ui.EndDragDropTarget();
                }
                if (reference.IsAssigned())
                {
                    ui.SameLine();
                    if (ui.Button((std::string("Clear##") + key).c_str()))
                    {
                        reference.Clear();
                        editedData.Set(key, ToJson(reference));
                        modified = true;
                    }
                }
            }
            else
                ui.ValueLabel(displayName.c_str(), "{object}");
        }
    }
    
    // If any property was modified, deserialize the edited data back to the component
    if (modified)
    {
        Deserialize(editedData);
    }
    return modified;
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

}
