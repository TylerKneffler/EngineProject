#include "Core/Scene/Rendering/SceneRenderer/SceneRendererObjectPipelines.h"

#include "Core/Graphics/IPipelineState.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDebugPipelines.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"

#include <stdexcept>
#include <string>

namespace Engine::Scene::Renderer
{
namespace
{
using Engine::Scene::Renderer::EngineShaderPath;
}

void SceneRendererObjectPipelines::BuildObjectPipeline(Scene& scene)
{
    if (!scene.m_graphicsProvider)
        throw std::runtime_error("Scene::BuildObjectPipeline: graphicsProvider not initialized");

    auto* shaderCompiler = scene.m_graphicsProvider->GetShaderCompiler();
    auto* pipelineFactory = scene.m_graphicsProvider->GetPipelineStateFactory();

    if (!shaderCompiler || !pipelineFactory)
        throw std::runtime_error("Failed to get shader compiler or pipeline factory");

    const std::string shaderPath = EngineShaderPath("Object.hlsl");
    auto vsShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(),
        "VSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!vsShader)
        throw std::runtime_error("Failed to compile object vertex shader: " + shaderCompiler->GetLastError());
    auto terrainVsShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(), "VSTerrainMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!terrainVsShader)
        throw std::runtime_error("Failed to compile terrain vertex shader: " +
            shaderCompiler->GetLastError());

    auto psShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(),
        "PSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!psShader)
        throw std::runtime_error("Failed to compile object pixel shader: " + shaderCompiler->GetLastError());
    auto shadowPsShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(), "PSShadowMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!shadowPsShader)
        throw std::runtime_error("Failed to compile shadow pixel shader: " +
            shaderCompiler->GetLastError());

    Engine::Graphics::IPipelineStateBuilder::VertexElement layout[] =
    {
        { "POSITION", 0, 6, 0,  0, false },
        { "NORMAL",   0, 6, 0, 12, false },
        { "TEXCOORD", 0, 16, 0, 24, false },
        { "TANGENT",  0, 2, 0, 32, false },
        { "TEXCOORD", 1, 16, 0, 48, false },
        { "COLOR",    0, 2, 0, 56, false },
        { "JOINTS",   0, 2, 0, 72, false },
        { "WEIGHTS",  0, 2, 0, 88, false },
        { "JOINTS",   1, 2, 0, 104, false },
        { "WEIGHTS",  1, 2, 0, 120, false },
        { "TEXCOORD", 3, 2, 0, 136, false },
        { "TEXCOORD", 4, 2, 0, 152, false },
        { "TEXCOORD", 5, 2, 0, 168, false },
        { "TEXCOORD", 6, 2, 0, 184, false },
        { "TEXCOORD", 7, 2, 0, 200, false },
        { "TEXCOORD", 8, 2, 0, 216, false },
        { "TEXCOORD", 9, 2, 0, 232, false },
        { "TEXCOORD", 10, 2, 0, 248, false },
    };
    Engine::Graphics::IPipelineStateBuilder::VertexElement terrainLayout[] =
    {
        { "POSITION", 0, 6, 0,  0, false },
        { "NORMAL",   0, 6, 0, 12, false },
        { "TEXCOORD", 0, 16, 0, 24, false },
        { "COLOR",    0, 2, 0, 32, false },
    };
    scene.m_terrainPipelineByBase.clear();
    scene.m_materialShaderBuilders.clear();
    scene.m_materialShaderPipelines.clear();
    scene.m_materialShaderBytecode.clear();
    if (!scene.m_deferGraphicsRetirement)
        scene.m_retiredMaterialShaderPipelines.clear();

    enum class PortalStencilMode
    {
        None,
        Write,
        Increment,
        ResetDepth,
        Read
    };

    auto buildConfiguredMaterialPipeline = [pipelineFactory, layout, terrainLayout](
                                         const Engine::Graphics::IShader* vertex,
                                         const Engine::Graphics::IShader* pixel,
                                         bool doubleSided,
                                         bool blend,
                                         bool wireframe,
                                         PortalStencilMode stencilMode,
                                         bool colorWriteEnabled,
                                         const char* description,
                                         bool terrainFormat,
                                         bool overlay,
                                         const std::string& blendMode)
    {
        const bool diagnosticDepthEqual = blendMode == "DiagnosticDepthEqual";
        if (blendMode == "Opaque" || diagnosticDepthEqual) blend = false;
        else if (blendMode == "Alpha" || blendMode == "Additive" || overlay) blend = true;
        auto materialBuilder = pipelineFactory->CreateBuilder();
        if (!materialBuilder)
            throw std::runtime_error(
                std::string("Failed to create ") + description +
                " pipeline state builder");
        auto& state = materialBuilder->SetVertexShader(vertex)
            .SetPixelShader(pixel)
            .SetFillMode(wireframe)
            .SetCullMode(!doubleSided)
            .SetFrontCounterClockwise(false)
            .SetDepthClipEnable(true)
            .SetBlendEnable(blend)
            .SetColorWriteMask(colorWriteEnabled ? 0x0F : 0x00);
        if (blend)
        {
            state.SetSrcBlend(4)
                .SetDestBlend(blendMode == "Additive" ? 1 : 5)
                .SetBlendOp(0)
                .SetSrcBlendAlpha(1)
                .SetDestBlendAlpha(0)
                .SetBlendOpAlpha(0);
        }

        switch (stencilMode)
        {
        case PortalStencilMode::Write:
            state.SetStencilEnable(true)
                .SetStencilReadMask(0xFF)
                .SetStencilWriteMask(0xFF)
                .SetStencilFunc(7)
                .SetStencilFailOp(0)
                .SetStencilDepthFailOp(0)
                .SetStencilPassOp(2)
                .SetStencilRef(1);
            break;
        case PortalStencilMode::Increment:
            state.SetStencilEnable(true)
                .SetStencilReadMask(0xFF)
                .SetStencilWriteMask(0xFF)
                .SetStencilFunc(2)
                .SetStencilFailOp(0)
                .SetStencilDepthFailOp(0)
                .SetStencilPassOp(3)
                .SetStencilRef(1);
            break;
        case PortalStencilMode::Read:
        case PortalStencilMode::ResetDepth:
            state.SetStencilEnable(true)
                .SetStencilReadMask(0xFF)
                .SetStencilWriteMask(0x00)
                .SetStencilFunc(2)
                .SetStencilFailOp(0)
                .SetStencilDepthFailOp(0)
                .SetStencilPassOp(0)
                .SetStencilRef(1);
            break;
        case PortalStencilMode::None:
        default:
            state.SetStencilEnable(false);
            break;
        }

        auto pipeline = state.SetDepthEnable(true)
            .SetDepthWriteEnable(
                diagnosticDepthEqual ? false :
                stencilMode == PortalStencilMode::ResetDepth ? true :
                stencilMode == PortalStencilMode::Write ||
                stencilMode == PortalStencilMode::Increment
                    ? false : (!overlay && !blend))
            .SetDepthFunc(
                diagnosticDepthEqual ? 2 :
                stencilMode == PortalStencilMode::ResetDepth ? 7 :
                stencilMode == PortalStencilMode::Write ||
                stencilMode == PortalStencilMode::Increment
                ? 3 : ((blend || overlay) ? 3 : 1))
            .SetInputLayout(terrainFormat ? terrainLayout : layout,
                terrainFormat ? 4u : 18u)
            .SetPrimitiveTopology(
                Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(10, 20)
            .Build();
        if (!pipeline)
            throw std::runtime_error(
                std::string("Failed to build ") + description +
                " pipeline: " + materialBuilder->GetLastError());
        return pipeline;
    };

    auto buildMaterialPipelineVariant = [&](bool doubleSided, bool blend,
                                     bool wireframe,
                                     PortalStencilMode stencilMode,
                                     bool colorWriteEnabled,
                                     const char* description,
                                     bool terrainFormat,
                                     bool shadowPass = false)
    {
        auto pipeline = buildConfiguredMaterialPipeline(
            terrainFormat ? terrainVsShader.get() : vsShader.get(),
            shadowPass ? shadowPsShader.get() : psShader.get(),
            doubleSided, blend, wireframe, stencilMode, colorWriteEnabled,
            description, terrainFormat, false, "Auto");
        if (colorWriteEnabled && !shadowPass)
            scene.m_materialShaderBuilders.emplace(pipeline.get(),
                [buildConfiguredMaterialPipeline, doubleSided, blend, wireframe,
                 stencilMode, terrainFormat](const Engine::Graphics::IShader* vertex,
                    const Engine::Graphics::IShader* pixel, bool overlay,
                    const std::string& blendMode)
                {
                    return buildConfiguredMaterialPipeline(vertex, pixel,
                        doubleSided, blend, wireframe, stencilMode, true,
                        "material shader asset", terrainFormat, overlay, blendMode);
                });
        return pipeline;
    };

    auto buildMaterialPipeline = [&](bool doubleSided, bool blend,
                                     bool wireframe,
                                     PortalStencilMode stencilMode,
                                     bool colorWriteEnabled,
                                     const char* description,
                                     bool shadowPass = false)
    {
        auto base = buildMaterialPipelineVariant(doubleSided, blend,
            wireframe, stencilMode, colorWriteEnabled, description, false,
            shadowPass);
        auto terrain = buildMaterialPipelineVariant(doubleSided, blend,
            wireframe, stencilMode, colorWriteEnabled, description, true,
            shadowPass);
        scene.m_terrainPipelineByBase.emplace(base.get(), std::move(terrain));
        return base;
    };

    scene.m_objectPipeline =
        buildMaterialPipeline(false, false, false, PortalStencilMode::None,
            true, "opaque material");
    scene.m_objectDoubleSidedPipeline =
        buildMaterialPipeline(true, false, false, PortalStencilMode::None,
            true,
            "double-sided material");
    scene.m_objectBlendPipeline =
        buildMaterialPipeline(false, true, false, PortalStencilMode::None,
            true, "blended material");
    scene.m_objectBlendDoubleSidedPipeline =
        buildMaterialPipeline(true, true, false, PortalStencilMode::None,
            true,
            "blended double-sided material");

    scene.m_directionalShadowPipeline =
        buildMaterialPipeline(false, false, false, PortalStencilMode::None,
            false, "directional shadow caster", true);
    scene.m_directionalShadowDoubleSidedPipeline =
        buildMaterialPipeline(true, false, false, PortalStencilMode::None,
            false, "double-sided directional shadow caster", true);

    scene.m_objectWirePipeline =
        buildMaterialPipeline(false, false, true, PortalStencilMode::None,
            true,
            "wireframe opaque material");
    scene.m_objectWireDoubleSidedPipeline =
        buildMaterialPipeline(true, false, true, PortalStencilMode::None,
            true,
            "wireframe double-sided material");
    scene.m_objectBlendWirePipeline =
        buildMaterialPipeline(false, true, true, PortalStencilMode::None,
            true,
            "wireframe blended material");
    scene.m_objectBlendWireDoubleSidedPipeline =
        buildMaterialPipeline(true, true, true, PortalStencilMode::None,
            true,
            "wireframe blended double-sided material");

    scene.m_objectPreviewPipeline =
        buildMaterialPipeline(false, true, false, PortalStencilMode::None,
            true, "object preview");
    scene.m_objectPreviewDoubleSidedPipeline =
        buildMaterialPipeline(true, true, false, PortalStencilMode::None,
            true,
            "double-sided object preview");
    scene.m_objectPreviewWirePipeline =
        buildMaterialPipeline(false, true, true, PortalStencilMode::None,
            true,
            "wireframe object preview");
    scene.m_objectPreviewWireDoubleSidedPipeline =
        buildMaterialPipeline(true, true, true, PortalStencilMode::None,
            true,
            "wireframe double-sided object preview");

    scene.m_objectPortalStencilWritePipeline =
        buildMaterialPipeline(true, false, false, PortalStencilMode::Write,
            false, "portal stencil write");
    scene.m_objectPortalStencilIncrementPipeline =
        buildMaterialPipeline(true, false, false, PortalStencilMode::Increment,
            false, "recursive portal stencil increment");
    scene.m_objectPortalDepthResetPipeline =
        buildMaterialPipeline(true, false, false, PortalStencilMode::ResetDepth,
            false, "portal aperture depth reset");
    scene.m_objectPortalStencilReadPipeline =
        buildMaterialPipeline(false, false, false, PortalStencilMode::Read,
            true, "portal stencil read opaque");
    scene.m_objectPortalStencilReadDoubleSidedPipeline =
        buildMaterialPipeline(true, false, false, PortalStencilMode::Read,
            true, "portal stencil read double-sided");
    scene.m_objectPortalStencilReadBlendPipeline =
        buildMaterialPipeline(false, true, false, PortalStencilMode::Read,
            true, "portal stencil read blend");
    scene.m_objectPortalStencilReadBlendDoubleSidedPipeline =
        buildMaterialPipeline(true, true, false, PortalStencilMode::Read,
            true, "portal stencil read blend double-sided");
    scene.m_objectPortalStencilReadWirePipeline =
        buildMaterialPipeline(false, false, true, PortalStencilMode::Read,
            true, "portal stencil read wire");
    scene.m_objectPortalStencilReadWireDoubleSidedPipeline =
        buildMaterialPipeline(true, false, true, PortalStencilMode::Read,
            true, "portal stencil read wire double-sided");
    scene.m_objectPortalStencilReadBlendWirePipeline =
        buildMaterialPipeline(false, true, true, PortalStencilMode::Read,
            true, "portal stencil read wire blend");
    scene.m_objectPortalStencilReadBlendWireDoubleSidedPipeline =
        buildMaterialPipeline(true, true, true, PortalStencilMode::Read,
            true, "portal stencil read wire blend double-sided");
    scene.m_objectPortalStencilReadPreviewPipeline =
        buildMaterialPipeline(false, true, false, PortalStencilMode::Read,
            true, "portal stencil read preview");
    scene.m_objectPortalStencilReadPreviewDoubleSidedPipeline =
        buildMaterialPipeline(true, true, false, PortalStencilMode::Read,
            true, "portal stencil read preview double-sided");
    scene.m_objectPortalStencilReadPreviewWirePipeline =
        buildMaterialPipeline(false, true, true, PortalStencilMode::Read,
            true, "portal stencil read preview wire");
    scene.m_objectPortalStencilReadPreviewWireDoubleSidedPipeline =
        buildMaterialPipeline(true, true, true, PortalStencilMode::Read,
            true, "portal stencil read preview wire double-sided");

    Engine::Scene::Renderer::SceneRendererDebugPipelines::BuildObjectDebugPipelines(
        scene,
        vsShader.get(),
        psShader.get(),
        layout,
        std::size(layout),
        terrainLayout,
        std::size(terrainLayout));
}

}
