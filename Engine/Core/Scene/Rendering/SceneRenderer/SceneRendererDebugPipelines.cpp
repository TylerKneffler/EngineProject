#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDebugPipelines.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"
#include <stdexcept>

namespace Engine::Scene::Renderer
{
void SceneRendererDebugPipelines::BuildObjectDebugPipelines(
    Scene& scene,
    const Engine::Graphics::IShader* vsShader,
    const Engine::Graphics::IShader* psShader,
    const Engine::Graphics::IPipelineStateBuilder::VertexElement* layout,
    size_t layoutCount,
    const Engine::Graphics::IPipelineStateBuilder::VertexElement* terrainLayout,
    size_t terrainLayoutCount)
{
    auto* shaderCompiler = scene.m_graphicsProvider ? scene.m_graphicsProvider->GetShaderCompiler() : nullptr;
    auto* pipelineFactory = scene.m_graphicsProvider ? scene.m_graphicsProvider->GetPipelineStateFactory() : nullptr;
    if (!shaderCompiler || !pipelineFactory)
        throw std::runtime_error("Failed to get shader compiler or pipeline factory for debug pipelines");

    const auto buildSpatialDebugPipeline = [&](bool wireframe)
    {
        auto debugBuilder = pipelineFactory->CreateBuilder();
        if (!debugBuilder)
            throw std::runtime_error("Failed to create spatial debug pipeline builder");
        auto pipeline = debugBuilder->SetVertexShader(vsShader)
            .SetPixelShader(psShader)
            .SetFillMode(wireframe)
            .SetCullMode(false)
            .SetFrontCounterClockwise(false)
            .SetDepthClipEnable(true)
            .SetBlendEnable(true)
            .SetSrcBlend(4)
            .SetDestBlend(5)
            .SetBlendOp(0)
            .SetSrcBlendAlpha(1)
            .SetDestBlendAlpha(0)
            .SetBlendOpAlpha(0)
            .SetDepthEnable(false)
            .SetDepthWriteEnable(false)
            .SetDepthFunc(7)
            .SetInputLayout(layout, static_cast<uint32_t>(layoutCount))
            .SetPrimitiveTopology(
                Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(10, 20)
            .Build();
        if (!pipeline)
            throw std::runtime_error("Failed to build spatial debug pipeline: " + debugBuilder->GetLastError());
        return pipeline;
    };
    scene.m_objectSpatialDebugPipeline = buildSpatialDebugPipeline(false);
    scene.m_objectSpatialDebugWirePipeline = buildSpatialDebugPipeline(true);

    auto outlineBuilder = pipelineFactory->CreateBuilder();
    if (!outlineBuilder)
        throw std::runtime_error("Failed to create outline pipeline state builder");

    const std::string outlineShaderPath = EngineShaderPath("ObjectOutline.hlsl");
    auto outlineVsShader = shaderCompiler->CompileFromFile(
        outlineShaderPath.c_str(),
        "VSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!outlineVsShader)
        throw std::runtime_error("Failed to compile object outline vertex shader: " + shaderCompiler->GetLastError());
    auto terrainOutlineVsShader = shaderCompiler->CompileFromFile(
        outlineShaderPath.c_str(), "VSTerrainMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!terrainOutlineVsShader)
        throw std::runtime_error("Failed to compile terrain outline vertex shader: " + shaderCompiler->GetLastError());

    auto outlinePsShader = shaderCompiler->CompileFromFile(
        outlineShaderPath.c_str(),
        "PSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!outlinePsShader)
        throw std::runtime_error("Failed to compile object outline pixel shader: " + shaderCompiler->GetLastError());

    auto& op = *outlineBuilder;
    scene.m_objectOutlinePipeline = op.SetVertexShader(outlineVsShader.get())
        .SetPixelShader(outlinePsShader.get())
        .SetFillMode(true)
        .SetCullMode(false)
        .SetFrontCounterClockwise(false)
        .SetDepthClipEnable(true)
        .SetBlendEnable(false)
        .SetDepthEnable(true)
        .SetDepthWriteEnable(false)
        .SetDepthFunc(3)
        .SetInputLayout(layout, static_cast<uint32_t>(layoutCount))
        .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(10, 20)
        .Build();
    if (!scene.m_objectOutlinePipeline)
        throw std::runtime_error("Failed to build object outline pipeline: " + outlineBuilder->GetLastError());

    auto pmxOutlineVs = shaderCompiler->CompileFromFile(
        outlineShaderPath.c_str(), "VSMmdMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    auto pmxOutlinePs = shaderCompiler->CompileFromFile(
        outlineShaderPath.c_str(), "PSMmdMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!pmxOutlineVs || !pmxOutlinePs)
        throw std::runtime_error("Failed to compile PMX outline shaders: " + shaderCompiler->GetLastError());
    auto pmxOutlineBuilder = pipelineFactory->CreateBuilder();
    scene.m_pmxOutlinePipeline = pmxOutlineBuilder->SetVertexShader(pmxOutlineVs.get())
        .SetPixelShader(pmxOutlinePs.get())
        .SetFillMode(false)
        .SetCullMode(true)
        .SetFrontCounterClockwise(true)
        .SetDepthClipEnable(true)
        .SetBlendEnable(true)
        .SetSrcBlend(4)
        .SetDestBlend(5)
        .SetBlendOp(0)
        .SetSrcBlendAlpha(1)
        .SetDestBlendAlpha(0)
        .SetBlendOpAlpha(0)
        .SetDepthEnable(true)
        .SetDepthWriteEnable(false)
        .SetDepthFunc(3)
        .SetInputLayout(layout, static_cast<uint32_t>(layoutCount))
        .SetPrimitiveTopology(
            Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(10, 20)
        .Build();
    if (!scene.m_pmxOutlinePipeline)
        throw std::runtime_error("Failed to build PMX outline pipeline: " + pmxOutlineBuilder->GetLastError());

    auto terrainOutlineBuilder = pipelineFactory->CreateBuilder();
    if (!terrainOutlineBuilder)
        throw std::runtime_error("Failed to create terrain outline pipeline builder");
    auto terrainOutline = terrainOutlineBuilder
        ->SetVertexShader(terrainOutlineVsShader.get())
        .SetPixelShader(outlinePsShader.get())
        .SetFillMode(true)
        .SetCullMode(false)
        .SetFrontCounterClockwise(false)
        .SetDepthClipEnable(true)
        .SetBlendEnable(false)
        .SetDepthEnable(true)
        .SetDepthWriteEnable(false)
        .SetDepthFunc(3)
        .SetInputLayout(terrainLayout, static_cast<uint32_t>(terrainLayoutCount))
        .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(10, 20)
        .Build();
    if (!terrainOutline)
        throw std::runtime_error("Failed to build terrain outline pipeline: " + terrainOutlineBuilder->GetLastError());
    scene.m_terrainPipelineByBase.emplace(scene.m_objectOutlinePipeline.get(), std::move(terrainOutline));
}
}
