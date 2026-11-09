#include "Core/Rendering/ShadowReceiverCulling.h"
#include "Core/Rendering/StableShadowCascade.h"
#include "Core/Serialization/PrefabLoadProfile.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Scene/Scene.h"
#include "Core/Graphics/GraphicsCommandPacket.h"
#include <chrono>
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Terrain/Terrain.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
#include "Core/Compoonents/Path/SplineGeometry.h"
#include "Core/Compoonents/Terrain/TerrainManager.h"
#include "Core/Compoonents/Terrain/TerrainBatchAppearance.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Compoonents/Sprite/Sprite.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Rendering/TextureFootprint.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include "Core/Rendering/Lighting/EnvironmentMap.h"
#include "Core/Compoonents/Lighting/LightProbe.h"
#include "Core/Rendering/Portal/PortalRenderPolicy.h"
#include "Core/Model/LightingData.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IShader.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/PostProcess.h"
#include "Core/Renderers/UIRenderer.h"
#include "Core/Scene/Rendering/SceneRenderer.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDebugPipelines.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererFramePreparation.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererMaterialShaders.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererObjectPipelines.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererPipelines.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererParticles.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererResources.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererShadows.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererViewPreparation.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#if defined(_WIN32)
#include "Core/Renderers/DX11/DX11GraphicsProvider.h"
#include "Core/Renderers/DX12/DX12GraphicsProvider.h"
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#endif

#if defined(ENGINE_VULKAN_ENABLED)
#include "Core/Renderers/Vulkan/VulkanGraphicsProvider.h"
#endif

#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif

#include "Core/Scene/Rendering/SceneRenderer/SceneNativeRenderPacket.h"

namespace Engine::Scene
{
using Renderer::HashRevision;
void Scene::Init(Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    Renderer::SceneRendererResources::Initialize(*this, graphicsProvider);

    // Set up the default editor camera
    Engine::Components::Camera* editorCameraComponent = editorCamera.AddComponent<Engine::Components::Camera>();
    editorCameraComponent->useTransformRotation = false;
    editorCameraComponent->farPlane = 1000.f;
    SetEditorMode2D(
        settings.dimension == Engine::Model::SceneDimension::TwoD);

    // Build pipeline states
    BuildGridPipeline();
    BuildParticlePipeline();
    BuildSkyboxPipeline();
    BuildObjectPipeline();
    m_uiRenderer = std::make_unique<Engine::Renderers::UIRenderer>();
    m_uiRenderer->Initialize(m_graphicsProvider);
}

void Scene::EnsureObjectRenderCapacity(uint32_t requiredObjects)
{
    Engine::Scene::Renderer::SceneRendererResources::EnsureObjectRenderCapacity(*this, requiredObjects);
}

void Scene::SelectRenderPassBuffers(uint32_t requiredObjectSlots)
{
    Engine::Scene::Renderer::SceneRendererResources::SelectRenderPassBuffers(*this, requiredObjectSlots);
}

void Scene::EnsureSkinPaletteCapacity(uint32_t requiredMatrices)
{
    Engine::Scene::Renderer::SceneRendererResources::EnsureSkinPaletteCapacity(*this, requiredMatrices);
}

void Scene::SetEditorMode2D(bool enabled)
{
    settings.dimension = enabled
        ? Engine::Model::SceneDimension::TwoD
        : Engine::Model::SceneDimension::ThreeD;
    if (m_editorCameraModeInitialized && m_editorMode2D == enabled)
        return;
    m_editorMode2D = enabled;
    Engine::Components::Camera* camera = editorCamera.GetComponent<Engine::Components::Camera>();
    if (!camera)
        return;
    m_editorCameraModeInitialized = true;
    camera->useTransformRotation = false;
    camera->orthographic = enabled;
    camera->target = { 0.f, 0.f, 0.f };
    camera->up = { 0.f, 1.f, 0.f };
    editorCamera.transform.position = enabled
        ? glm::vec3(0.f, 0.f, -10.f)
        : glm::vec3(0.f, 1.5f, 3.f);
}

void Scene::BuildSkyboxPipeline()
{
    Engine::Scene::Renderer::SceneRendererPipelines::BuildSkyboxPipeline(*this);
}

const Engine::Components::Texture* Scene::ResolveSkyboxTexture()
{
    // A 2D scene has no surrounding environment.  Let the render target's
    // clear colour be its flat, edge-to-edge background instead of projecting
    // either the authored panorama or the editor's fallback skybox.
    if (settings.dimension == Engine::Model::SceneDimension::TwoD ||
        !settings.skyboxEnabled)
        return nullptr;

    if (settings.skyboxTexture.empty())
        return m_defaultSkyboxTexture.get();

    if (m_loadedSkyboxPath != settings.skyboxTexture)
    {
        m_loadedSkyboxPath = settings.skyboxTexture;
        m_sceneSkyboxTexture = Engine::Components::Texture::Acquire(settings.skyboxTexture);
    }
    if (m_sceneSkyboxTexture && m_sceneSkyboxTexture->Prepare(m_graphicsProvider))
        return m_sceneSkyboxTexture.get();
    return m_defaultSkyboxTexture.get();
}

const Engine::Components::Texture* Scene::GetSkyboxPreviewTexture()
{
    return ResolveSkyboxTexture();
}

void Scene::UpdateEnvironmentLighting(const Engine::Components::Texture* texture)
{
    Engine::Scene::Renderer::SceneRendererPipelines::UpdateEnvironmentLighting(*this, texture);
}

std::shared_ptr<Engine::Rendering::EnvironmentMap>
Scene::ResolveReflectionEnvironment(
    const Engine::Components::Material& material)
{
    return Engine::Scene::Renderer::SceneRendererPipelines::ResolveReflectionEnvironment(*this, material);
}

// ---------------------------------------------------------------------------
// Scene::BuildGridPipeline
// ---------------------------------------------------------------------------

void Scene::BuildGridPipeline()
{
    Engine::Scene::Renderer::SceneRendererPipelines::BuildGridPipeline(*this);
}

void Scene::BuildParticlePipeline()
{
    Engine::Scene::Renderer::SceneRendererPipelines::BuildParticlePipeline(*this);
}

void Scene::RecordGpuParticleSimulation(IGraphicsContext* context)
{
    Renderer::SceneRendererParticles::RecordGpuSimulation(*this, context);
}

void Scene::RenderParticles(IGraphicsContext* context,
    const glm::mat4& viewProjection, const glm::mat4& view,
    const Object* contentScopeRoot, const glm::vec4& clipPlane,
    bool portalView)
{
    Renderer::SceneRendererParticles::Render(*this, context,
        viewProjection, view, contentScopeRoot, clipPlane, portalView);
}

Scene::MaterialShaderFuture Scene::QueueMaterialShaderBytecode(
    const std::string& path, bool terrain, int64_t stamp)
{
    return Engine::Scene::Renderer::SceneRendererMaterialShaders::QueueMaterialShaderBytecode(*this, path, terrain, stamp);
}

const Engine::Rendering::ShaderSourceCache::Source& Scene::ResolveMaterialShaderSource(
    const Engine::Components::Material& material, const std::string& path)
{
    return Engine::Scene::Renderer::SceneRendererMaterialShaders::ResolveMaterialShaderSource(*this, material, path);
}

void Scene::PrewarmMaterialShaders(const Engine::Components::Material& material,
    bool terrain)
{
    Engine::Scene::Renderer::SceneRendererMaterialShaders::PrewarmMaterialShaders(*this, material, terrain);
}

void Scene::PublishMaterialShaderPipelines(size_t maxPipelines)
{
    Engine::Scene::Renderer::SceneRendererMaterialShaders::PublishMaterialShaderPipelines(*this, maxPipelines);
}

Scene::IPipelineState* Scene::ResolveMaterialShaderPipeline(
    const Engine::Rendering::ShaderSourceCache::Source& requestedSource, IPipelineState* base, bool terrain,
    bool overlay, const std::string& blendMode, std::string& error)
{
    return Engine::Scene::Renderer::SceneRendererMaterialShaders::ResolveMaterialShaderPipeline(
        *this, requestedSource, base, terrain, overlay, blendMode, error);
}

void Scene::BuildObjectPipeline()
{
    Engine::Scene::Renderer::SceneRendererObjectPipelines::BuildObjectPipeline(*this);
}

// ---------------------------------------------------------------------------
// Scene::PrepareRenderFrame
// ---------------------------------------------------------------------------

Scene::RetiredGraphicsResources Scene::TakeRetiredGraphicsResources()
{
    RetiredGraphicsResources retired;
    retired.objects.swap(m_retiredRenderObjects);
    retired.buffers.swap(m_retiredRenderBuffers);
    retired.sharedBuffers.swap(m_retiredSharedRenderBuffers);
    retired.pipelines.swap(m_retiredMaterialShaderPipelines);
    retired.textures.swap(m_retiredShadowTextures);
    return retired;
}

void Scene::RetireGraphicsBuffer(std::unique_ptr<IGraphicsBuffer> buffer)
{
    if (buffer && m_deferGraphicsRetirement)
        m_retiredRenderBuffers.push_back(std::move(buffer));
}

void Scene::RetireGraphicsBuffer(std::shared_ptr<IGraphicsBuffer> buffer)
{
    if (buffer && m_deferGraphicsRetirement)
        m_retiredSharedRenderBuffers.push_back(std::move(buffer));
}


}
