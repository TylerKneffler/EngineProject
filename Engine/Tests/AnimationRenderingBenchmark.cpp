#include "Core/Compoonents/Animation/Animation.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Model/ProjectSettings.h"
#include "Core/Object.h"
#include "Core/Renderers/IGameRenderer.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

struct Options
{
    bool render = false;
    int rigs = 1;
    int vertices = 3000;
    int bones = 64;
    int morphs = 0;
    int layers = 0;
    int keys = 32;
    int frames = 300;
    bool maskAll = false;
    const char* api = "DirectX11";
};

struct Rig
{
    Engine::Components::AnimationManager* manager = nullptr;
    Engine::Components::SkinnedMesh* skinned = nullptr;
    Engine::Components::Mesh* mesh = nullptr;
};

double Milliseconds(Clock::time_point first, Clock::time_point second)
{
    return std::chrono::duration<double, std::milli>(second - first).count();
}

double Percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(std::round(
        fraction * static_cast<double>(values.size() - 1u)));
    return values[std::min(index, values.size() - 1u)];
}

double Average(const std::vector<double>& values)
{
    return values.empty() ? 0.0 : std::accumulate(values.begin(), values.end(),
        0.0) / static_cast<double>(values.size());
}

void PumpWindowMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

Engine::Model::AnimationChannel MakeChannel(unsigned node,
    Engine::Model::AnimationChannel::Path path, unsigned width, int keyCount,
    int morphCount = 0)
{
    Engine::Model::AnimationChannel channel;
    channel.nodeIndex = node;
    channel.path = path;
    channel.valueWidth = width;
    channel.interpolation = Engine::Model::AnimationChannel::Interpolation::Linear;
    channel.times.reserve(keyCount);
    channel.values.reserve(static_cast<size_t>(keyCount) * width);
    for (int key = 0; key < keyCount; ++key)
    {
        const float phase = static_cast<float>(key) /
            static_cast<float>(std::max(1, keyCount - 1));
        channel.times.push_back(phase * 2.f);
        if (path == Engine::Model::AnimationChannel::Path::Rotation)
        {
            const float angle = std::sin(phase * 6.283185307f + node * 0.17f) * 0.35f;
            channel.values.insert(channel.values.end(),
                { 0.f, std::sin(angle * 0.5f), 0.f, std::cos(angle * 0.5f) });
        }
        else if (path == Engine::Model::AnimationChannel::Path::Weights)
        {
            for (int morph = 0; morph < morphCount; ++morph)
                channel.values.push_back(0.5f + 0.5f * std::sin(
                    phase * 6.283185307f + morph * 0.71f));
        }
        else
        {
            channel.values.insert(channel.values.end(),
                { std::sin(phase * 6.283185307f + node) * 0.02f,
                  static_cast<float>(node) * 0.01f, 0.f });
        }
    }
    return channel;
}

std::vector<Engine::Model::AnimationVertex> MakeDenseMesh(int requestedVertices,
    int boneCount)
{
    const int vertexCount = std::max(3, requestedVertices / 3 * 3);
    const int triangleCount = vertexCount / 3;
    const int side = static_cast<int>(std::ceil(std::sqrt(
        static_cast<float>(triangleCount))));
    std::vector<Engine::Model::AnimationVertex> vertices(vertexCount);
    for (int triangle = 0; triangle < triangleCount; ++triangle)
    {
        const float x = (static_cast<float>(triangle % side) /
            std::max(1, side - 1) - 0.5f) * 1.6f;
        const float y = (static_cast<float>(triangle / side) /
            std::max(1, side - 1) - 0.5f) * 1.6f;
        const float size = 0.8f / std::max(1, side);
        const glm::vec3 positions[3] = {
            { x - size, y - size, 0.f }, { x + size, y - size, 0.f },
            { x, y + size, 0.f }
        };
        for (int corner = 0; corner < 3; ++corner)
        {
            auto& vertex = vertices[static_cast<size_t>(triangle) * 3u + corner];
            vertex.pos[0] = positions[corner].x;
            vertex.pos[1] = positions[corner].y;
            vertex.pos[2] = positions[corner].z;
            vertex.normal[2] = -1.f;
            vertex.tangent[0] = 1.f;
            vertex.tangent[3] = 1.f;
            vertex.color[0] = 0.3f;
            vertex.color[1] = 0.7f;
            vertex.color[2] = 1.f;
            vertex.color[3] = 1.f;
            vertex.joints0[0] = static_cast<float>(triangle % std::max(1, boneCount));
            vertex.weights0[0] = 1.f;
        }
    }
    return vertices;
}

Rig AddRig(Engine::Scene::Scene& scene, int rigIndex, const Options& options,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    auto* root = scene.AddObject("Animation stress rig");
    const int columns = static_cast<int>(std::ceil(std::sqrt(
        static_cast<float>(options.rigs))));
    root->transform.position = {
        (static_cast<float>(rigIndex % columns) - (columns - 1) * 0.5f) * 2.f,
        (static_cast<float>(rigIndex / columns) - (columns - 1) * 0.5f) * 2.f,
        0.f
    };
    auto* model = root->AddComponent<Engine::Components::Model>();
    auto* animation = root->AddComponent<Engine::Components::Animation>();
    auto* manager = root->AddComponent<Engine::Components::AnimationManager>();
    auto* skeleton = root->AddComponent<Engine::Components::Skeleton>();
    auto* mesh = root->AddComponent<Engine::Components::Mesh>();
    auto* skinned = root->AddComponent<Engine::Components::SkinnedMesh>();
    animation->clipName = "stress";
    animation->duration = 2.f;
    manager->clip = animation->clipName;
    model->BindNode(0, root);

    for (int bone = 0; bone < options.bones; ++bone)
    {
        auto* joint = scene.AddObject("Stress joint");
        joint->Parent = root;
        root->Children.push_back(joint);
        joint->transform.position.y = static_cast<float>(bone) * 0.01f;
        const unsigned node = static_cast<unsigned>(bone + 1);
        model->BindNode(node, joint);
        skeleton->jointNodes.push_back(node);
        skeleton->inverseBindMatrices.emplace_back(1.f);
        animation->channels.push_back(MakeChannel(node,
            Engine::Model::AnimationChannel::Path::Rotation, 4, options.keys));
        animation->channels.push_back(MakeChannel(node,
            Engine::Model::AnimationChannel::Path::Translation, 3, options.keys));
    }

    std::vector<Engine::Model::AnimationVertex> vertices = MakeDenseMesh(
        options.vertices, options.bones);
    mesh->SetDeformedVertices(std::move(vertices));
    if (options.morphs > 0)
    {
        std::vector<Engine::Model::MorphTarget> targets(options.morphs);
        for (int target = 0; target < options.morphs; ++target)
        {
            targets[target].positions.resize(mesh->GetVertexCount());
            for (size_t vertex = 0; vertex < targets[target].positions.size(); ++vertex)
                targets[target].positions[vertex].z = 0.002f *
                    std::sin(static_cast<float>(vertex) * 0.03f + target);
        }
        mesh->SetMorphData(0, std::move(targets),
            std::vector<float>(options.morphs, 0.f));
        animation->channels.push_back(MakeChannel(0,
            Engine::Model::AnimationChannel::Path::Weights,
            static_cast<unsigned>(options.morphs), options.keys, options.morphs));
    }
    skinned->skinIndex = 0;
    for (int layer = 0; layer < options.layers; ++layer)
    {
        Engine::Components::AnimationManager::Layer animationLayer;
        animationLayer.clip = animation->clipName;
        animationLayer.weight = 0.5f;
        animationLayer.additive = (layer & 1) != 0;
        if (options.maskAll)
            for (int bone = 0; bone < options.bones; ++bone)
                animationLayer.nodeMask.push_back(static_cast<unsigned>(bone + 1));
        manager->layers.push_back(std::move(animationLayer));
    }
    if (graphicsProvider)
        mesh->OnAfterDeserialize(graphicsProvider);
    return { manager, skinned, mesh };
}

Options ParseOptions(int count, char** arguments)
{
    Options options;
    for (int i = 1; i < count; ++i)
    {
        const auto value = [&](int fallback)
        {
            return i + 1 < count ? std::max(0, std::atoi(arguments[++i])) : fallback;
        };
        if (std::strcmp(arguments[i], "--render") == 0) options.render = true;
        else if (std::strcmp(arguments[i], "--mask-all") == 0) options.maskAll = true;
        else if (std::strcmp(arguments[i], "--rigs") == 0) options.rigs = value(options.rigs);
        else if (std::strcmp(arguments[i], "--vertices") == 0) options.vertices = value(options.vertices);
        else if (std::strcmp(arguments[i], "--bones") == 0) options.bones = value(options.bones);
        else if (std::strcmp(arguments[i], "--morphs") == 0) options.morphs = value(options.morphs);
        else if (std::strcmp(arguments[i], "--layers") == 0) options.layers = value(options.layers);
        else if (std::strcmp(arguments[i], "--keys") == 0) options.keys = value(options.keys);
        else if (std::strcmp(arguments[i], "--frames") == 0) options.frames = value(options.frames);
        else if (std::strcmp(arguments[i], "--api") == 0 && i + 1 < count) options.api = arguments[++i];
    }
    options.rigs = std::max(1, options.rigs);
    options.vertices = std::max(3, options.vertices);
    options.bones = std::clamp(options.bones, 1, 256);
    options.keys = std::max(2, options.keys);
    options.frames = std::max(1, options.frames);
    return options;
}

void PrintCpuResult(const Options& options, const std::vector<double>& animation,
    const std::vector<double>& morph, const std::vector<double>& palette,
    const std::vector<double>& cpuWork)
{
    std::printf("animation rigs=%d vertices_per_rig=%d bones=%d morphs=%d "
        "layers=%d mask_all=%s keys=%d frames=%d animation_avg_ms=%.3f animation_p95_ms=%.3f "
        "skinned_update_avg_ms=%.3f skinned_update_p95_ms=%.3f palette_avg_ms=%.3f "
        "palette_p95_ms=%.3f total_avg_ms=%.3f total_p95_ms=%.3f\n",
        options.rigs, options.vertices / 3 * 3, options.bones, options.morphs,
        options.layers, options.maskAll ? "true" : "false", options.keys,
        options.frames, Average(animation),
        Percentile(animation, 0.95), Average(morph), Percentile(morph, 0.95),
        Average(palette), Percentile(palette, 0.95), Average(cpuWork),
        Percentile(cpuWork, 0.95));
}
}

int main(int argumentCount, char** arguments)
{
    try
    {
        const Options options = ParseOptions(argumentCount, arguments);
        constexpr uint32_t width = 1280, height = 720;
        std::unique_ptr<Engine::Core::Window> window;
        std::unique_ptr<Engine::Renderers::IGameRenderer> renderer;
        Engine::Scene::Scene scene;
        Engine::Graphics::IGraphicsProvider* graphicsProvider = nullptr;
        if (options.render)
        {
            Engine::Model::ProjectSettings settings{};
            settings.gameRenderingAPI = options.api;
            window = std::make_unique<Engine::Core::Window>(GetModuleHandleW(nullptr),
                L"Animation Rendering Benchmark", width, height);
            renderer = Engine::Renderers::RendererFactory::CreateGameRenderer(settings);
            if (!renderer || !renderer->Init(window->GetHWND(), width, height))
                throw std::runtime_error("renderer initialization failed");
            graphicsProvider = renderer->GetGraphicsProvider();
            scene.Init(graphicsProvider);
            auto* cameraObject = scene.AddObject("Benchmark camera");
            cameraObject->transform.position = { 0.f, 0.f,
                -std::max(8.f, std::sqrt(static_cast<float>(options.rigs)) * 2.2f) };
            cameraObject->AddComponent<Engine::Components::Camera>();
        }

        std::vector<Rig> rigs;
        rigs.reserve(options.rigs);
        for (int rig = 0; rig < options.rigs; ++rig)
            rigs.push_back(AddRig(scene, rig, options, graphicsProvider));
        scene.Start();

        std::vector<double> animationTimes, morphTimes, paletteTimes;
        std::vector<double> cpuWorkTimes;
        std::vector<double> prepareTimes, renderTimes, presentTimes, frameTimes;
        const int warmupFrames = std::min(30, options.frames);
        double gpuOpaqueTotal = 0.0;
        uint32_t gpuSamples = 0;
        uint64_t lastGpuSample = 0;
        uint64_t uploadBytesTotal = 0;
        uint64_t constantWritesTotal = 0;
        for (int frame = -warmupFrames; frame < options.frames; ++frame)
        {
            const auto frameStart = Clock::now();
            for (Rig& rig : rigs) rig.manager->Tick(1.f / 60.f);
            const auto afterAnimation = Clock::now();
            for (Rig& rig : rigs) rig.skinned->Update();
            const auto afterMorph = Clock::now();
            // Render preparation builds and uploads every visible palette.  Do
            // not duplicate that work in rendered measurements; the explicit
            // palette phase is only for the CPU microbenchmark.
            if (!options.render)
                for (Rig& rig : rigs) rig.skinned->BuildPalette();
            const auto afterPalette = Clock::now();
            if (options.render)
            {
                PumpWindowMessages();
                scene.PrepareRenderFrame();
                const auto afterPrepare = Clock::now();
                renderer->BeginFrame();
                renderer->Clear(0.015f, 0.02f, 0.03f);
                auto context = renderer->CreateFrameGraphicsContext();
                if (auto* camera = scene.FindGameCamera(); context && camera)
                    scene.Render(context.get(), static_cast<float>(width) / height,
                        camera, false, width, height);
                const auto afterRender = Clock::now();
                renderer->EndFrame();
                const auto afterPresent = Clock::now();
                if (frame >= 0)
                {
                    prepareTimes.push_back(Milliseconds(afterPalette, afterPrepare));
                    renderTimes.push_back(Milliseconds(afterPrepare, afterRender));
                    presentTimes.push_back(Milliseconds(afterRender, afterPresent));
                    uploadBytesTotal += scene.GetLastObjectDataUploadBytes();
                    if (auto* factory = graphicsProvider->GetContextFactory())
                        constantWritesTotal += factory->GetConstantBufferArenaWrites();
                    const auto telemetry = renderer->GetFrameTimingTelemetry();
                    if (telemetry.gpuTimingsValid && telemetry.gpuSampleId != lastGpuSample)
                    {
                        gpuOpaqueTotal += telemetry.gpuMilliseconds[static_cast<size_t>(
                            Engine::Graphics::GpuTimingStage::Opaque)];
                        lastGpuSample = telemetry.gpuSampleId;
                        ++gpuSamples;
                    }
                }
            }
            const auto frameEnd = Clock::now();
            if (frame >= 0)
            {
                animationTimes.push_back(Milliseconds(frameStart, afterAnimation));
                morphTimes.push_back(Milliseconds(afterAnimation, afterMorph));
                paletteTimes.push_back(Milliseconds(afterMorph, afterPalette));
                cpuWorkTimes.push_back(Milliseconds(frameStart, afterPalette));
                frameTimes.push_back(Milliseconds(frameStart, frameEnd));
            }
        }

        PrintCpuResult(options, animationTimes, morphTimes, paletteTimes,
            cpuWorkTimes);
        if (options.render)
        {
            std::printf("render api=%s rigs=%d total_vertices=%lld frame_avg_ms=%.3f "
                "frame_p95_ms=%.3f frame_p99_ms=%.3f prepare_avg_ms=%.3f "
                "render_submit_avg_ms=%.3f present_avg_ms=%.3f gpu_opaque_avg_ms=%.3f "
                "gpu_samples=%u object_upload_avg_bytes=%.0f constant_writes_avg=%.1f "
                "ordinary_draws=%u skinned_objects=%u\n",
                options.api, options.rigs,
                static_cast<long long>(options.rigs) * (options.vertices / 3 * 3),
                Average(frameTimes), Percentile(frameTimes, 0.95),
                Percentile(frameTimes, 0.99), Average(prepareTimes),
                Average(renderTimes), Average(presentTimes),
                gpuOpaqueTotal / std::max(1u, gpuSamples), gpuSamples,
                static_cast<double>(uploadBytesTotal) / options.frames,
                static_cast<double>(constantWritesTotal) / options.frames,
                scene.GetLastOrdinaryDrawCount(),
                scene.GetLastSkinnedObjectCount());
            renderer->WaitIdle();
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "animation rendering benchmark failed: %s\n", error.what());
        return 1;
    }
}
