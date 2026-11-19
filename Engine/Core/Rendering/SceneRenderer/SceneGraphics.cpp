#include "Core/Rendering/ShadowReceiverCulling.h"
#include "Core/Rendering/StableShadowCascade.h"
#include "Core/Serialization/PrefabLoadProfile.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Rendering/SceneRenderer.h"
#include "Core/Rendering/SceneRenderer/SceneRendererVisibility.h"
#include "Core/Rendering/SceneRenderer/SceneRendererLighting.h"
#include "Core/Graphics/GraphicsCommandPacket.h"
#include <chrono>
#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Path/SplineGeometry.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainBatchAppearance.h"
#include "Core/Components/Physics/SpatialManipulator.h"
#include "Core/Components/Sprite/Sprite.h"
#include "Core/Components/Animation/SkinnedMesh.h"
#include "Core/Components/Animation/Skeleton.h"
#include "Core/Components/Materials/Texture.h"
#include "Core/Rendering/TextureFootprint.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include "Core/Rendering/Lighting/EnvironmentMap.h"
#include "Core/Components/Lighting/LightProbe.h"
#include "Core/Rendering/Portal/PortalRenderPolicy.h"
#include "Core/Model/LightingData.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IShader.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/PostProcess.h"
#include "Core/Renderers/UIRenderer.h"
#include "Core/Rendering/SceneRenderer.h"
#include "Core/Rendering/SceneRenderer/SceneRendererDebugPipelines.h"
#include "Core/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"
#include "Core/Rendering/SceneRenderer/SceneRendererFramePreparation.h"
#include "Core/Rendering/SceneRenderer/SceneRendererMaterialShaders.h"
#include "Core/Rendering/SceneRenderer/SceneRendererObjectPipelines.h"
#include "Core/Rendering/SceneRenderer/SceneRendererPipelines.h"
#include "Core/Rendering/SceneRenderer/SceneRendererParticles.h"
#include "Core/Rendering/SceneRenderer/SceneRendererResources.h"
#include "Core/Rendering/SceneRenderer/SceneRendererShadows.h"
#include "Core/Rendering/RenderUtilities.h"
#include "Core/Rendering/SceneRenderer/SceneRendererViewPreparation.h"
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

#include "Core/Rendering/SceneRenderer/SceneNativeRenderPacket.h"

namespace Engine::Rendering
{
using Scene = Engine::Scene::Scene;

void SceneRenderer::Initialize()
{
    SceneRendererResources::Initialize(*this, m_graphicsProvider);

    // Build pipeline states
    BuildGridPipeline();
    BuildParticlePipeline();
    BuildSkyboxPipeline();
    BuildObjectPipeline();
    m_uiRenderer = std::make_unique<Engine::Renderers::UIRenderer>();
    m_uiRenderer->Initialize(m_graphicsProvider);
}

void SceneRenderer::EnsureObjectRenderCapacity(uint32_t requiredObjects)
{
    Engine::Rendering::SceneRendererResources::EnsureObjectRenderCapacity(*this, requiredObjects);
}

void SceneRenderer::SelectRenderPassBuffers(uint32_t requiredObjectSlots)
{
    Engine::Rendering::SceneRendererResources::SelectRenderPassBuffers(*this, requiredObjectSlots);
}

void SceneRenderer::EnsureSkinPaletteCapacity(uint32_t requiredMatrices)
{
    Engine::Rendering::SceneRendererResources::EnsureSkinPaletteCapacity(*this, requiredMatrices);
}

void SceneRenderer::BuildSkyboxPipeline()
{
    Engine::Rendering::SceneRendererPipelines::BuildSkyboxPipeline(*this);
}

const Engine::Components::Texture* SceneRenderer::ResolveSkyboxTexture()
{
    // A 2D scene has no surrounding environment.  Let the render target's
    // clear colour be its flat, edge-to-edge background instead of projecting
    // either the authored panorama or the editor's fallback skybox.
    if (m_scene.settings.dimension == Engine::Model::SceneDimension::TwoD ||
        !m_scene.settings.skyboxEnabled)
        return nullptr;

    // Physical sky: the atmosphere's panorama, once its first build is done.
    if (m_scene.settings.physicalSky.enabled)
        if (const auto& panorama = m_physicalSky.Panorama();
            panorama && panorama->PrepareImmediate(m_graphicsProvider))
            return panorama.get();

    if (m_scene.settings.skyboxTexture.empty())
        return m_defaultSkyboxTexture.get();

    if (m_loadedSkyboxPath != m_scene.settings.skyboxTexture)
    {
        m_loadedSkyboxPath = m_scene.settings.skyboxTexture;
        m_sceneSkyboxTexture = Engine::Components::Texture::Acquire(m_scene.settings.skyboxTexture);
    }
    if (m_sceneSkyboxTexture && m_sceneSkyboxTexture->Prepare(m_graphicsProvider))
        return m_sceneSkyboxTexture.get();
    return m_defaultSkyboxTexture.get();
}

const Engine::Components::Texture* SceneRenderer::GetSkyboxPreviewTexture()
{
    return ResolveSkyboxTexture();
}

void SceneRenderer::UpdateEnvironmentLighting(const Engine::Components::Texture* texture)
{
    Engine::Rendering::SceneRendererPipelines::UpdateEnvironmentLighting(*this, texture);
}

std::shared_ptr<Engine::Rendering::EnvironmentMap>
SceneRenderer::ResolveReflectionEnvironment(
    const Engine::Components::Material& material)
{
    return Engine::Rendering::SceneRendererPipelines::ResolveReflectionEnvironment(*this, material);
}

// ---------------------------------------------------------------------------
// SceneRenderer::BuildGridPipeline
// ---------------------------------------------------------------------------

void SceneRenderer::BuildGridPipeline()
{
    Engine::Rendering::SceneRendererPipelines::BuildGridPipeline(*this);
}

void SceneRenderer::BuildParticlePipeline()
{
    Engine::Rendering::SceneRendererPipelines::BuildParticlePipeline(*this);
}

void SceneRenderer::RecordGpuParticleSimulation(IGraphicsContext* context)
{
    SceneRendererParticles::RecordGpuSimulation(*this, context);
}

void SceneRenderer::RenderParticles(IGraphicsContext* context,
    const glm::mat4& viewProjection, const glm::mat4& view,
    const Object* contentScopeRoot, const glm::vec4& clipPlane,
    bool portalView)
{
    SceneRendererParticles::Render(*this, context,
        viewProjection, view, contentScopeRoot, clipPlane, portalView);
}

SceneRenderer::MaterialShaderFuture SceneRenderer::QueueMaterialShaderBytecode(
    const std::string& path, bool terrain, int64_t stamp)
{
    return Engine::Rendering::SceneRendererMaterialShaders::QueueMaterialShaderBytecode(*this, path, terrain, stamp);
}

const Engine::Rendering::ShaderSourceCache::Source& SceneRenderer::ResolveMaterialShaderSource(
    const Engine::Components::Material& material, const std::string& path)
{
    return Engine::Rendering::SceneRendererMaterialShaders::ResolveMaterialShaderSource(*this, material, path);
}

void SceneRenderer::PrewarmMaterialShaders(const Engine::Components::Material& material,
    bool terrain)
{
    Engine::Rendering::SceneRendererMaterialShaders::PrewarmMaterialShaders(*this, material, terrain);
}

void SceneRenderer::PublishMaterialShaderPipelines(size_t maxPipelines)
{
    Engine::Rendering::SceneRendererMaterialShaders::PublishMaterialShaderPipelines(*this, maxPipelines);
}

Scene::IPipelineState* SceneRenderer::ResolveMaterialShaderPipeline(
    const Engine::Rendering::ShaderSourceCache::Source& requestedSource, IPipelineState* base, bool terrain,
    bool overlay, const std::string& blendMode, std::string& error)
{
    return Engine::Rendering::SceneRendererMaterialShaders::ResolveMaterialShaderPipeline(
        *this, requestedSource, base, terrain, overlay, blendMode, error);
}

void SceneRenderer::BuildObjectPipeline()
{
    Engine::Rendering::SceneRendererObjectPipelines::BuildObjectPipeline(*this);
}

// ---------------------------------------------------------------------------
// SceneRenderer::TakeRetiredGraphicsResources
// ---------------------------------------------------------------------------

void SceneRenderer::TakeRetiredGraphicsResources(RetiredGraphicsResources& retired)
{
    for (auto& pipeline : m_retiredMaterialShaderPipelines)
        retired.pipelines.push_back(std::move(pipeline));
    m_retiredMaterialShaderPipelines.clear();
    for (auto& texture : m_retiredShadowTextures)
        retired.textures.push_back(std::move(texture));
    m_retiredShadowTextures.clear();
}

}
