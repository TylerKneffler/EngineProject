#include "AssetInspectorTemplate.h"
#include "Engine/Editor/Core/View/Templates/Common/AssetPathTemplate.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Assets/AssetRecord.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Rendering/Sprites/SpriteAnimationAsset.h"
#include "Core/Rendering/Sprites/SpriteSheetAsset.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <pugixml.hpp>

namespace Editor::ViewTemplates
{
namespace
{
std::string XmlDisplayName(const char* rawName)
{
    std::string name = rawName ? rawName : "Property";
    const std::size_t colon = name.find(':');
    if (colon != std::string::npos)
        name.erase(0, colon + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name.erase(0, dot + 1);
    if (!name.empty())
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

bool ParseFloat(const char* value, float& result)
{
    if (!value || !*value)
        return false;
    char* end = nullptr;
    result = std::strtof(value, &end);
    return end && *end == '\0';
}

std::string ImportSettingText(const AssetImportSetting& setting)
{
    if (const auto* value = std::get_if<bool>(&setting))
        return *value ? "true" : "false";
    if (const auto* value = std::get_if<double>(&setting))
        return std::to_string(*value);
    return std::get<std::string>(setting);
}

bool DrawPrefabFields(IEditorUi& ui, pugi::xml_node parent,
    const std::string& prefix)
{
    bool changed = false;
    std::size_t index = 0;
    for (pugi::xml_node node : parent.children())
    {
        const std::string id = prefix + "/" + node.name() + "/" +
            std::to_string(index++);
        const std::string label = XmlDisplayName(node.name());
        ui.PushId(id.c_str());
        const std::size_t children = static_cast<std::size_t>(
            std::distance(node.children().begin(), node.children().end()));
        if (children == 3 && std::string(node.first_child().name()) == "item")
        {
            float values[3]{};
            bool numeric = true;
            std::size_t item = 0;
            for (pugi::xml_node value : node.children("item"))
                numeric = ParseFloat(value.text().as_string(), values[item++]) && numeric;
            if (numeric && ui.DragFloat3(label.c_str(), values, 0.01f))
            {
                item = 0;
                for (pugi::xml_node value : node.children("item"))
                    value.text().set(values[item++]);
                changed = true;
            }
        }
        else if (children > 0)
        {
            if (ui.CollapsingHeader(label.c_str(), false))
                changed = DrawPrefabFields(ui, node, id) || changed;
        }
        else
        {
            const std::string text = node.text().as_string();
            if (text == "true" || text == "false")
            {
                bool value = text == "true";
                if (ui.Checkbox(label.c_str(), &value))
                {
                    node.text().set(value);
                    changed = true;
                }
            }
            else
            {
                float number = 0.f;
                if (ParseFloat(text.c_str(), number))
                {
                    if (ui.DragFloat(label.c_str(), &number, 0.01f))
                    {
                        node.text().set(number);
                        changed = true;
                    }
                }
                else
                {
                    char value[512]{};
                    strncpy_s(value, sizeof(value), text.c_str(), _TRUNCATE);
                    if (ui.InputText(label.c_str(), value, sizeof(value)))
                    {
                        node.text().set(value);
                        changed = true;
                    }
                }
            }
        }
        ui.PopId();
    }
    return changed;
}
}

AssetInspectorTemplate::~AssetInspectorTemplate() = default;

void AssetInspectorTemplate::Select(const std::string& path)
{
    m_selectedPath = path;
    m_renameError.clear();
    m_texturePreview.reset();
    const std::string name = std::filesystem::path(path).filename().string();
    strncpy_s(m_nameEdit, sizeof(m_nameEdit), name.c_str(), _TRUNCATE);
}

void AssetInspectorTemplate::Clear()
{
    m_selectedPath.clear();
    m_renameError.clear();
    m_texturePreview.reset();
}

void AssetInspectorTemplate::Draw(IEditorUi& ui, Scene* scene)
{
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path path(m_selectedPath);
    if (!fs::exists(path, error))
    {
        ui.ColoredLabel("Asset no longer exists", { 1.f, 0.3f, 0.3f, 1.f });
        ui.ValueLabel("Path", m_selectedPath.c_str());
        return;
    }

    const bool directory = fs::is_directory(path, error);
    ui.ColoredLabel(directory ? "Folder Asset" : "File Asset",
        { 0.35f, 0.7f, 1.f, 1.f });
    ui.Separator();
    ui.BeginDisabled(directory);
    ui.InputText("Filename", m_nameEdit, sizeof(m_nameEdit));
    if (ui.Button("Apply Rename"))
    {
        const std::string requested(m_nameEdit);
        const bool invalid = requested.empty() || requested == "." || requested == ".." ||
            requested.find_first_of("<>:\"/\\|?*") != std::string::npos;
        if (invalid)
            m_renameError = "Filename is empty or contains invalid characters.";
        else
        {
            const fs::path destination = path.parent_path() / requested;
            if (fs::exists(destination, error) && destination != path)
                m_renameError = "An asset with that filename already exists.";
            else
            {
                const std::string oldPath = m_selectedPath;
                fs::rename(path, destination, error);
                if (error)
                    m_renameError = "Rename failed: " + error.message();
                else
                {
                    std::error_code recordError;
                    if (!AssetRecord::Move(path, destination, recordError))
                    {
                        std::error_code rollbackError;
                        fs::rename(destination, path, rollbackError);
                        m_renameError = "Asset record rename failed: " +
                            recordError.message();
                        ui.EndDisabled();
                        return;
                    }
                    m_selectedPath = destination.string();
                    m_renameError.clear();
                    m_texturePreview.reset();
                    if (OnRenamed)
                        OnRenamed(oldPath, m_selectedPath);
                }
            }
        }
    }
    ui.EndDisabled();
    if (directory)
        ui.DisabledLabel("Folder renaming is disabled here to protect asset paths.");
    if (!m_renameError.empty())
        ui.ColoredLabel(m_renameError.c_str(), { 1.f, 0.35f, 0.35f, 1.f });

    ui.ValueLabel("Path", m_selectedPath.c_str());
    const std::string extension = LowerAssetExtension(m_selectedPath);
    ui.ValueLabel("Type", directory ? "Folder" :
        (extension.empty() ? "Generic file" : extension.c_str()));
    if (directory)
    {
        std::size_t entries = 0;
        for (fs::directory_iterator iterator(m_selectedPath, error), end;
            !error && iterator != end; iterator.increment(error))
            if (iterator->path().extension() != ".meta")
                ++entries;
        const std::string value = std::to_string(entries);
        ui.ValueLabel("Entries", value.c_str());
        return;
    }

    const auto bytes = fs::file_size(m_selectedPath, error);
    if (!error)
    {
        const std::string size = std::to_string(bytes) + " bytes";
        ui.ValueLabel("Size", size.c_str());
    }

    try
    {
        if (const auto record = AssetRecord::Load(m_selectedPath))
        {
            ui.Separator();
            ui.Label("Asset Record");
            ui.ValueLabel("Stable ID", record->id.c_str());
            ui.ValueLabel("Source", record->sourcePath.c_str());
            for (const auto& [name, setting] : record->importSettings)
            {
                const std::string value = ImportSettingText(setting);
                ui.ValueLabel(name.c_str(), value.c_str());
            }
        }
    }
    catch (const std::exception& exception)
    {
        const std::string message = "Invalid asset record: " +
            std::string(exception.what());
        ui.ColoredLabel(message.c_str(), { 1.f, 0.35f, 0.35f, 1.f });
    }

    if (IsTextureAssetExtension(extension))
    {
        if (!m_texturePreview)
            m_texturePreview = Texture::Acquire(m_selectedPath, true);
        IGraphicsProvider* graphics = scene ? scene->GetGraphicsProvider() : nullptr;
        if (m_texturePreview && graphics)
            m_texturePreview->Prepare(graphics);
        void* handle = m_texturePreview && m_texturePreview->GetGraphicsTexture()
            ? m_texturePreview->GetGraphicsTexture()->GetNativeHandle() : nullptr;
        if (handle)
        {
            const float aspect = m_texturePreview->GetHeight() > 0
                ? static_cast<float>(m_texturePreview->GetWidth()) /
                    static_cast<float>(m_texturePreview->GetHeight()) : 1.f;
            ui.DrawImage(handle, 192.f, 192.f / std::max(aspect, 0.1f));
            const std::string dimensions = std::to_string(m_texturePreview->GetWidth()) +
                " x " + std::to_string(m_texturePreview->GetHeight());
            ui.ValueLabel("Dimensions", dimensions.c_str());
        }
        return;
    }

    if (extension == ".spriteanim")
    {
        SpriteAnimationAsset asset;
        if (!asset.Load(m_selectedPath))
        {
            ui.ColoredLabel("Invalid sprite animation asset", { 1.f, 0.35f, 0.35f, 1.f });
            return;
        }
        ui.Separator();
        ui.Label("Sprite Animation");
        char sheet[512]{};
        strncpy_s(sheet, sizeof(sheet), asset.spriteSheetFile.c_str(), _TRUNCATE);
        if (ui.InputText("Sprite Sheet", sheet, sizeof(sheet)))
        {
            asset.spriteSheetFile = sheet;
            asset.Save();
            if (OnContentsChanged) OnContentsChanged(m_selectedPath);
        }
        char animation[256]{};
        strncpy_s(animation, sizeof(animation), asset.animation.c_str(), _TRUNCATE);
        if (ui.InputText("Animation", animation, sizeof(animation)))
        {
            asset.animation = animation;
            asset.Save();
            if (OnContentsChanged) OnContentsChanged(m_selectedPath);
        }
        if (ui.DragFloat("Speed", &asset.speed, 0.05f, 0.f, 20.f))
        {
            asset.Save();
            if (OnContentsChanged) OnContentsChanged(m_selectedPath);
        }
        if (ui.Checkbox("Loop", &asset.loop))
        {
            asset.Save();
            if (OnContentsChanged) OnContentsChanged(m_selectedPath);
        }
        if (ui.Checkbox("Autoplay", &asset.autoplay))
        {
            asset.Save();
            if (OnContentsChanged) OnContentsChanged(m_selectedPath);
        }
        return;
    }

    if (extension == ".spritesheet")
    {
        SpriteSheetAsset sheet;
        if (!sheet.Load(m_selectedPath))
        {
            ui.ColoredLabel("Invalid sprite sheet asset", { 1.f, 0.35f, 0.35f, 1.f });
            return;
        }
        std::size_t frames = 0;
        for (const SpriteSheetAnimation& animation : sheet.GetAnimations())
            frames += animation.frames.size();
        const std::string animations = std::to_string(sheet.GetAnimations().size());
        const std::string frameCount = std::to_string(frames);
        ui.ValueLabel("Animations", animations.c_str());
        ui.ValueLabel("Frames", frameCount.c_str());
        return;
    }

    if (extension == ".prefab")
    {
        pugi::xml_document document;
        if (!document.load_file(m_selectedPath.c_str()))
        {
            ui.ColoredLabel("Invalid prefab asset", { 1.f, 0.35f, 0.35f, 1.f });
            return;
        }
        pugi::xml_node root = document.child("Prefab").child("object");
        if (!root)
            return;
        ui.Separator();
        ui.Label("Prefab Defaults");
        const bool prefabChanged = DrawPrefabFields(ui, root, "Prefab");
        std::size_t components = 0;
        for (pugi::xml_node ignored : root.child("components").children())
        {
            (void)ignored;
            ++components;
        }
        const std::string componentCount = std::to_string(components);
        ui.ValueLabel("Components", componentCount.c_str());
        if (prefabChanged && document.save_file(m_selectedPath.c_str(), "  "))
            if (OnContentsChanged)
                OnContentsChanged(m_selectedPath);
    }
}
}
