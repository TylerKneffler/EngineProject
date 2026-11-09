#include "Core/Scene/Rendering/SceneRenderer/SceneRendererPipelines.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Rendering/Lighting/EnvironmentMap.h"
#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif
#include <filesystem>
#include <stdexcept>

namespace Engine::Scene::Renderer
{
void SceneRendererPipelines::BuildSkyboxPipeline(Scene& scene)
{
    auto* shaderCompiler = scene.m_graphicsProvider->GetShaderCompiler();
    auto* pipelineFactory = scene.m_graphicsProvider->GetPipelineStateFactory();
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
            .SetDepthFunc(7)
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
    scene.m_skyboxPipeline = buildSkyboxPipeline(false, "skybox");
    scene.m_portalSkyboxStencilReadPipeline = buildSkyboxPipeline(true, "portal skybox stencil read");

    const std::string defaultPath =
        (std::filesystem::path(ENGINE_ASSETS_PATH) / "Textures" / "Skyboxes" /
            "editor-default-sky.png").string();
    scene.m_defaultSkyboxTexture = Engine::Components::Texture::Acquire(defaultPath);
    if (!scene.m_defaultSkyboxTexture->PrepareImmediate(scene.m_graphicsProvider))
        scene.m_defaultSkyboxTexture.reset();
}

void SceneRendererPipelines::BuildGridPipeline(Scene& scene)
{
    if (!scene.m_graphicsProvider)
        throw std::runtime_error("Scene::BuildGridPipeline: graphicsProvider not initialized");

    auto* shaderCompiler = scene.m_graphicsProvider->GetShaderCompiler();
    auto* pipelineFactory = scene.m_graphicsProvider->GetPipelineStateFactory();
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
    scene.m_gridPipeline = bp.SetVertexShader(vsShader.get())
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
        .SetDepthFunc(3)
        .SetInputLayout(nullptr, 0)
        .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(10, 20)
        .Build();

    if (!scene.m_gridPipeline)
        throw std::runtime_error("Failed to build grid pipeline: " + builder->GetLastError());
}

void SceneRendererPipelines::BuildParticlePipeline(Scene& scene)
{
    auto* compiler = scene.m_graphicsProvider->GetShaderCompiler();
    auto* factory = scene.m_graphicsProvider->GetPipelineStateFactory();
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
            .SetDepthFunc(3);
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
    scene.m_particlePipeline = build(false, false, false);
    scene.m_particleAlphaPipeline = build(true, false, false);
    scene.m_particlePortalPipeline = build(false, true, false);
    scene.m_particlePortalAlphaPipeline = build(true, true, false);
    scene.m_particleGpuPipeline = build(false, false, true);
    scene.m_particleGpuAlphaPipeline = build(true, false, true);
    scene.m_particleGpuPortalPipeline = build(false, true, true);
    scene.m_particleGpuPortalAlphaPipeline = build(true, true, true);

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
            .SetDepthWriteEnable(false).SetDepthFunc(3);
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
    scene.m_fluidSurfacePipeline = buildFluid(false);
    scene.m_fluidSurfacePortalPipeline = buildFluid(true);
}

void SceneRendererPipelines::UpdateEnvironmentLighting(Scene& scene, const Engine::Components::Texture* texture)
{
    const std::string path = texture ? texture->GetFilePath() : std::string{};
    auto environment = texture ? Engine::Rendering::EnvironmentMap::Acquire(*texture) : nullptr;
    if (path == scene.m_environmentLightingPath && environment == scene.m_environmentMap)
        return;

    scene.m_environmentLightingPath = path;
    scene.m_environmentMap = std::move(environment);
    if (scene.m_environmentMap)
        scene.m_environmentMap->Prepare(scene.m_graphicsProvider);
    scene.m_environmentSH = scene.m_environmentMap
        ? scene.m_environmentMap->GetRadianceSH() : std::array<glm::vec4, 9>{};
}

std::shared_ptr<Engine::Rendering::EnvironmentMap> SceneRendererPipelines::ResolveReflectionEnvironment(
    Scene& scene, const Engine::Components::Material& material)
{
    if (!material.useCustomReflectionEnvironment ||
        !material.reflectionEnvironmentMap ||
        !material.reflectionEnvironmentMap->HasPixels())
        return nullptr;
    auto environment = Engine::Rendering::EnvironmentMap::Acquire(*material.reflectionEnvironmentMap);
    if (environment)
        environment->Prepare(scene.m_graphicsProvider);
    return environment;
}
}
