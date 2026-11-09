#include "pch.h"
#include "EditorState.h"
#include "Engine/Editor/Diagnostics/FrameActivity.h"
#include "Core/SceneManager.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/IEditorPanel.h"
#include "Core/View/View.h"
#include "Core/View/Views/ConsoleView.h"
#include "Core/View/Views/PropertiesView.h"
#include "Core/View/Views/HierarchyView.h"
#include "Core/View/Views/SceneView.h"
#include "Core/View/Views/AssetsExplorerView.h"
#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/Physics/Physics.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/Serialization/PreparedPrefab.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/AssetRecord.h"
#include "Core/Importers/ModelImporter.h"
#include "Core/Importers/VmdSceneImporter.h"
#include "Core/Importers/MmdDanceAssetBinder.h"
#include "Core/Importers/PortableAssetCopy.h"
#include "Core/Threading/JobScheduler.h"
#include <commdlg.h>

#include "EditorStateSupport.h"

namespace Engine::Editor
{
using namespace StateSupport;

void EditorState::ImportAsset()
{
    FrameActivity::Mark(FrameActivity::Import);
    wchar_t source[MAX_PATH] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = m_window ? m_window->GetHWND() : nullptr;
    dialog.lpstrFile = source;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter =
        L"Supported Assets (*.obj;*.gltf;*.glb;*.fbx;*.pmx;*.vmd;*.prefab;*.animpack;*.spriteanim;*.spritesheet;*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2;*.wav;*.ogg;*.mp3;*.ttf;*.otf)\0"
        L"*.obj;*.gltf;*.glb;*.fbx;*.pmx;*.vmd;*.prefab;*.animpack;*.spriteanim;*.spritesheet;*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2;*.wav;*.ogg;*.mp3;*.ttf;*.otf\0"
        L"3D Models (*.obj;*.gltf;*.glb;*.fbx;*.pmx)\0*.obj;*.gltf;*.glb;*.fbx;*.pmx\0"
        L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2)\0*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2\0"
        L"Audio (*.wav;*.ogg;*.mp3)\0*.wav;*.ogg;*.mp3\0"
        L"Fonts (*.ttf;*.otf)\0*.ttf;*.otf\0"
        L"All Files (*.*)\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog))
    {
        const std::string path = std::filesystem::path(source).string();
        std::string extension = std::filesystem::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension == ".vmd" || extension == ".fbx" || extension == ".pmx" ||
            extension == ".gltf" || extension == ".glb" ||
            extension == ".obj" || extension == ".spriteanim" ||
            extension == ".prefab")
            StartObjectImport(path, false);
        else StartObjectImport(path, false);
    }
}

void EditorState::StartObjectImport(const std::string& path, bool intoScene,
    const std::string& destinationDirectory)
{
    if (m_importJob.valid())
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Finish or cancel the current asset import first.");
        return;
    }
    const std::filesystem::path assetsRoot = m_projectSettings.assetsDirectory.empty()
        ? std::filesystem::path("Assets")
        : std::filesystem::path(m_projectSettings.assetsDirectory);
    std::error_code directoryError;
    std::filesystem::create_directories(assetsRoot, directoryError);
    m_pendingImportAssetPath = path;
    m_pendingImportDirectory = !destinationDirectory.empty()
        ? destinationDirectory : (m_primaryAssets &&
        !m_primaryAssets->GetCurrentDirectory().empty()
        ? m_primaryAssets->GetCurrentDirectory()
        : (m_projectSettings.assetsDirectory.empty()
            ? std::string("Assets") : m_projectSettings.assetsDirectory));
    m_pendingImportScene = GetActiveDocumentScene();
    m_pendingImportIntoScene = intoScene;
    m_pendingImportAsPrefab = false;
    m_pendingPmxUnitScaleToMeters = 1.f;
    m_pendingPmxImportMorphs = true;
    m_pendingPmxImportRigConstraints = true;
    m_pendingPmxImportFollowBoneColliders = true;
    m_pendingPmxImportDynamicPhysics = false;
    m_pendingPmxPhysicsQuality = 1;
    m_pendingPmxWeightFittedColliders = false;
    m_pendingPmxColliderMode = 1;
    ClearImportReference();
    if (m_pendingImportScene)
    {
        auto* selected = m_pendingImportScene->GetSelectedObject();
        auto* root = selected ? selected->GetPrefabInstanceRoot() : nullptr;
        if (selected && (!PendingImportSupportsReference() ||
            (intoScene && root && root->GetComponent<Engine::Components::Model>())))
        {
            m_pickingImportReference = true;
            CaptureImportReference(selected);
        }
    }
}

bool EditorState::PendingImportSupportsReference() const
{
    std::string extension = std::filesystem::path(m_pendingImportAssetPath).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension == ".vmd";
}

bool EditorState::IsPendingPmxImport() const
{
    std::string extension = std::filesystem::path(m_pendingImportAssetPath).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension == ".pmx";
}

void EditorState::BeginImportReferencePick()
{
    if (m_pendingImportAssetPath.empty()) return;
    if (!m_pendingImportScene || !m_primaryHierarchy)
    { m_pendingImportError = "Open a scene with a Hierarchy panel to choose an object."; return; }
    m_pickingImportReference = true;
    m_pendingImportError.clear();
    // Require a fresh click, even when the desired object was already selected.
    m_primaryHierarchy->SetSelectedObject(nullptr);
    m_primaryHierarchy->SetOpen(true);
    m_focusImportHierarchy = true;
}

void EditorState::CaptureImportReference(Engine::Core::Object* object)
{
    if (!m_pickingImportReference || !object) return;
    Engine::Scene::Scene* scene = GetActiveDocumentScene();
    Engine::Scene::Scene::ObjectPath path;
    if (!scene || scene != m_pendingImportScene ||
        !scene->TryGetObjectPath(object, path)) return;
    m_pendingImportReferencePath = std::move(path);
    m_pendingImportReferenceObject = object;
    m_hasImportReference = true;
    m_pickingImportReference = false;
    m_pendingImportError.clear();
    std::vector<std::string> names;
    for (auto* node = object; node; node = node->Parent)
        names.push_back(node->name.empty() ? "(unnamed)" : node->name);
    m_pendingImportReferenceLabel.clear();
    for (auto it = names.rbegin(); it != names.rend(); ++it)
    {
        if (!m_pendingImportReferenceLabel.empty())
            m_pendingImportReferenceLabel += " / ";
        m_pendingImportReferenceLabel += *it;
    }
}

void EditorState::ClearImportReference()
{
    m_pendingImportReferencePath.clear();
    m_pendingImportReferenceObject = nullptr;
    m_pendingImportReferenceLabel.clear();
    m_hasImportReference = false;
    m_pickingImportReference = false;
    m_pendingImportError.clear();
}

void EditorState::CancelPendingImportAsset()
{
    if (m_importJob.valid() && m_importJobControl)
        m_importJobControl->cancelled.store(true, std::memory_order_release);
    m_pendingImportAssetPath.clear();
    m_pendingImportDirectory.clear();
    m_pendingImportScene = nullptr;
    m_pendingImportIntoScene = false;
    m_pendingImportAsPrefab = false;
    m_pendingPmxUnitScaleToMeters = 1.f;
    m_pendingPmxImportMorphs = true;
    m_pendingPmxImportRigConstraints = true;
    m_pendingPmxImportFollowBoneColliders = true;
    m_pendingPmxImportDynamicPhysics = false;
    m_pendingPmxPhysicsQuality = 1;
    m_pendingPmxWeightFittedColliders = false;
    m_pendingPmxColliderMode = 1;
    m_focusImportHierarchy = false;
    ClearImportReference();
}

std::string EditorState::ConsumeImportHierarchyFocusRequest()
{
    if (!m_focusImportHierarchy || !m_primaryHierarchy) return {};
    m_focusImportHierarchy = false;
    return m_primaryHierarchy->GetTitle();
}

void EditorState::ConfirmPendingImportAsset()
{
    FrameActivity::Mark(FrameActivity::Import);
    if (m_pendingImportAssetPath.empty()) return;
    if (m_importJob.valid()) return;
    if (IsPendingPmxImport() && (!std::isfinite(m_pendingPmxUnitScaleToMeters) ||
        m_pendingPmxUnitScaleToMeters <= 0.f ||
        m_pendingPmxUnitScaleToMeters > 1000.f))
    { m_pendingImportError = "PMX unit scale must be greater than zero and at most 1000."; return; }
    namespace fs = std::filesystem;
    std::error_code pathError;
    const fs::path assetRoot = fs::weakly_canonical(
        m_projectSettings.assetsDirectory.empty()
            ? fs::path("Assets") : fs::path(m_projectSettings.assetsDirectory),
        pathError);
    const fs::path destination = fs::weakly_canonical(
        m_pendingImportDirectory, pathError);
    const fs::path relative = destination.lexically_relative(assetRoot);
    if (pathError || !fs::is_directory(destination) || relative.empty() ||
        *relative.begin() == "..")
    { m_pendingImportError = "Choose an existing directory inside Assets."; return; }
    if (m_pendingImportIntoScene &&
        (!m_pendingImportScene || m_pendingImportScene != GetActiveDocumentScene()))
    { m_pendingImportError = "The target scene changed. Reopen the import tool."; return; }
    Engine::Core::Object* reference = nullptr;
    if (m_pendingImportIntoScene && m_hasImportReference)
    {
        if (!m_pendingImportScene || m_pendingImportScene != GetActiveDocumentScene())
        { m_pendingImportError = "The target scene changed. Pick an object again."; return; }
        reference = m_pendingImportScene->FindObjectByPath(m_pendingImportReferencePath);
        if (!reference || reference != m_pendingImportReferenceObject)
        { m_pendingImportError = "The selected object changed. Pick an object again."; return; }
        if (PendingImportSupportsReference())
        {
            auto* root = reference->GetPrefabInstanceRoot();
            if (!root || !root->GetComponent<Engine::Components::Model>())
            { m_pendingImportError = "Choose an imported character prefab instance."; return; }
        }
    }
    const std::string source = m_pendingImportAssetPath;
    const bool asPrefab = m_pendingImportAsPrefab;
    m_pendingImportError.clear();
    std::string extension = fs::path(source).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    std::string bindPrefab;
    if (extension == ".vmd" && reference)
        if (auto* root = reference->GetPrefabInstanceRoot(); root && root->Prefab)
            bindPrefab = root->Prefab->GetPath();
    const PmxImportOptions options{
        m_pendingPmxImportMorphs, m_pendingPmxImportRigConstraints,
        m_pendingPmxImportFollowBoneColliders, m_pendingPmxImportDynamicPhysics,
        m_pendingPmxWeightFittedColliders,
        m_pendingPmxPhysicsQuality <= 0 ? PmxPhysicsQuality::High :
            m_pendingPmxPhysicsQuality == 1 ? PmxPhysicsQuality::Medium :
            PmxPhysicsQuality::Low,
        m_pendingPmxColliderMode <= 0 ? PmxColliderMode::Hull :
            m_pendingPmxColliderMode == 1 ? PmxColliderMode::Primitive :
            PmxColliderMode::None };
    m_importSceneGeneration = m_sceneDocumentGeneration;
    m_importDestinationRoot = destination;
    m_importReferencePath = m_pendingImportReferencePath;
    m_importReferenceObject = reference;
    m_importJobControl = std::make_shared<AssetJobControl>();
    const auto control = m_importJobControl;
    const float unitScale = m_pendingPmxUnitScaleToMeters;
    const fs::path projectAssetsRoot = m_projectSettings.assetsDirectory.empty()
        ? fs::path("Assets") : fs::path(m_projectSettings.assetsDirectory);
    m_importJob = Engine::Threading::JobScheduler::Get().Submit(
        [source, destination, projectAssetsRoot, extension, asPrefab, bindPrefab, options,
            unitScale, control]() -> ImportJobResult
        {
            AssetJobScope scope(control.get());
            ImportJobResult result;
            fs::path createdCopy;
            fs::path createdPrefab;
            try
            {
                AssetJobCheckpoint(0.01f);
                const fs::path input(source);
                if (ModelImporter::SupportsExtension(extension))
                {
                    auto imported = extension == ".vmd" && asPrefab
                        ? VmdSceneImporter::Import(source,
                            destination.string(), true)
                        : ModelImporter::Import(source, destination.string(),
                            unitScale, options);
                    if (!imported.success)
                        throw std::runtime_error(imported.message);
                    result.primary = asPrefab && !imported.prefabPath.empty()
                        ? imported.prefabPath : imported.primaryAssetPath.empty()
                        ? imported.prefabPath : imported.primaryAssetPath;
                    result.outputDirectory = imported.outputDirectory;
                    result.message = imported.message;
                    AssetJobCheckpoint(0.9f);
                    if (!bindPrefab.empty())
                    {
                        AssetJobCheckpoint();
                        control->committing.store(true, std::memory_order_release);
                        result.boundPrefab = bindPrefab;
                        result.boundCharacter = MmdDanceAssetBinder::BindFromPrefab(
                            bindPrefab, source, result.bindingError,
                            (fs::path(imported.outputDirectory) /
                                (input.stem().string() + ".animpack")).string());
                    }
                }
                else
                {
                    const bool portable = extension == ".obj" ||
                        extension == ".spriteanim";
                    std::error_code pathError;
                    const fs::path absoluteSource = fs::weakly_canonical(input, pathError);
                    const fs::path absoluteAssets = fs::weakly_canonical(destination, pathError);
                    if (!pathError)
                    {
                        const fs::path relative = absoluteSource.lexically_relative(absoluteAssets);
                        if (!relative.empty() && *relative.begin() != "..")
                        {
                            Engine::Core::AssetRecord::Ensure(input, input,
                                { { "importer", std::string("native") } });
                            result.primary = source;
                        }
                    }
                    if (result.primary.empty())
                    {
                        fs::path output = destination / input.filename();
                        const std::string stem = output.stem().string();
                        const std::string suffix = output.extension().string();
                        for (unsigned index = 2; fs::exists(output) ||
                            (portable && fs::exists(output.parent_path() /
                                (output.stem().string() + "_dependencies"))); ++index)
                            output = destination /
                                (stem + " " + std::to_string(index) + suffix);
                        AssetJobCheckpoint(0.1f);
                        if (portable) CopyPortableAsset(input, output);
                        else AssetJobCopyFile(input, output);
                        createdCopy = output;
                        result.createdPaths.push_back(output);
                        AssetJobCheckpoint(0.9f);
                        Engine::Core::AssetRecord::Ensure(output, input,
                            { { "importer", std::string("copy") } });
                        result.primary = output.string();
                    }
                    if (asPrefab && portable)
                    {
                        AssetJobCheckpoint(0.93f);
                        const fs::path assetsRoot = fs::weakly_canonical(
                            projectAssetsRoot);
                        const fs::path importedPath = fs::weakly_canonical(
                            result.primary);
                        const fs::path withinAssets = importedPath.lexically_relative(
                            assetsRoot);
                        if (withinAssets.empty() || *withinAssets.begin() == "..")
                            throw std::runtime_error("Imported asset is outside Assets");
                        const std::string portablePath =
                            (assetsRoot.filename() / withinAssets).generic_string();
                        Engine::Scene::Scene prefabScene;
                        Engine::Core::Object* object = prefabScene.AddObject(
                            importedPath.stem().string());
                        if (extension == ".obj")
                        {
                            auto* mesh = object->AddComponent<
                                Engine::Components::Mesh>();
                            mesh->LoadFromFile(result.primary);
                            mesh->SetAuthoredFilePath(portablePath);
                            object->AddComponent<Engine::Components::Material>();
                        }
                        else
                        {
                            auto* manager = object->AddComponent<
                                Engine::Components::SpriteAnimationManager>();
                            auto* sprite = object->AddComponent<
                                Engine::Components::Sprite>();
                            sprite->SetAnimationManager(manager);
                            if (!manager->LoadFromFile(result.primary))
                                throw std::runtime_error(
                                    "Could not load sprite animation");
                            manager->animationFile = portablePath;
                        }
                        fs::path prefab = importedPath;
                        prefab.replace_extension(".prefab");
                        const std::string stem = prefab.stem().string();
                        for (unsigned index = 2; fs::exists(prefab); ++index)
                            prefab = prefab.parent_path() /
                                (stem + " " + std::to_string(index) + ".prefab");
                        createdPrefab = prefab;
                        result.createdPaths.push_back(prefab);
                        AssetJobCheckpoint(0.97f);
                        if (!Engine::Serialization::SceneSerializer::SavePrefab(
                                *object, prefab.string()))
                            throw std::runtime_error(
                                "Could not save imported object prefab");
                        Engine::Core::AssetRecord::Ensure(prefab, result.primary,
                            { { "importer", std::string("object-prefab") } });
                        result.primary = prefab.string();
                    }
                }
                if (control->committing.load(std::memory_order_acquire))
                    control->SetProgress(1.f);
                else AssetJobCheckpoint(1.f);
            }
            catch (const std::exception& error)
            {
                if (!control->committing.load(std::memory_order_acquire))
                {
                    std::error_code ignored;
                    if (!createdPrefab.empty())
                    {
                        fs::remove(createdPrefab, ignored);
                        fs::remove(Engine::Core::AssetRecord::SidecarPath(
                            createdPrefab), ignored);
                    }
                    if (!createdCopy.empty())
                    {
                        fs::remove(createdCopy, ignored);
                        fs::remove(Engine::Core::AssetRecord::SidecarPath(
                            createdCopy), ignored);
                        fs::remove_all(createdCopy.parent_path() /
                            (createdCopy.stem().string() + "_dependencies"), ignored);
                    }
                    if (!result.outputDirectory.empty())
                        fs::remove_all(result.outputDirectory, ignored);
                }
                result.primary.clear();
                result.message = error.what();
            }
            return result;
        });
}

bool EditorState::PollImportJob()
{
    if (!m_importJob.valid()) return false;
    if (m_importJob.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) return true;
    ImportJobResult result = m_importJob.get();
    const bool cancelled = m_importJobControl &&
        m_importJobControl->cancelled.load(std::memory_order_acquire) &&
        !m_importJobControl->committing.load(std::memory_order_acquire);
    m_importJobControl.reset();
    if (cancelled)
    {
        const auto root = m_importDestinationRoot;
        Engine::Threading::JobScheduler::Get().Submit(
            [result = std::move(result), root] {
                const auto cleanup = [&root](const std::filesystem::path& path) {
                    const auto relative = path.lexically_relative(root);
                    if (relative.empty() || *relative.begin() == "..") return;
                    std::error_code ignored;
                    std::filesystem::remove_all(path, ignored);
                    std::filesystem::remove(
                        Engine::Core::AssetRecord::SidecarPath(path), ignored);
                };
                for (const auto& path : result.createdPaths) cleanup(path);
                if (!result.outputDirectory.empty()) cleanup(result.outputDirectory);
            });
        m_importDestinationRoot.clear();
        return true;
    }
    m_importDestinationRoot.clear();
    if (result.primary.empty())
    {
        m_pendingImportError = result.message.empty()
            ? "Import failed. See the Console for details, then retry."
            : result.message;
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Asset import failed: " + m_pendingImportError);
        return true;
    }
    if (m_primaryConsole)
    {
        m_primaryConsole->AddLog(ConsoleView::Level::Info,
            "Asset imported: " + result.primary);
        if (!result.message.empty() &&
            (std::filesystem::path(m_pendingImportAssetPath).extension() == ".pmx" ||
                result.message.find("missing source texture files:") != std::string::npos))
            m_primaryConsole->AddLog(ConsoleView::Level::Warning, result.message);
    }
    Engine::Scene::Scene* scene = m_pendingImportScene;
    Engine::Core::Object* reference = nullptr;
    if (scene && scene == GetActiveDocumentScene() &&
        m_importSceneGeneration == m_sceneDocumentGeneration &&
        m_importReferenceObject)
    {
        reference = scene->FindObjectByPath(m_importReferencePath);
        if (reference != m_importReferenceObject) reference = nullptr;
    }
    if (result.boundCharacter && reference && !result.boundPrefab.empty())
    {
        Engine::Serialization::SceneSerializer::RefreshPrefabInstances(
            *scene, result.boundPrefab, scene->GetGraphicsProvider());
    }
    else if (!result.bindingError.empty() && m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Warning,
            "MMD character binding skipped: " + result.bindingError);
    const std::string source = m_pendingImportAssetPath;
    const bool intoScene = m_pendingImportIntoScene && scene &&
        scene == GetActiveDocumentScene() &&
        m_importSceneGeneration == m_sceneDocumentGeneration;
    PublishImportJob(result.primary, source, intoScene, scene, reference);
    return true;
}

void EditorState::PublishImportJob(const std::string& primaryPath,
    const std::string& source, bool intoScene,
    Engine::Scene::Scene* scene, Engine::Core::Object* reference)
{
    namespace fs = std::filesystem;
    std::string primary = primaryPath;
    CancelPendingImportAsset();
    std::string extension = fs::path(source).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (!intoScene || !scene) return;
    if (extension != ".vmd")
    {
        Engine::Core::Object* object = InstantiateAsset(primary, true, false);
        if (object && reference)
        {
            const auto position = object->transform.position;
            const auto rotation = object->transform.rotation;
            const auto scale = object->transform.scale;
            if (scene->MoveObject(object, reference,
                    Engine::Scene::Scene::ObjectPlacement::AsChild))
            {
                object->transform.position = position;
                object->transform.rotation = rotation;
                object->transform.scale = scale;
            }
        }
        SelectObject(object);
        return;
    }
    const fs::path output = fs::path(primary).parent_path();
    for (const char* name : { "Camera.prefab", "Light.prefab" })
    {
        const fs::path prefab = output / name;
        if (fs::is_regular_file(prefab))
            InstantiateAsset(prefab.string(), true, false);
    }
}

Engine::Core::Object* EditorState::InstantiateAsset(const std::string& path,
    bool recordChange, bool showImportDialog)
{
    Engine::Scene::Scene* targetScene = GetActiveDocumentScene();
    if (!targetScene)
        return nullptr;

    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    Engine::Core::Object* object = nullptr;
    try
    {
        if (showImportDialog && (extension == ".vmd" ||
            extension == ".fbx" || extension == ".pmx" || extension == ".gltf" ||
            extension == ".glb" || extension == ".obj" ||
            extension == ".spriteanim"))
        {
            if (recordChange) StartObjectImport(path, true);
            return nullptr;
        }
        if (ModelImporter::SupportsExtension(extension))
        {
            if (recordChange) StartObjectImport(path, true);
            return nullptr;
        }
        else if (extension == ".prefab")
        {
            object = Engine::Serialization::SceneSerializer::InstantiatePrefab(
                *targetScene, path, targetScene->GetGraphicsProvider());
        }
        else if (extension == ".obj")
        {
            object = targetScene->AddObject(std::filesystem::path(path).stem().string());
            Engine::Components::Mesh* mesh = object->AddComponent<Engine::Components::Mesh>();
            mesh->LoadFromFile(path);
            if (targetScene->GetGraphicsProvider())
                mesh->CreateBuffer(targetScene->GetGraphicsProvider()->GetBufferFactory());
            object->AddComponent<Engine::Components::Material>();
        }
        else if (extension == ".spriteanim")
        {
            object = targetScene->AddObject(std::filesystem::path(path).stem().string());
            Engine::Components::SpriteAnimationManager* manager = object->AddComponent<Engine::Components::SpriteAnimationManager>();
            Engine::Components::Sprite* sprite = object->AddComponent<Engine::Components::Sprite>();
            sprite->SetAnimationManager(manager);
            if (!manager->LoadFromFile(path) ||
                !sprite->Prepare(targetScene->GetGraphicsProvider()))
                throw std::runtime_error("Could not load sprite animation");
        }
        else if (extension == ".scene" || extension == ".xml")
        {
            QueueSceneAssetDocumentOpen(path);
            return nullptr;
        }
        else
        {
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                    "Unsupported scene drop: " + path);
            return nullptr;
        }
    }
    catch (const std::exception& error)
    {
        if (object)
            targetScene->RemoveObject(object);
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not instantiate asset '" + path + "': " + error.what());
        return nullptr;
    }

    if (object && recordChange)
    {
        if (m_activeSceneAssetDocument)
        {
            if (m_renderer) m_renderer->MarkDirty();
        }
        else if (m_prefabDocumentFocused)
            SetPrefabDirty(true);
        else
        {
            m_hasUnsavedChanges = true;
            MarkSceneEdited();
        }
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "Added to scene: " + path);
    }
    return object;
}

}
