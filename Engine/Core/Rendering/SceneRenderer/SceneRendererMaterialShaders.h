#pragma once

#include "Core/Rendering/SceneRenderer.h"
#include "Core/Rendering/RenderUtilities.h"
#include "Core/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

#include <map>

namespace Engine::Rendering
{
class SceneRendererMaterialShaders
{
public:
    using Scene = Engine::Scene::Scene;
    using FoliagePipelineCache = std::map<
        std::pair<Scene::IPipelineState*, std::string>,
        std::pair<Scene::IPipelineState*, std::string>>;
    static SceneRenderer::MaterialShaderFuture QueueMaterialShaderBytecode(
        SceneRenderer& renderer, const std::string& path, bool terrain, int64_t stamp);
    static const Engine::Rendering::ShaderSourceCache::Source& ResolveMaterialShaderSource(
        SceneRenderer& renderer, const Engine::Components::Material& material, const std::string& path);
    static void PrewarmMaterialShaders(SceneRenderer& renderer, const Engine::Components::Material& material, bool terrain);
    static void PublishMaterialShaderPipelines(SceneRenderer& renderer, size_t maxPipelines);
    static Scene::IPipelineState* ResolveMaterialShaderPipeline(
        SceneRenderer& renderer,
        const Engine::Rendering::ShaderSourceCache::Source& requestedSource,
        Scene::IPipelineState* base,
        bool terrain,
        bool overlay,
        const std::string& blendMode,
        std::string& error);
    static Scene::IPipelineState* ResolveObjectPipeline(
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
        FoliagePipelineCache& foliagePipelines);
};
}
