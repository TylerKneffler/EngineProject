#pragma once

#include "Core/Scene/Scene.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

#include <map>

namespace Engine::Scene::Renderer
{
class SceneRendererMaterialShaders
{
public:
    using FoliagePipelineCache = std::map<
        std::pair<Scene::IPipelineState*, std::string>,
        std::pair<Scene::IPipelineState*, std::string>>;
    static Scene::MaterialShaderFuture QueueMaterialShaderBytecode(
        Scene& scene, const std::string& path, bool terrain, int64_t stamp);
    static const Engine::Rendering::ShaderSourceCache::Source& ResolveMaterialShaderSource(
        Scene& scene, const Engine::Components::Material& material, const std::string& path);
    static void PrewarmMaterialShaders(Scene& scene, const Engine::Components::Material& material, bool terrain);
    static void PublishMaterialShaderPipelines(Scene& scene, size_t maxPipelines);
    static Scene::IPipelineState* ResolveMaterialShaderPipeline(
        Scene& scene,
        const Engine::Rendering::ShaderSourceCache::Source& requestedSource,
        Scene::IPipelineState* base,
        bool terrain,
        bool overlay,
        const std::string& blendMode,
        std::string& error);
    static Scene::IPipelineState* ResolveObjectPipeline(
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
        FoliagePipelineCache& foliagePipelines);
};
}
