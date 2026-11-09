#include "pch.h"
#include "EditorState.h"
#include "Engine/Editor/Diagnostics/FrameActivity.h"
#include "Core/SceneManager.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/IEditorPanel.h"
#include "Core/View/View.h"
#include "Core/View/Views/PreferencesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Lighting/LightProbe.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Threading/JobScheduler.h"

#include "EditorStateSupport.h"

namespace Engine::Editor
{
using namespace StateSupport;

void EditorState::BakeLighting()
{
    FrameActivity::Mark(FrameActivity::Bake);
    if (IsBakeJobRunning())
    {
        CancelBakeJob();
        return;
    }
    if (!m_scene)
        return;
    auto document = Engine::Serialization::SceneSerializer::CaptureSceneDocument(
        *m_scene);
    const std::string assets = m_projectSettings.assetsDirectory.empty()
        ? "Assets" : m_projectSettings.assetsDirectory;
    std::string sceneName = m_currentScenePath.empty()
        ? m_projectSettings.name
        : std::filesystem::path(m_currentScenePath).stem().string();
    if (sceneName.empty())
        sceneName = "Scene";
    for (char& character : sceneName)
        if (!std::isalnum(static_cast<unsigned char>(character)) &&
            character != '_' && character != '-') character = '_';
    static std::atomic<uint64_t> nextBakeId{1};
    const std::string outputName = sceneName + "_job_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "_" + std::to_string(nextBakeId.fetch_add(1));
    const auto settings = m_projectSettings.bakedLighting;
    m_bakeSceneGeneration = m_sceneDocumentGeneration;
    m_bakeSceneRevision = m_sceneEditRevision;
    m_bakePublicationIndex = 0;
    m_bakePublicationFailed = false;
    m_bakeJobControl = std::make_shared<AssetJobControl>();
    const auto control = m_bakeJobControl;
    m_bakeJob = Engine::Threading::JobScheduler::Get().Submit(
        [document = std::move(document), assets, outputName, settings,
            control]() mutable -> BakeJobResult
        {
            BakeJobResult output;
            output.outputRoot = std::filesystem::path(assets) / ".temp" /
                "Baked" / outputName;
            try
            {
                control->Checkpoint();
                control->SetProgress(0.02f);
                Engine::Scene::Scene snapshot;
                if (!Engine::Serialization::SceneSerializer::LoadFromValue(
                        snapshot, document, nullptr))
                    throw std::runtime_error("Could not prepare lighting bake scene");
                control->Checkpoint();
                control->SetProgress(0.1f);
                output.status = snapshot.BakeLighting(assets, outputName,
                    settings,
                    [control](float progress) {
                        control->SetProgress(0.1f + 0.85f * progress);
                    },
                    [control] { return control->cancelled.load(
                        std::memory_order_acquire); });
                if (!output.status.succeeded)
                    throw std::runtime_error(output.status.message);
                control->Checkpoint();
                for (const auto& object : snapshot.GetObjects())
                {
                    BakeObjectResult item;
                    if (const auto* data = object->GetComponent<
                            Engine::Rendering::BakedLightingData>())
                    {
                        item.hasMaterial = data->valid;
                        item.irradiance = data->irradiance;
                        item.directionalIrradiance = data->directionalIrradiance;
                        item.lightDirection = data->lightDirection;
                        item.originalMaterialAsset = data->originalMaterialAsset;
                        item.originalMaterialSnapshot = data->originalMaterialSnapshot;
                        item.bakedMaterialAsset = data->bakedMaterialAsset;
                        item.bakedLightmapAsset = data->bakedLightmapAsset;
                    }
                    if (const auto* probe = object->GetComponent<
                            Engine::Components::LightProbe>())
                    {
                        item.hasProbe = probe->valid;
                        item.probeIrradiance = probe->irradiance;
                        item.probeDirectional = probe->directionalIrradiance;
                        item.probeDirection = probe->lightDirection;
                    }
                    if (const auto* group = object->GetComponent<
                            Engine::Components::LightProbeGroup>())
                    {
                        item.hasProbeGroup = group->valid;
                        item.groupIrradiance = group->irradiance;
                        item.groupDirectional = group->directionalIrradiance;
                        item.groupDirections = group->lightDirections;
                    }
                    if (item.hasMaterial || item.hasProbe || item.hasProbeGroup)
                    {
                        snapshot.TryGetObjectPath(object.get(), item.path);
                        output.objects.push_back(std::move(item));
                    }
                }
                control->Checkpoint();
                control->SetProgress(1.f);
            }
            catch (const std::exception& error)
            {
                output.status.succeeded = false;
                output.status.message = error.what();
                std::error_code ignored;
                std::filesystem::remove_all(output.outputRoot, ignored);
                output.objects.clear();
            }
            return output;
        });
}

void EditorState::CancelBakeJob()
{
    if (m_bakeJobControl)
        m_bakeJobControl->cancelled.store(true, std::memory_order_release);
    if (m_bakePublication)
    {
        const auto root = m_bakePublication->outputRoot;
        m_bakePublication.reset();
        if (m_scene)
        {
            m_scene->ClearBakedLighting();
            m_hasUnsavedChanges = true;
            MarkSceneEdited();
        }
        Engine::Threading::JobScheduler::Get().Submit([root] {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        });
    }
}

bool EditorState::PollBakeJob()
{
    if (m_bakeJob.valid())
    {
        if (m_bakeJob.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready) return true;
        BakeJobResult result = m_bakeJob.get();
        const bool cancelled = m_bakeJobControl &&
            m_bakeJobControl->cancelled.load(std::memory_order_acquire);
        const bool stale = !m_scene ||
            m_bakeSceneGeneration != m_sceneDocumentGeneration ||
            m_bakeSceneRevision != m_sceneEditRevision;
        if (cancelled || stale || !result.status.succeeded)
        {
            const auto root = result.outputRoot;
            Engine::Threading::JobScheduler::Get().Submit([root] {
                std::error_code ignored;
                std::filesystem::remove_all(root, ignored);
            });
            if (!cancelled && !stale && m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    result.status.message);
            if (!cancelled && !stale && m_scene)
                m_scene->SetLightingBakeStatus(result.status.message);
            m_bakeJobControl.reset();
            return true;
        }
        m_bakePublication = std::move(result);
        m_bakePublicationIndex = 0;
        m_bakePublicationFailed = false;
    }
    if (!m_bakePublication) return false;
    if (!m_scene || m_bakeSceneGeneration != m_sceneDocumentGeneration ||
        m_bakeSceneRevision != m_sceneEditRevision)
    {
        CancelBakeJob();
        m_bakeJobControl.reset();
        return true;
    }
    if (m_bakePublicationIndex == 0)
        m_scene->ClearBakedLighting();
    constexpr size_t publicationBudget = 2;
    const size_t end = std::min(m_bakePublicationIndex + publicationBudget,
        m_bakePublication->objects.size());
    for (; m_bakePublicationIndex < end; ++m_bakePublicationIndex)
    {
        const BakeObjectResult& item =
            m_bakePublication->objects[m_bakePublicationIndex];
        Engine::Core::Object* object = m_scene->FindObjectByPath(item.path);
        if (!object) { m_bakePublicationFailed = true; continue; }
        if (item.hasMaterial)
        {
            auto* material = object->GetComponent<Engine::Components::Material>();
            if (!material)
                material = object->AddComponent<Engine::Components::Material>();
            if (material->LoadFromFile(item.bakedMaterialAsset))
            {
                material->PrepareTextures(m_scene->GetGraphicsProvider());
                auto* data = object->GetComponent<
                    Engine::Rendering::BakedLightingData>();
                if (!data)
                    data = object->AddComponent<
                        Engine::Rendering::BakedLightingData>();
                data->irradiance = item.irradiance;
                data->directionalIrradiance = item.directionalIrradiance;
                data->lightDirection = item.lightDirection;
                data->originalMaterialAsset = item.originalMaterialAsset;
                data->originalMaterialSnapshot = item.originalMaterialSnapshot;
                data->bakedMaterialAsset = item.bakedMaterialAsset;
                data->bakedLightmapAsset = item.bakedLightmapAsset;
                data->valid = true;
                data->version = 3;
            }
            else
            {
                m_bakePublicationFailed = true;
                if (m_primaryConsole)
                    m_primaryConsole->AddLog(ConsoleView::Level::Error,
                        "Could not publish baked material: " +
                        item.bakedMaterialAsset);
            }
        }
        if (item.hasProbe)
            if (auto* probe = object->GetComponent<
                    Engine::Components::LightProbe>())
            {
                probe->irradiance = item.probeIrradiance;
                probe->directionalIrradiance = item.probeDirectional;
                probe->lightDirection = item.probeDirection;
                probe->valid = true;
            }
        if (item.hasProbeGroup)
            if (auto* group = object->GetComponent<
                    Engine::Components::LightProbeGroup>())
            {
                group->irradiance = item.groupIrradiance;
                group->directionalIrradiance = item.groupDirectional;
                group->lightDirections = item.groupDirections;
                group->valid = true;
            }
    }
    if (m_bakePublicationIndex == m_bakePublication->objects.size())
    {
        if (m_bakePublicationFailed)
        {
            const auto root = m_bakePublication->outputRoot;
            m_scene->ClearBakedLighting();
            m_scene->SetLightingBakeStatus("Lighting bake publication failed.");
            Engine::Threading::JobScheduler::Get().Submit([root] {
                std::error_code ignored;
                std::filesystem::remove_all(root, ignored);
            });
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Lighting bake publication failed.");
        }
        else
        {
            m_scene->SetLightingBakeStatus(m_bakePublication->status.message);
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Info,
                    m_bakePublication->status.message);
        }
        m_bakePublication.reset();
        m_bakeJobControl.reset();
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
    }
    return true;
}

void EditorState::ClearBakedLighting()
{
    CancelBakeJob();
    if (!m_scene)
        return;
    m_scene->ClearBakedLighting();
    if (m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Info,
            m_scene->GetLightingBakeStatus());
    m_hasUnsavedChanges = true;
    MarkSceneEdited();
}

}
