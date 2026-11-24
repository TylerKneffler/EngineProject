#include "Core/Rendering/SceneRenderer.h"
#include "Core/Rendering/SceneRenderer/SceneRendererMaterialShaders.h"
#include <chrono>
#include <stdexcept>
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Rendering/RenderUtilities.h"

namespace Engine::Rendering
{
using Scene = Engine::Scene::Scene;
SceneRenderer::MaterialShaderFuture SceneRendererMaterialShaders::QueueMaterialShaderBytecode(
    SceneRenderer& renderer,
    const std::string& path,
    bool terrain,
    int64_t stamp)
{
    const std::string key = path + "|" + (terrain ? "terrain|" : "mesh|") + std::to_string(stamp);
    if (auto found = renderer.m_materialShaderBytecode.find(key);
        found != renderer.m_materialShaderBytecode.end())
        return found->second;

    auto compiler = renderer.m_graphicsProvider->GetShaderCompiler()->CreateWorkerCompiler();
    auto future = Engine::Rendering::RenderJobs::Get().Submit(
        [path, terrain, compiler = std::move(compiler)]() mutable {
            SceneRenderer::MaterialShaderBytecode result;
            const char* vertexEntry = terrain ? "VSTerrainMain" : "VSMain";
            auto vertex = compiler->CompileFromFile(path, vertexEntry,
                Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
            if (!vertex) { result.error = path + ": " + compiler->GetLastError(); return result; }
            auto pixel = compiler->CompileFromFile(path, "PSMain",
                Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
            if (!pixel) { result.error = path + ": " + compiler->GetLastError(); return result; }
            result.vertex = std::move(vertex);
            result.pixel = std::move(pixel);
            return result;
        }).share();
    return renderer.m_materialShaderBytecode.emplace(key, std::move(future)).first->second;
}

const Engine::Rendering::ShaderSourceCache::Source& SceneRendererMaterialShaders::ResolveMaterialShaderSource(
    SceneRenderer& renderer,
    const Engine::Components::Material& material,
    const std::string& path)
{
    Scene& scene = renderer.GetScene();
    scene.AssertOwnerThread();
    static const Engine::Rendering::ShaderSourceCache::Source builtin{};
    if(path.empty()) return builtin;
    const auto before = renderer.m_materialShaderSources.GetStatistics().resolutions;
    const auto& source = renderer.m_materialShaderSources.Resolve(material.GetFilePath(), path,
        [&]{ return material.ShaderSearchPaths(path); });
    renderer.m_lastPrepareTimings.itemShaderResolutions += renderer.m_materialShaderSources.GetStatistics().resolutions - before;
    return source;
}

void SceneRendererMaterialShaders::PrewarmMaterialShaders(
    SceneRenderer& renderer,
    const Engine::Components::Material& material,
    bool terrain)
{
    if (!renderer.m_graphicsProvider || !renderer.m_graphicsProvider->GetShaderCompiler()) return;
    if (material.shader.empty()) return;
    ++renderer.m_lastPrepareTimings.itemShaderRequests;
    const SceneRenderer::PreparedMaterialShaderKey key{ terrain, material.shader, material.GetFilePath() };
    auto& prepared = renderer.m_preparedMaterialShaderSources;
    const auto found = prepared.lower_bound(key);
    if (found != prepared.end() && !prepared.key_comp()(key, *found)) return;
    prepared.insert(found, { terrain, material.shader, material.GetFilePath() });
    const auto& source = ResolveMaterialShaderSource(renderer, material, material.shader);
    if (source.hlsl && source.exists)
        QueueMaterialShaderBytecode(renderer, source.path, terrain, source.revision);
}

void SceneRendererMaterialShaders::PublishMaterialShaderPipelines(SceneRenderer& renderer, size_t maxPipelines)
{
    size_t published = 0;
    for (auto& [key, cached] : renderer.m_materialShaderPipelines)
    {
        if (published >= maxPipelines) break;
        if (!cached.bytecode.valid() || cached.pipeline || !cached.error.empty() ||
            cached.bytecode.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            continue;
        try
        {
            const auto& bytecode = cached.bytecode.get();
            if (!bytecode.error.empty()) cached.error = bytecode.error;
            else if (auto builder = renderer.m_materialShaderBuilders.find(cached.base);
                builder != renderer.m_materialShaderBuilders.end())
                cached.pipeline = builder->second(bytecode.vertex.get(),
                    bytecode.pixel.get(), cached.overlay, cached.blendMode);
            else cached.error = "Material shader is unsupported for this render pass";
            // The new pipeline replaces the one drawn meanwhile (queued frames
            // may still reference it). On an error the old one keeps drawing.
            if (cached.previous && cached.pipeline)
                renderer.m_retiredMaterialShaderPipelines.push_back(std::move(cached.previous));
        }
        catch (const std::exception& exception) { cached.error = exception.what(); }
        ++published;
    }
}

Scene::IPipelineState* SceneRendererMaterialShaders::ResolveMaterialShaderPipeline(
    SceneRenderer& renderer,
    const Engine::Rendering::ShaderSourceCache::Source& requestedSource,
    Scene::IPipelineState* base,
    bool terrain,
    bool overlay,
    const std::string& blendMode,
    std::string& error)
{
    if (requestedSource.path.empty() && !overlay && blendMode == "Auto")
    {
        error.clear();
        return base;
    }
    const auto& source = requestedSource.path.empty()
        ? renderer.m_materialShaderSources.Resolve(EngineShaderPath("Object.hlsl"))
        : requestedSource;
    const std::string& path = source.path;
    const std::string key = std::to_string(reinterpret_cast<uintptr_t>(base)) +
        "|" + (overlay ? "overlay|" : "surface|") + blendMode + "|" + path;
    const int64_t stamp = source.revision;
    auto found = renderer.m_materialShaderPipelines.find(key);
    if (found != renderer.m_materialShaderPipelines.end() && found->second.sourceStamp == stamp)
    {
        ++renderer.m_lastRenderCpuTimings.materialShaderPipelineCacheHits;
        error = found->second.error;
        // While a changed source recompiles, keep drawing the old pipeline.
        return found->second.pipeline ? found->second.pipeline.get() : found->second.previous.get();
    }
    ++renderer.m_lastRenderCpuTimings.materialShaderPipelineCacheMisses;
    auto& cached = renderer.m_materialShaderPipelines[key];
    // The old pipeline keeps drawing until the new one is built (Publish
    // retires it then): a shader edit swaps in place instead of the draws
    // disappearing for the compile.
    if (cached.pipeline)
    {
        if (cached.previous)
            renderer.m_retiredMaterialShaderPipelines.push_back(std::move(cached.previous));
        cached.previous = std::move(cached.pipeline);
    }
    cached.sourceStamp = stamp;
    cached.error.clear();
    cached.base = base;
    cached.overlay = overlay;
    cached.blendMode = blendMode;
    cached.bytecode = {};
    try
    {
        if (!source.hlsl)
            throw std::runtime_error("Material shader must reference an .hlsl asset: " + path);
        if (!source.exists)
            throw std::runtime_error("Material shader asset not found: " + path);
        const auto builder = renderer.m_materialShaderBuilders.find(base);
        if (builder == renderer.m_materialShaderBuilders.end())
            throw std::runtime_error("Material shader is unsupported for this render pass");
        cached.bytecode = QueueMaterialShaderBytecode(renderer, path, terrain, stamp);
    }
    catch (const std::exception& exception)
    {
        cached.error = exception.what();
    }
    error = cached.error;
    return cached.pipeline ? cached.pipeline.get() : cached.previous.get();
}

Scene::IPipelineState* SceneRendererMaterialShaders::ResolveObjectPipeline(
    SceneRenderer& renderer,
    PreparedDraw& draw,
    Engine::Components::Material* material,
    Engine::Components::Mesh* mesh,
    Scene::IPipelineState* basePipeline,
    bool preview,
    bool blended,
    bool sprite,
    bool wireframe,
    bool shadowOnly,
    bool terrain,
    bool terrainCell,
    uint32_t skinJointCount,
    FoliagePipelineCache& foliagePipelines)
{
    auto* pipeline = basePipeline;
    if (!shadowOnly && !preview && !wireframe && !sprite && mesh &&
        !terrain && !terrainCell && !mesh->HasMorphTargets() &&
        !mesh->HasSurfaceOffsets() && !skinJointCount &&
        IsFastFoliageSurface(material))
    {
        auto [entry, inserted] = foliagePipelines.try_emplace(
            std::make_pair(pipeline, material->shader));
        if (inserted)
            entry->second.first = ResolveMaterialShaderPipeline(renderer,
                ResolveMaterialShaderSource(renderer, *material, material->shader),
                pipeline, false, false, "Auto", entry->second.second);
        if (entry->second.first)
        {
            pipeline = entry->second.first;
            draw.surfaceShaderResolved = true;
        }
        else material->shaderError = entry->second.second;
    }
    if (renderer.m_diagnosticOpaqueDepthEqual && !shadowOnly && material &&
        !blended && !preview && !sprite && !terrain && !terrainCell)
    {
        std::string error;
        if (auto* equal = ResolveMaterialShaderPipeline(renderer,
            ResolveMaterialShaderSource(renderer, *material, material->shader),
            pipeline, false, false, "DiagnosticDepthEqual", error))
        {
            pipeline = equal;
            draw.surfaceShaderResolved = true;
        }
        else
        {
            renderer.m_diagnosticOpaqueDepthEqualReady = false;
            if (!error.empty()) throw std::runtime_error(error);
        }
    }
    draw.pipeline = pipeline;
    return pipeline;
}
}
