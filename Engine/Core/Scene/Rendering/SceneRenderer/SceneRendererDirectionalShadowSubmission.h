#pragma once

#include "Core/Scene/Rendering/SceneRenderer/SceneRendererGeometrySubmission.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererLodSelection.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererShadows.h"
#include "Core/Rendering/DrawBatchPreparation.h"

#include <array>
#include <chrono>
#include <functional>
#include <vector>

namespace Engine::Scene::Renderer
{
struct DirectionalShadowSubmissionContext
{
    using AutomaticGroup = Engine::Rendering::DrawBatchPreparation::Group;
    using SubmitAutomaticGroup = std::function<SubmittedGeometry(
        const AutomaticGroup&, const glm::mat4&, uint32_t,
        Engine::Graphics::IPipelineState*, const std::vector<uint8_t>*)>;

    Scene& scene;
    Engine::Graphics::IGraphicsContext* graphics;
    const std::vector<PreparedDraw>& draws;
    const std::array<std::vector<uint8_t>, 4>& cascadeVisible;
    const std::vector<uint8_t>& cascadeMasks;
    const std::array<uint8_t, 4>& dirtyMask;
    uint32_t cascadeCount;
    uint32_t atlasResolution;
    bool preserveAtlas;
    const SceneRendererShadows::ShadowCascadeSet& cascades;
    const Engine::Model::GraphicsQualityValues& quality;
    const Engine::Model::RealtimeShadowSelection& shadowSelection;
    const glm::vec3& cameraPosition;
    const std::vector<AutomaticGroup>& automaticGroups;
    const SceneRendererLodSelection& lodSelection;
    SceneRendererGeometrySubmission& geometry;
    SubmitAutomaticGroup submitAutomaticGroup;
    std::chrono::steady_clock::time_point recordsUploadedAt;
};

class SceneRendererDirectionalShadowSubmission
{
public:
    static void Submit(DirectionalShadowSubmissionContext& submission);
};
}
