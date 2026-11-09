#include "Core/Scene/Rendering/SceneRenderer/SceneRendererMaterialShaders.h"
#include <chrono>
#include <stdexcept>
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"

namespace Engine::Scene::Renderer
{
Scene::MaterialShaderFuture SceneRendererMaterialShaders::QueueMaterialShaderBytecode(
    Scene& scene,
    const std::string& path,
    bool terrain,
    int64_t stamp)
{
    const std::string key = path + "|" + (terrain ? "terrain|" : "mesh|") + std::to_string(stamp);
    if (auto found = scene.m_materialShaderBytecode.find(key);
        found != scene.m_materialShaderBytecode.end())
        return found->second;

    auto compiler = scene.m_graphicsProvider->GetShaderCompiler()->CreateWorkerCompiler();
    auto future = Engine::Rendering::RenderJobs::Get().Submit(
        [path, terrain, compiler = std::move(compiler)]() mutable {
            Scene::MaterialShaderBytecode result;
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
    return scene.m_materialShaderBytecode.emplace(key, std::move(future)).first->second;
}

const Engine::Rendering::ShaderSourceCache::Source& SceneRendererMaterialShaders::ResolveMaterialShaderSource(
    Scene& scene,
    const Engine::Components::Material& material,
    const std::string& path)
{
    scene.AssertOwnerThread();
    static const Engine::Rendering::ShaderSourceCache::Source builtin{};
    if(path.empty()) return builtin;
    const auto before = scene.m_materialShaderSources.GetStatistics().resolutions;
    const auto& source = scene.m_materialShaderSources.Resolve(material.GetFilePath(), path,
        [&]{ return material.ShaderSearchPaths(path); });
    scene.m_lastPrepareTimings.itemShaderResolutions += scene.m_materialShaderSources.GetStatistics().resolutions - before;
    return source;
}

void SceneRendererMaterialShaders::PrewarmMaterialShaders(
    Scene& scene,
    const Engine::Components::Material& material,
    bool terrain)
{
    if (!scene.m_graphicsProvider || !scene.m_graphicsProvider->GetShaderCompiler()) return;
    if (material.shader.empty()) return;
    ++scene.m_lastPrepareTimings.itemShaderRequests;
    if (!scene.m_preparedMaterialShaderSources.emplace(material.GetFilePath(), material.shader, terrain).second) return;
    const auto& source = ResolveMaterialShaderSource(scene, material, material.shader);
    if (source.hlsl && source.exists)
        QueueMaterialShaderBytecode(scene, source.path, terrain, source.revision);
}

void SceneRendererMaterialShaders::PublishMaterialShaderPipelines(Scene& scene, size_t maxPipelines)
{
    size_t published = 0;
    for (auto& [key, cached] : scene.m_materialShaderPipelines)
    {
        if (published >= maxPipelines) break;
        if (!cached.bytecode.valid() || cached.pipeline || !cached.error.empty() ||
            cached.bytecode.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            continue;
        try
        {
            const auto& bytecode = cached.bytecode.get();
            if (!bytecode.error.empty()) cached.error = bytecode.error;
            else if (auto builder = scene.m_materialShaderBuilders.find(cached.base);
                builder != scene.m_materialShaderBuilders.end())
                cached.pipeline = builder->second(bytecode.vertex.get(),
                    bytecode.pixel.get(), cached.overlay, cached.blendMode);
            else cached.error = "Material shader is unsupported for this render pass";
        }
        catch (const std::exception& exception) { cached.error = exception.what(); }
        ++published;
    }
}

Scene::IPipelineState* SceneRendererMaterialShaders::ResolveMaterialShaderPipeline(
    Scene& scene,
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
        ? scene.m_materialShaderSources.Resolve(EngineShaderPath("Object.hlsl"))
        : requestedSource;
    const std::string& path = source.path;
    const std::string key = std::to_string(reinterpret_cast<uintptr_t>(base)) +
        "|" + (overlay ? "overlay|" : "surface|") + blendMode + "|" + path;
    const int64_t stamp = source.revision;
    auto found = scene.m_materialShaderPipelines.find(key);
    if (found != scene.m_materialShaderPipelines.end() && found->second.sourceStamp == stamp)
    {
        error = found->second.error;
        return found->second.pipeline.get();
    }
    auto& cached = scene.m_materialShaderPipelines[key];
    if (cached.pipeline)
        scene.m_retiredMaterialShaderPipelines.push_back(std::move(cached.pipeline));
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
        const auto builder = scene.m_materialShaderBuilders.find(base);
        if (builder == scene.m_materialShaderBuilders.end())
            throw std::runtime_error("Material shader is unsupported for this render pass");
        cached.bytecode = QueueMaterialShaderBytecode(scene, path, terrain, stamp);
    }
    catch (const std::exception& exception)
    {
        cached.error = exception.what();
    }
    error = cached.error;
    return cached.pipeline.get();
}

Scene::IPipelineState* SceneRendererMaterialShaders::ResolveObjectPipeline(
    Scene& scene,
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
            entry->second.first = ResolveMaterialShaderPipeline(scene,
                ResolveMaterialShaderSource(scene, *material, material->shader),
                pipeline, false, false, "Auto", entry->second.second);
        if (entry->second.first)
        {
            pipeline = entry->second.first;
            draw.surfaceShaderResolved = true;
        }
        else material->shaderError = entry->second.second;
    }
    if (scene.m_diagnosticOpaqueDepthEqual && !shadowOnly && material &&
        !blended && !preview && !sprite && !terrain && !terrainCell)
    {
        std::string error;
        if (auto* equal = ResolveMaterialShaderPipeline(scene,
            ResolveMaterialShaderSource(scene, *material, material->shader),
            pipeline, false, false, "DiagnosticDepthEqual", error))
        {
            pipeline = equal;
            draw.surfaceShaderResolved = true;
        }
        else
        {
            scene.m_diagnosticOpaqueDepthEqualReady = false;
            if (!error.empty()) throw std::runtime_error(error);
        }
    }
    draw.pipeline = pipeline;
    return pipeline;
}
}
