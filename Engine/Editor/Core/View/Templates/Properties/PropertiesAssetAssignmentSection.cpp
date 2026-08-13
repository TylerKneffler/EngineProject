#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Engine/Editor/Core/View/Templates/Common/AssetPathTemplate.h"
#include "Engine/Editor/Core/View/Templates/Properties/PropertiesViewTemplateHelpers.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Sprite.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/Compoonents/AudioSource.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <filesystem>
#include <memory>

namespace Engine::Editor
{

std::string PropertiesView::HandleWindowAssetDrop(IEditorUi& ui)
{
    const EditorUiDragDropPayloadResult payload =
        ui.WindowDragDropTarget("ENGINE_ASSET_PATH");
    if (!payload.data || payload.size == 0)
        return {};

    const char* bytes = static_cast<const char*>(payload.data);
    size_t length = 0;
    while (length < payload.size && bytes[length] != '\0')
        ++length;
    const std::string path(bytes, length);
    if (path.empty())
        return {};

    if (payload.delivered)
    {
        std::string message;
        const bool added = AddComponentFromAsset(path, message);
        LogAssetDrop(message, !added);
        if (added && OnComponentsChanged)
            OnComponentsChanged();
        return {};
    }

    const std::string extension = Engine::Editor::LowerAssetExtension(path);
    if (extension == ".mesh" || extension == ".obj")
        return "Mesh Preview";
    if (extension == ".material" || extension == ".mat" ||
        Engine::Editor::IsTextureAssetExtension(extension))
        return "Material Preview";
    if (extension == ".spriteanim")
        return "Sprite Animation Preview";
    if (Engine::Editor::IsAudioAssetExtension(extension))
        return "Audio Source Preview";
    if (extension == ".spritesheet")
        return "Sprite Sheet Preview";
    if (extension == ".h" || extension == ".hpp")
    {
        const std::string className = FindComponentSubclass(path);
        return (className.empty()
            ? std::filesystem::path(path).stem().string()
            : className) + " Preview";
    }
    return std::filesystem::path(path).filename().string() + " Preview";
}

bool PropertiesView::AddComponentFromAsset(const std::string& path, std::string& message)
{
    if (!m_selectedObject)
    {
        message = "[Properties] Select an object before dropping an asset.";
        return false;
    }
    if (m_selectedObject->IsPartOfPrefabInstance())
    {
        message = "[Properties] Prefab instance content is read-only. Open the prefab asset or unpack the instance first.";
        return false;
    }
    if (path.empty() || !std::filesystem::is_regular_file(path))
    {
        message = "[Properties] Dropped asset does not exist: " + path;
        return false;
    }

    const std::string extension = Engine::Editor::LowerAssetExtension(path);
    try
    {
        if (extension == ".mesh" || extension == ".obj")
        {
            auto mesh = std::make_unique<Engine::Components::Mesh>();
            mesh->LoadFromFile(path);
            mesh->OnAfterDeserialize(m_scene ? m_scene->GetGraphicsProvider() : nullptr);
            ReplaceOrAddComponent(*m_selectedObject, mesh.release());
            message = "[Properties] Assigned Mesh from: " + path;
            return true;
        }

        if (extension == ".material" || extension == ".mat")
        {
            auto material = std::make_unique<Engine::Components::Material>();
            if (!material->LoadFromFile(path))
            {
                message = "[Properties] Failed to read material asset: " + path;
                return false;
            }
            material->PrepareTextures(m_scene ? m_scene->GetGraphicsProvider() : nullptr);
            ReplaceOrAddComponent(*m_selectedObject, material.release());
            message = "[Properties] Assigned Material from: " + path;
            return true;
        }

        if (extension == ".spriteanim")
        {
            auto manager = std::make_unique<Engine::Components::SpriteAnimationManager>();
            if (!manager->LoadFromFile(path))
            {
                message = "[Properties] Failed to read sprite animation asset: " + path;
                return false;
            }
            manager->OnAfterDeserialize(
                m_scene ? m_scene->GetGraphicsProvider() : nullptr);
            ReplaceOrAddComponent(*m_selectedObject, manager.release());
            Engine::Components::Sprite* sprite = m_selectedObject->GetComponent<Engine::Components::Sprite>();
            if (!sprite)
                sprite = m_selectedObject->AddComponent<Engine::Components::Sprite>();
            sprite->SetAnimationManager(
                m_selectedObject->GetComponent<Engine::Components::SpriteAnimationManager>());
            sprite->Prepare(m_scene ? m_scene->GetGraphicsProvider() : nullptr);
            message = "[Properties] Assigned Engine::Components::Sprite Animation from: " + path;
            return true;
        }

        if (extension == ".spritesheet")
        {
            message = "[Properties] A .spritesheet stores frames. Drop a .spriteanim asset to add playback.";
            return false;
        }

        if (Engine::Editor::IsTextureAssetExtension(extension))
        {
            Engine::Components::Material* material = m_selectedObject->GetComponent<Engine::Components::Material>();
            if (!material)
                material = m_selectedObject->AddComponent<Engine::Components::Material>();
            material->SetBaseColorTexture(path);
            material->PrepareTextures(m_scene ? m_scene->GetGraphicsProvider() : nullptr);
            message = "[Properties] Assigned texture as Material base color: " + path;
            return true;
        }

        if (Engine::Editor::IsAudioAssetExtension(extension))
        {
            Engine::Components::AudioSource* source = m_selectedObject->GetComponent<Engine::Components::AudioSource>();
            if (!source)
                source = m_selectedObject->AddComponent<Engine::Components::AudioSource>();
            source->audioPath = path;
            message = "[Properties] Assigned Audio Source from: " + path;
            return true;
        }

        if (extension == ".h" || extension == ".hpp")
        {
            const std::string className = FindComponentSubclass(path);
            if (className.empty())
            {
                message = "[Properties] Header does not declare a public Component/Script subclass: " + path;
                return false;
            }

            if (!AddRegisteredComponent(className, message))
                return false;
            message += " from: " + path;
            return true;
        }
    }
    catch (const std::exception& error)
    {
        message = "[Properties] Could not apply asset '" + path + "': " + error.what();
        return false;
    }

    message = "[Properties] No compatible component mapping for asset: " + path;
    return false;
}
}
