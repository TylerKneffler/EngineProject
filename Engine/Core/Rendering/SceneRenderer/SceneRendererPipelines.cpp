#include "Core/Rendering/SceneRenderer.h"
#include "Core/Rendering/SceneRenderer/SceneRendererPipelines.h"
#include "Core/Graphics/DepthConvention.h"
#include "Core/Components/Materials/Texture.h"
#include "Core/Rendering/Lighting/EnvironmentMap.h"
#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif
#include <filesystem>
#include <stdexcept>

namespace Engine::Rendering
{
using Scene = Engine::Scene::Scene;
void SceneRendererPipelines::BuildSkyboxPipeline(SceneRenderer& renderer)
{
    auto* shaderCompiler = renderer.m_graphicsProvider->GetShaderCompiler();
    auto* pipelineFactory = renderer.m_graphicsProvider->GetPipelineStateFactory();
    if (!shaderCompiler || !pipelineFactory)
        throw std::runtime_error("Failed to get skybox shader or pipeline factory");

    const std::string shaderPath = EngineShaderPath("Skybox.hlsl");
    auto vertexShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(), "VSMain", Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!vertexShader)
        throw std::runtime_error("Failed to compile skybox vertex shader: " + shaderCompiler->GetLastError());
    auto pixelShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(), "PSMain", Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!pixelShader)
        throw std::runtime_error("Failed to compile skybox pixel shader: " + shaderCompiler->GetLastError());

    const auto buildSkyboxPipeline = [&](bool stencilClip, const char* description)
    {
        auto builder = pipelineFactory->CreateBuilder();
        if (!builder)
            throw std::runtime_error(std::string("Failed to create ") + description + " pipeline builder");
        auto& state = builder->SetVertexShader(vertexShader.get())
            .SetPixelShader(pixelShader.get())
            .SetFillMode(false)
            .SetCullMode(false)
            .SetFrontCounterClockwise(false)
            .SetDepthClipEnable(false)
            .SetBlendEnable(false)
            .SetDepthEnable(false)
            .SetDepthWriteEnable(false)
            .SetDepthFunc(Engine::Graphics::Depth::CompareAlways)
            .SetInputLayout(nullptr, 0)
            .SetPrimitiveTopology(
                Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(10, 20);
        if (stencilClip)
        {
            state.SetStencilEnable(true)
                .SetStencilReadMask(0xFF)
                .SetStencilWriteMask(0x00)
                .SetStencilFunc(2)
                .SetStencilFailOp(0)
                .SetStencilDepthFailOp(0)
                .SetStencilPassOp(0)
                .SetStencilRef(1);
        }
        auto pipeline = state.Build();
        if (!pipeline)
            throw std::runtime_error(std::string("Failed to build ") + description + " pipeline: " + builder->GetLastError());
        return pipeline;
    };
    renderer.m_skyboxPipeline = buildSkyboxPipeline(false, "skybox");
    renderer.m_portalSkyboxStencilReadPipeline = buildSkyboxPipeline(true, "portal skybox stencil read");

    // Volumetric clouds: a fullscreen pass blended (premultiplied alpha)
    // over the sky before the scene's surfaces, so they hide it.
    {
        const std::string cloudPath = EngineShaderPath("Clouds.hlsl");
        auto cloudVertex = shaderCompiler->CompileFromFile(cloudPath.c_str(), "VSMain",
            Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
        auto cloudPixel = shaderCompiler->CompileFromFile(cloudPath.c_str(), "PSMain",
            Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
        if (!cloudVertex || !cloudPixel)
            throw std::runtime_error("Failed to compile cloud shaders: " + shaderCompiler->GetLastError());
        auto builder = pipelineFactory->CreateBuilder();
        if (!builder)
            throw std::runtime_error("Failed to create cloud pipeline builder");
        renderer.m_cloudPipeline = builder->SetVertexShader(cloudVertex.get())
            .SetPixelShader(cloudPixel.get())
            .SetFillMode(false)
            .SetCullMode(false)
            .SetFrontCounterClockwise(false)
            .SetDepthClipEnable(false)
            .SetBlendEnable(true)
            .SetSrcBlend(1)        // ONE: premultiplied
            .SetDestBlend(5)       // INV_SRC_ALPHA
            .SetBlendOp(0)
            .SetSrcBlendAlpha(1)
            .SetDestBlendAlpha(5)
            .SetBlendOpAlpha(0)
            .SetDepthEnable(false)
            .SetDepthWriteEnable(false)
            .SetDepthFunc(Engine::Graphics::Depth::CompareAlways)
            .SetInputLayout(nullptr, 0)
            .SetPrimitiveTopology(
                Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(10, 20)
            .Build();
        if (!renderer.m_cloudPipeline)
            throw std::runtime_error("Failed to build cloud pipeline: " + builder->GetLastError());
        // The half-resolution march (CSMain). Optional: without compute the
        // clouds are simply not drawn.
        if (auto march = shaderCompiler->CompileFromFile(cloudPath.c_str(), "CSMain",
                Engine::Graphics::IShaderCompiler::CompileProfile::CS_5_0))
            renderer.m_cloudMarchPipeline = pipelineFactory->CreateComputePipeline(march.get());
    }

    // Screen-space ambient occlusion (compute). Optional: without it,
    // surfaces are simply unoccluded.
    {
        const std::string occlusionPath = EngineShaderPath("AmbientOcclusion.hlsl");
        if (auto occlusion = shaderCompiler->CompileFromFile(occlusionPath.c_str(), "CSMain",
                Engine::Graphics::IShaderCompiler::CompileProfile::CS_5_0))
            renderer.m_ambientOcclusionPipeline = pipelineFactory->CreateComputePipeline(occlusion.get());
    }

    const std::string defaultPath =
        (std::filesystem::path(ENGINE_ASSETS_PATH) / "Textures" / "Skyboxes" /
            "editor-default-sky.png").string();
    renderer.m_defaultSkyboxTexture = Engine::Components::Texture::Acquire(defaultPath);
    if (!renderer.m_defaultSkyboxTexture->PrepareImmediate(renderer.m_graphicsProvider))
        renderer.m_defaultSkyboxTexture.reset();
}

void SceneRendererPipelines::BuildGridPipeline(SceneRenderer& renderer)
{
    if (!renderer.m_graphicsProvider)
        throw std::runtime_error("SceneRenderer::BuildGridPipeline: graphicsProvider not initialized");

    auto* shaderCompiler = renderer.m_graphicsProvider->GetShaderCompiler();
    auto* pipelineFactory = renderer.m_graphicsProvider->GetPipelineStateFactory();
    if (!shaderCompiler || !pipelineFactory)
        throw std::runtime_error("Failed to get shader compiler or pipeline factory");

    std::string shaderPath = EngineShaderPath("Grid.hlsl");
    auto vsShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(), "VSMain", Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!vsShader)
        throw std::runtime_error("Failed to compile grid vertex shader: " + shaderCompiler->GetLastError());

    auto psShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(), "PSMain", Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!psShader)
        throw std::runtime_error("Failed to compile grid pixel shader: " + shaderCompiler->GetLastError());

    auto builder = pipelineFactory->CreateBuilder();
    if (!builder)
        throw std::runtime_error("Failed to create pipeline state builder");

    auto& bp = *builder;
    renderer.m_gridPipeline = bp.SetVertexShader(vsShader.get())
        .SetPixelShader(psShader.get())
        .SetFillMode(false)
        .SetCullMode(false)
        .SetFrontCounterClockwise(false)
        .SetDepthClipEnable(true)
        .SetBlendEnable(true)
        .SetSrcBlend(4)
        .SetDestBlend(5)
        .SetBlendOp(0)
        .SetSrcBlendAlpha(1)
        .SetDestBlendAlpha(5)
        .SetBlendOpAlpha(0)
        .SetDepthEnable(true)
        .SetDepthWriteEnable(false)
        .SetDepthFunc(Engine::Graphics::Depth::CompareNearerOrEqual)
        .SetInputLayout(nullptr, 0)
        .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(10, 20)
        .Build();

    if (!renderer.m_gridPipeline)
        throw std::runtime_error("Failed to build grid pipeline: " + builder->GetLastError());
}

void SceneRendererPipelines::BuildParticlePipeline(SceneRenderer& renderer)
{
    auto* compiler = renderer.m_graphicsProvider->GetShaderCompiler();
    auto* factory = renderer.m_graphicsProvider->GetPipelineStateFactory();
    if (!compiler || !factory)
        throw std::runtime_error("Particle rendering requires shader and pipeline factories");
    const std::string path = EngineShaderPath("Particle.hlsl");
    auto vertex = compiler->CompileFromFile(path, "VSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!vertex)
        throw std::runtime_error("Particle vertex shader: " + compiler->GetLastError());
    auto pixel = compiler->CompileFromFile(path, "PSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!pixel)
        throw std::runtime_error("Particle pixel shader: " + compiler->GetLastError());
    auto gpuVertex = compiler->CompileFromFile(path, "VSMainGpu",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!gpuVertex)
        throw std::runtime_error("GPU particle vertex shader: " + compiler->GetLastError());
    auto fluidVertex = compiler->CompileFromFile(path, "VSMainFluid",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!fluidVertex)
        throw std::runtime_error("Fluid surface vertex shader: " + compiler->GetLastError());
    auto fluidPixel = compiler->CompileFromFile(path, "PSMainFluid",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!fluidPixel)
        throw std::runtime_error("Fluid surface pixel shader: " + compiler->GetLastError());

    const Engine::Graphics::IPipelineStateBuilder::VertexElement layout[] = {
        { "POSITION", 0, 2, 0, 0, true },
        { "COLOR", 0, 2, 0, 16, true },
        { "TEXCOORD", 0, 2, 0, 32, true }
    };

    const auto build = [&](bool alphaBlend, bool portalView, bool gpu)
    {
        auto builder = factory->CreateBuilder();
        auto& state = builder->SetVertexShader(gpu ? gpuVertex.get() : vertex.get())
            .SetPixelShader(pixel.get())
            .SetFillMode(false)
            .SetCullMode(false)
            .SetFrontCounterClockwise(false)
            .SetDepthClipEnable(true)
            .SetBlendEnable(true)
            .SetSrcBlend(4)
            .SetDestBlend(alphaBlend ? 5 : 1)
            .SetBlendOp(0)
            .SetSrcBlendAlpha(1)
            .SetDestBlendAlpha(alphaBlend ? 5 : 1)
            .SetBlendOpAlpha(0)
            .SetDepthEnable(true)
            .SetDepthWriteEnable(false)
            .SetDepthFunc(Engine::Graphics::Depth::CompareNearerOrEqual);
        if (portalView)
            state.SetStencilEnable(true)
                .SetStencilReadMask(0xFF)
                .SetStencilWriteMask(0x00)
                .SetStencilFunc(2)
                .SetStencilFailOp(0)
                .SetStencilDepthFailOp(0)
                .SetStencilPassOp(0)
                .SetStencilRef(1);
        auto pipeline = state
            .SetInputLayout(gpu ? nullptr : layout, gpu ? 0 : 3)
            .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(10, 20)
            .Build();
        if (!pipeline)
            throw std::runtime_error(std::string("Particle pipeline ") +
                (gpu ? "GPU " : "CPU ") +
                (alphaBlend ? "alpha " : "additive ") +
                (portalView ? "portal: " : "main: ") +
                builder->GetLastError());
        return pipeline;
    };
    renderer.m_particlePipeline = build(false, false, false);
    renderer.m_particleAlphaPipeline = build(true, false, false);
    renderer.m_particlePortalPipeline = build(false, true, false);
    renderer.m_particlePortalAlphaPipeline = build(true, true, false);
    renderer.m_particleGpuPipeline = build(false, false, true);
    renderer.m_particleGpuAlphaPipeline = build(true, false, true);
    renderer.m_particleGpuPortalPipeline = build(false, true, true);
    renderer.m_particleGpuPortalAlphaPipeline = build(true, true, true);

    const auto buildFluid = [&](bool portalView)
    {
        auto builder = factory->CreateBuilder();
        auto& state = builder->SetVertexShader(fluidVertex.get())
            .SetPixelShader(fluidPixel.get())
            .SetFillMode(false).SetCullMode(false)
            .SetFrontCounterClockwise(false).SetDepthClipEnable(true)
            .SetBlendEnable(true).SetSrcBlend(4).SetDestBlend(5)
            .SetBlendOp(0).SetSrcBlendAlpha(1).SetDestBlendAlpha(5)
            .SetBlendOpAlpha(0).SetDepthEnable(true)
            .SetDepthWriteEnable(false).SetDepthFunc(Engine::Graphics::Depth::CompareNearerOrEqual);
        if (portalView)
            state.SetStencilEnable(true).SetStencilReadMask(0xFF)
                .SetStencilWriteMask(0x00).SetStencilFunc(2)
                .SetStencilFailOp(0).SetStencilDepthFailOp(0)
                .SetStencilPassOp(0).SetStencilRef(1);
        auto pipeline = state.SetInputLayout(nullptr, 0)
            .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(10, 20).Build();
        if (!pipeline)
            throw std::runtime_error("Fluid surface pipeline: " + builder->GetLastError());
        return pipeline;
    };
    renderer.m_fluidSurfacePipeline = buildFluid(false);
    renderer.m_fluidSurfacePortalPipeline = buildFluid(true);
}

void SceneRendererPipelines::UpdateEnvironmentLighting(SceneRenderer& renderer, const Engine::Components::Texture* texture)
{
    const std::string path = texture ? texture->GetFilePath() : std::string{};
    auto environment = texture ? Engine::Rendering::EnvironmentMap::Acquire(*texture) : nullptr;
    if (path == renderer.m_environmentLightingPath && environment == renderer.m_environmentMap)
        return;

    renderer.m_environmentLightingPath = path;
    renderer.m_environmentMap = std::move(environment);
    if (renderer.m_environmentMap)
        renderer.m_environmentMap->Prepare(renderer.m_graphicsProvider);
    renderer.m_environmentSH = renderer.m_environmentMap
        ? renderer.m_environmentMap->GetRadianceSH() : std::array<glm::vec4, 9>{};
}

std::shared_ptr<Engine::Rendering::EnvironmentMap> SceneRendererPipelines::ResolveReflectionEnvironment(
    SceneRenderer& renderer, const Engine::Components::Material& material)
{
    if (!material.useCustomReflectionEnvironment ||
        !material.reflectionEnvironmentMap ||
        !material.reflectionEnvironmentMap->HasPixels())
        return nullptr;
    auto environment = Engine::Rendering::EnvironmentMap::Acquire(*material.reflectionEnvironmentMap);
    if (environment)
        environment->Prepare(renderer.m_graphicsProvider);
    return environment;
}
}
