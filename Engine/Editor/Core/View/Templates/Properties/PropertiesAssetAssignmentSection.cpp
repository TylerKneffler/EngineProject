#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Engine/Editor/Core/View/Templates/Common/AssetPathTemplate.h"
#include "Engine/Editor/Core/View/Templates/Properties/PropertiesViewTemplateHelpers.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Engine/Editor/Core/Importers/MmdDanceAssetBinder.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Model/AnimationPackAsset.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Model/RigAsset.h"
#include "Core/Compoonents/Audio/AudioSource.h"
#include "Core/Compoonents/UI/UIText.h"
#include "Core/Compoonents/UI/UIImage.h"
#include "Core/Compoonents/UI/UIObject.h"
#include "Core/Compoonents/UI/Canvas.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <filesystem>
#include <memory>

namespace Engine::Editor
{
bool PropertiesView::AssignAssetToObject(Engine::Core::Object& object,
    const std::string& path, std::string& message)
{
    PropertiesView assignment;
    assignment.m_scene = object.GetScene();
    assignment.m_selectedObject = &object;
    if (!assignment.AddComponentFromAsset(path, message))
        return false;
    object.NotifyStructureChanged();
    object.InvalidatePrefabOverrideCache();
    return true;
}

namespace
{
bool IsUiContext(const Engine::Core::Object* object)
{
    for (const Engine::Core::Object* current = object; current; current = current->Parent)
        if (current->GetComponent<Engine::Components::Canvas>() ||
            current->GetComponent<Engine::Components::UIObject>())
            return true;
    return false;
}
}

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
        if (added)
        {
            m_selectedObject->NotifyStructureChanged();
            m_selectedObject->InvalidatePrefabOverrideCache();
            if (OnComponentsChanged)
                OnComponentsChanged();
        }
        return {};
    }

    const std::string extension = Engine::Editor::LowerAssetExtension(path);
    if (extension == ".mesh" || extension == ".obj")
        return "Mesh Preview";
    if (Engine::Editor::IsTextureAssetExtension(extension) &&
        IsUiContext(m_selectedObject))
        return "UI Image Preview";
    if (extension == ".material" || extension == ".mat" ||
        Engine::Editor::IsTextureAssetExtension(extension))
        return "Material Preview";
    if (extension == ".spriteanim")
        return "Sprite Animation Preview";
    if (extension == ".animpack")
        return "Animation Pack for AnimationManager";
    if (extension == ".vmd")
        return "Bind VMD motion to character";
    if (extension == ".anim" || extension == ".animation")
        return "Animation for AnimationManager";
    if (extension == ".skeleton")
        return "Skeleton Preview";
    if (Engine::Editor::IsAudioAssetExtension(extension))
        return "Audio Source Preview";
    if (Engine::Editor::IsFontAssetExtension(extension))
        return "UI Text Font Preview";
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
    if (path.empty() || !std::filesystem::is_regular_file(path))
    {
        message = "[Properties] Dropped asset does not exist: " + path;
        return false;
    }

    const std::string extension = Engine::Editor::LowerAssetExtension(path);
    try
    {
        if (extension == ".skeleton")
        {
            const auto rig = Engine::Model::RigAsset::Load(path);
            std::string error;
            if (!rig || !rig->ApplySkeleton(*m_selectedObject, path, error))
            { message = "[Properties] Could not attach skeleton: " + error; return false; }
            message = "[Properties] Assigned skeleton from: " + path;
            return true;
        }
        if (extension == ".anim" || extension == ".animation")
        {
            if (!m_selectedObject->GetComponentInParent<Engine::Components::Model>())
            { message = "[Properties] Attach a skeleton or Model hierarchy before adding an animation."; return false; }
            auto* manager = m_selectedObject->GetComponent<Engine::Components::AnimationManager>();
            std::unique_ptr<Engine::Components::AnimationManager> newManager;
            if (!manager)
            {
                newManager = std::make_unique<Engine::Components::AnimationManager>();
                newManager->Owner = m_selectedObject;
                manager = newManager.get();
            }
            std::string error;
            if (!manager->AddAnimationAsset(path, error))
            { message = "[Properties] " + error; return false; }
            if (newManager)
                m_selectedObject->Components.push_back(newManager.release());
            manager->EndEditorPreview();
            message = "[Properties] Added animation to AnimationManager from: " + path;
            return true;
        }
        if (extension == ".animpack" || extension == ".vmd")
        {
            std::string error;
            std::optional<Engine::Model::AnimationPackAsset> pack;
            if (extension == ".animpack") pack = Engine::Model::AnimationPackAsset::Load(path, error);
            if (extension == ".animpack" && !pack)
            { message = "[Properties] Failed to read animation pack: " + error; return false; }
            auto* model = m_selectedObject->GetComponentInParent<Engine::Components::Model>();
            if (!model)
            { message = "[Properties] Select a character Model or one of its bones before applying motion."; return false; }
            if (extension == ".vmd" || pack->clips.empty())
            {
                auto* root = model->Owner->GetPrefabInstanceRoot();
                if (!root || !root->Prefab)
                { message = "[Properties] VMD binding needs an imported character prefab with source model metadata."; return false; }
                Engine::Model::AnimationPackAsset bound;
                if (!MmdDanceAssetBinder::BuildForPrefab(root->Prefab->GetPath(), path, bound, error))
                { message = "[Properties] Could not bind VMD motion: " + error; return false; }
                pack = std::move(bound);
            }
            auto* manager = model->Owner->GetComponent<Engine::Components::AnimationManager>();
            std::unique_ptr<Engine::Components::AnimationManager> newManager;
            if (!manager)
            {
                newManager = std::make_unique<Engine::Components::AnimationManager>();
                newManager->Owner = model->Owner;
                manager = newManager.get();
            }
            if (!pack->Apply(*manager, error))
            { message = "[Properties] Could not apply animation pack: " + error; return false; }
            if (newManager)
                model->Owner->Components.push_back(newManager.release());
            manager->EndEditorPreview();
            message = "[Properties] Applied animation pack from: " + path;
            return true;
        }

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
            if (auto* image = m_selectedObject->GetComponent<Engine::Components::UIImage>())
            {
                image->sourcePath = path;
                image->MarkConfigurationDirty();
                message = "[Properties] Assigned UI image from: " + path;
                return true;
            }
            if (IsUiContext(m_selectedObject))
            {
                if (!m_selectedObject->GetComponent<Engine::Components::UIObject>())
                    m_selectedObject->AddComponent<Engine::Components::UIObject>();
                auto* image = m_selectedObject->AddComponent<Engine::Components::UIImage>();
                image->sourcePath = path;
                image->MarkConfigurationDirty();
                message = "[Properties] Added UI image from: " + path;
                return true;
            }
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

        if (Engine::Editor::IsFontAssetExtension(extension))
        {
            Engine::Components::UIText* text =
                m_selectedObject->GetComponent<Engine::Components::UIText>();
            if (!text)
                text = m_selectedObject->AddComponent<Engine::Components::UIText>();
            if (!m_selectedObject->GetComponent<Engine::Components::UIObject>())
                m_selectedObject->AddComponent<Engine::Components::UIObject>();
            text->fontPath = path;
            message = "[Properties] Assigned UI font from: " + path;
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
