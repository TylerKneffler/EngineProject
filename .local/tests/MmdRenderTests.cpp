#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Physics/RigPhysicsBinding.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Physics/Adapters/BulletPhysicsAdapter.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Object.h"
#include "Core/Model/ProjectSettings.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/DX11/DX11EditorRenderer.h"
#include "Core/Renderers/IView.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include <algorithm>
#include <Windows.h>
#include <Psapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <chrono>
#pragma comment(lib, "Psapi.lib")

static void Checkpoint(const char* text)
{
    PROCESS_MEMORY_COUNTERS_EX info{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info));
    std::cout << text << ": working " << info.WorkingSetSize / (1024 * 1024)
              << " MiB, private " << info.PrivateUsage / (1024 * 1024) << " MiB\n" << std::flush;
}

static void SaveBmp(const std::vector<uint8_t>& rgba, unsigned width, unsigned height, const char* path)
{
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + width * height * 4;
    info.biSize = sizeof(info); info.biWidth = width; info.biHeight = height;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (int y = static_cast<int>(height) - 1; y >= 0; --y)
        for (unsigned x = 0; x < width; ++x)
        {
            const auto* p = rgba.data() + (static_cast<size_t>(y) * width + x) * 4;
            const uint8_t bgra[] = { p[2], p[1], p[0], p[3] };
            out.write(reinterpret_cast<const char*>(bgra), 4);
        }
}

static uint64_t CheckMorphBuffers(Engine::Core::Object* object)
{
    uint64_t staticBytes = 0;
    if (auto* mesh = object->GetComponent<Engine::Components::Mesh>())
        if (auto* deltas = mesh->GetMorphDeltaBuffer())
        {
            if (deltas->GetUploadShadowSize() != 0 ||
                deltas->GetAccessMode() != Engine::Graphics::IGraphicsBuffer::AccessMode::Default)
                throw std::runtime_error("Static DX11 morph deltas retain an upload shadow");
            auto* weights = mesh->GetMorphWeightBuffer();
            if (!weights || weights->GetAccessMode() != Engine::Graphics::IGraphicsBuffer::AccessMode::Upload)
                throw std::runtime_error("Animated morph weights are not writable");
            staticBytes += deltas->GetSize();
        }
    return staticBytes;
}

// Set MMD_EDITOR_PLAY_PROFILE=scene, game, or both to exercise the DX11 editor
// swap chain, offscreen view, wall-clock updates, and continuous Present loop.
// MMD_EDITOR_PLAY_FRAMES extends the run; MMD_EDITOR_PLAY_CASCADES compares the
// playback shadow budget. This mode does not draw the ImGui editor chrome.
// The capture test below remains deterministic and uses the export renderer.
static int ProfileEditorPlay(const char* viewMode)
{
    using Clock = std::chrono::steady_clock;
    using Engine::Renderers::DX11EditorRenderer;
    constexpr unsigned windowWidth = 1536, windowHeight = 900;
    constexpr unsigned viewWidth = 783, viewHeight = 721;
    constexpr unsigned idleFrames = 30;
    const char* frameOverride = std::getenv("MMD_EDITOR_PLAY_FRAMES");
    const unsigned playFrames = frameOverride
        ? static_cast<unsigned>(std::clamp(std::atoi(frameOverride), 30, 1200))
        : 180u;
    const std::string mode = viewMode && *viewMode ? viewMode : "scene";
    if (mode != "scene" && mode != "game" && mode != "both")
        throw std::runtime_error("MMD_EDITOR_PLAY_PROFILE must be scene, game, or both");

    Engine::Core::Window window(GetModuleHandleW(nullptr),
        L"MMD hidden editor Play profile", windowWidth, windowHeight);
    DX11EditorRenderer renderer;
    if (!renderer.Init(window.GetHWND(), windowWidth, windowHeight))
        throw std::runtime_error("DX11 editor initialization failed");
    auto makeView = [&]()
    {
        auto [handles, slot] = renderer.AllocateSrvSlot();
        auto view = renderer.CreateViewBackend();
        view->Init(renderer.GetNativeDeviceHandle(), viewWidth, viewHeight,
            handles.first, handles.second, slot);
        return view;
    };
    auto sceneView = mode == "game" ? nullptr : makeView();
    auto gameView = mode == "scene" ? nullptr : makeView();

    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    const char* selectedScene = std::getenv("MMD_SCENE");
    if (!scene.Load(selectedScene ? selectedScene :
        "Engine/Core/Assets/Scenes/Animation/mmd.scene"))
        throw std::runtime_error("Scene load failed");
    Engine::Model::RealtimeShadowSettings shadows;
    scene.SetRealtimeShadowSettings(shadows);
    Engine::Model::ProjectSettings projectSettings;
    scene.SetDistanceLightingSettings(projectSettings.distanceLighting);
    auto* bullet = dynamic_cast<Engine::Physics::BulletPhysicsAdapter*>(
        &scene.GetPhysics().GetAdapter());
    if (!bullet) throw std::runtime_error("MMD scene lacks Bullet physics");

    struct FrameSample
    {
        double deltaMs = 0, updateMs = 0, renderMs = 0, frameMs = 0;
        uint32_t substeps = 0, shadowDraws = 0;
    };
    std::vector<FrameSample> idle, playing;
    auto lastTick = Clock::now();
    auto drawView = [&](Engine::Renderers::IView& view, bool editorCamera)
    {
        auto* factory = renderer.GetGraphicsProvider()->GetContextFactory();
        void* cmd = renderer.GetCurrentCommandBuffer();
        auto renderScene = [&](void* command, bool shadowsOnly)
        {
            factory->SetCommandBuffer(command);
            auto context = factory->CreateContext();
            Engine::Components::Camera* camera = editorCamera
                ? nullptr : scene.FindGameCamera();
            if (!editorCamera && !camera)
                throw std::runtime_error("MMD scene has no Game camera");
            scene.Render(context.get(), view.GetAspect(), camera, editorCamera,
                view.GetWidth(), view.GetHeight(), shadowsOnly);
        };
        view.Render(cmd, renderer.GetCurrentRenderTargetHandle(),
            [&](void* command) { renderScene(command, false); },
            [&](void* command) { renderScene(command, true); });
    };
    const auto tick = [&](bool play)
    {
        const auto begin = Clock::now();
        const float delta = std::chrono::duration<float>(begin - lastTick).count();
        lastTick = begin;
        FrameSample sample;
        sample.deltaMs = delta * 1000.0;
        if (play)
        {
            scene.Update(std::max(delta, 0.f));
            sample.substeps = bullet->GetLastSubstepCount();
        }
        const auto updated = Clock::now();
        renderer.MarkDirty();
        renderer.RenderIfNeeded([&]
        {
            renderer.Clear(.18f, .18f, .18f);
            scene.PrepareRenderFrame();
            if (sceneView)
            {
                scene.UpdateEditorCameraFollow();
                drawView(*sceneView, true);
                sample.shadowDraws += scene.GetShadowDebugSnapshot().submittedCasterDrawCount;
            }
            if (gameView)
            {
                drawView(*gameView, false);
                sample.shadowDraws += scene.GetShadowDebugSnapshot().submittedCasterDrawCount;
            }
        });
        const auto end = Clock::now();
        sample.updateMs = std::chrono::duration<double, std::milli>(updated - begin).count();
        sample.renderMs = std::chrono::duration<double, std::milli>(end - updated).count();
        sample.frameMs = std::chrono::duration<double, std::milli>(end - begin).count();
        (play ? playing : idle).push_back(sample);
    };
    for (unsigned i = 0; i < idleFrames; ++i) tick(false);
    const auto transitionStart = Clock::now();
    const auto snapshot = scene.SaveToString();
    scene.Start();
    const double startMs = std::chrono::duration<double, std::milli>(
        Clock::now() - transitionStart).count();
    const char* cascadeOverride = std::getenv("MMD_EDITOR_PLAY_CASCADES");
    shadows.directionalCascadeCount = cascadeOverride
        ? static_cast<uint32_t>(std::clamp(std::atoi(cascadeOverride), 1, 4))
        : shadows.directionalCascadeCount;
    scene.SetRealtimeShadowSettings(shadows);
    for (unsigned i = 0; i < playFrames; ++i) tick(true);
    const auto stopStart = Clock::now();
    if (!scene.LoadFromString(snapshot))
        throw std::runtime_error("Play snapshot restore failed");
    const double stopMs = std::chrono::duration<double, std::milli>(
        Clock::now() - stopStart).count();

    const auto report = [](const char* name, const std::vector<FrameSample>& samples)
    {
        const size_t skip = std::min<size_t>(5, samples.size());
        auto metric = [&](auto pick)
        {
            std::vector<double> values;
            for (size_t i = skip; i < samples.size(); ++i)
                values.push_back(pick(samples[i]));
            std::sort(values.begin(), values.end());
            return std::pair{ values[values.size() / 2],
                values[std::min(values.size() - 1, values.size() * 95 / 100)] };
        };
        const auto [frame50, frame95] = metric([](const auto& s) { return s.frameMs; });
        const auto [update50, update95] = metric([](const auto& s) { return s.updateMs; });
        const auto [render50, render95] = metric([](const auto& s) { return s.renderMs; });
        const auto [delta50, delta95] = metric([](const auto& s) { return s.deltaMs; });
        const auto [steps50, steps95] = metric([](const auto& s) { return double(s.substeps); });
        std::cout << name << ": frame p50/p95 " << frame50 << '/' << frame95
            << " ms, update " << update50 << '/' << update95
            << " ms, render+Present " << render50 << '/' << render95
            << " ms, input dt " << delta50 << '/' << delta95
            << " ms, physics steps " << steps50 << '/' << steps95
            << ", shadow draws " << samples.back().shadowDraws << '\n';
    };
    std::cout << "Editor Play profile: DX11 " << mode << " view "
        << viewWidth << 'x' << viewHeight << ", window "
        << windowWidth << 'x' << windowHeight << ", Play cascades "
        << shadows.directionalCascadeCount << '\n';
    report("Idle continuous redraw", idle);
    std::cout << "Play snapshot + Start " << startMs << " ms\n";
    report("Playing", playing);
    if (playing.size() >= 240)
    {
        report("Playing first 120", { playing.begin(), playing.begin() + 120 });
        report("Playing last 120", { playing.end() - 120, playing.end() });
    }
    std::cout << "Play restore " << stopMs << " ms\n";
    return 0;
}

int main()
{
    try
    {
        if (const char* editorProfile = std::getenv("MMD_EDITOR_PLAY_PROFILE"))
            return ProfileEditorPlay(editorProfile);
        constexpr unsigned width = 960, height = 540;
        Engine::Core::Window window(GetModuleHandleW(nullptr), L"MMD hidden playback test", width, height);
        Engine::Model::ProjectSettings settings;
        settings.gameRenderingAPI = "DirectX11";
        auto renderer = Engine::Renderers::RendererFactory::CreateGameRenderer(settings);
        if (!renderer || !renderer->Init(window.GetHWND(), width, height)) throw std::runtime_error("DX11 initialization failed");
        auto* dx = dynamic_cast<Engine::Renderers::DX11GameRenderer*>(renderer.get());
        if (!dx || !dx->EnableOffscreenExport(width, height, 1, 2)) throw std::runtime_error("Offscreen capture initialization failed");
        Engine::Scene::Scene scene;
        Checkpoint("Before scene initialization");
        scene.Init(renderer->GetGraphicsProvider());
        Checkpoint("Before scene load");
        const char* selectedScene = std::getenv("MMD_SCENE");
        if (!scene.Load(selectedScene ? selectedScene :
            "Engine/Core/Assets/Scenes/Animation/mmd.scene"))
            throw std::runtime_error("Scene load failed");
        if (std::getenv("MMD_PROFILE_ONE_CASCADE"))
        {
            Engine::Model::RealtimeShadowSettings shadows;
            shadows.directionalCascadeCount = 1;
            scene.SetRealtimeShadowSettings(shadows);
        }
        Checkpoint("After scene load");
        uint64_t totalVertices = 0, totalIndices = 0;
        uint64_t cpuMeshBytes = 0, uploadShadowBytes = 0;
        size_t renderMeshes = 0, morphMeshes = 0;
        for (const auto& object : scene.GetObjects())
            if (auto* mesh = object->GetComponent<Engine::Components::Mesh>())
            {
                ++renderMeshes;
                totalVertices += mesh->GetVertexCount();
                totalIndices += mesh->GetIndexCount();
                morphMeshes += mesh->HasMorphTargets();
                cpuMeshBytes += mesh->GetCpuMeshMemoryBytes();
                uploadShadowBytes += mesh->GetUploadShadowMemoryBytes();
            }
        std::cout << "Scene geometry: " << renderMeshes << " meshes, "
            << totalVertices << " vertices, " << totalIndices << " indices, "
            << morphMeshes << " morph meshes; CPU mesh "
            << cpuMeshBytes / (1024 * 1024) << " MiB, upload shadows "
            << uploadShadowBytes / (1024 * 1024) << " MiB\n";
        size_t dynamicBodies = 0;
        size_t physicsJoints = 0;
        for (const auto& object : scene.GetObjects())
        {
            if (auto* body = object->GetComponent<Engine::Components::RigidBody>())
                dynamicBodies += body->bodyType == "Dynamic";
            if (auto* rig = object->GetComponent<Engine::Components::RigPhysicsBinding>())
                physicsJoints += rig->physicsJoints.size();
        }
        if (dynamicBodies != 133 || physicsJoints != 133)
            throw std::runtime_error("Mococo scene lost its PMX dynamic bodies or joints");
        size_t texturedMaterials = 0;
        for (const auto& object : scene.GetObjects())
            if (auto* material = object->GetComponent<Engine::Components::Material>())
                if (material->baseColorTexture)
                {
                    if (!std::filesystem::is_regular_file(
                        material->baseColorTexture->GetFilePath()))
                        throw std::runtime_error("Model material has a missing texture");
                    ++texturedMaterials;
                }
        if (texturedMaterials == 0)
            throw std::runtime_error("Model has no textured materials");
        uint64_t staticMorphBytes = 0;
        for (const auto& object : scene.GetObjects()) staticMorphBytes += CheckMorphBuffers(object.get());
        if (!staticMorphBytes) throw std::runtime_error("MMD scene has no GPU morph buffers");
        if (!selectedScene)
        {
            if (totalVertices >= 300000 || totalIndices < 1000000)
                throw std::runtime_error("Bundled Mococo meshes are not indexed");
            if (uploadShadowBytes >= 5ull * 1024 * 1024)
                throw std::runtime_error("Static MMD vertex buffers retain DX11 upload shadows");
            if (staticMorphBytes >= 160ull * 1024 * 1024)
                throw std::runtime_error("Position-only MMD morphs use an expanded GPU layout");
            size_t releasedLargeTextures = 0;
            for (const auto& object : scene.GetObjects())
                if (auto* material = object->GetComponent<Engine::Components::Material>())
                    if (const auto& texture = material->baseColorTexture;
                        texture && texture->GetGraphicsTexture() &&
                        texture->GetWidth() >= 2048)
                    {
                        if (texture->HasPixels())
                            throw std::runtime_error("Prepared MMD texture retains decoded pixels");
                        ++releasedLargeTextures;
                    }
            if (releasedLargeTextures < 6)
                throw std::runtime_error("MMD texture release check covered too few materials");
            for (const auto& object : scene.GetObjects())
                if (auto* material = object->GetComponent<Engine::Components::Material>())
                    if (const auto& texture = material->baseColorTexture;
                        texture && texture->GetGraphicsTexture() &&
                        texture->GetWidth() <= 512)
                    {
                        if (texture->GetPixels().size() <
                            static_cast<size_t>(texture->GetWidth()) *
                                texture->GetHeight() * 4)
                            throw std::runtime_error("Released texture did not decode on demand");
                        break;
                    }
        }
        std::cout << "Static morph upload shadows avoided: " << staticMorphBytes / (1024 * 1024) << " MiB\n";
        const auto snapshot = scene.SaveToString();
        Checkpoint("After editor Play snapshot");
        scene.Start();
        Checkpoint("After Play start");
        std::filesystem::create_directories(".local/test-output/mmd-render");
        const auto render = [&]()
        {
            const auto begin = std::chrono::steady_clock::now();
            scene.PrepareRenderFrame();
            const auto prepared = std::chrono::steady_clock::now();
            dx->BeginExportFrame(1);
            renderer->BeginFrame();
            auto context = renderer->CreateFrameGraphicsContext();
            auto* camera = scene.FindGameCamera();
            if (!context || !camera) throw std::runtime_error("Missing graphics context or camera");
            scene.Render(context.get(), float(width) / height, camera, false, width, height, true);
            const auto shadowed = std::chrono::steady_clock::now();
            const auto shadow = scene.GetShadowDebugSnapshot();
            renderer->Clear(.025f, .025f, .035f);
            scene.Render(context.get(), float(width) / height, camera, false, width, height, false);
            const auto drawn = std::chrono::steady_clock::now();
            renderer->EndFrame();
            renderer->WaitIdle();
            const auto complete = std::chrono::steady_clock::now();
            if (std::getenv("MMD_PROFILE"))
            {
                const auto ms = [](auto first, auto last) {
                    return std::chrono::duration<double, std::milli>(last - first).count();
                };
                std::cout << "Render profile: prepare " << ms(begin, prepared)
                    << " ms, shadow submit " << ms(prepared, shadowed)
                    << " ms, main submit " << ms(shadowed, drawn)
                    << " ms, EndFrame/WaitIdle " << ms(drawn, complete)
                    << " ms; " << shadow.casterCount << " casters x "
                    << shadow.cascadeCount << " cascades\n";
            }
        };
        const bool sphereOnlyAudit = std::getenv("MMD_SPHERE_VISUAL_AUDIT") != nullptr;
        if (sphereOnlyAudit || std::getenv("MMD_MATERIAL_VISUAL_AUDIT"))
        {
            render();
            std::vector<uint8_t> baseline;
            if (!dx->ReadExportFrameRGBA(baseline, true))
                throw std::runtime_error("Baseline material capture failed");
            std::vector<Engine::Components::Material*> affected;
            for (const auto& object : scene.GetObjects())
                if (auto* material = object->GetComponent<Engine::Components::Material>())
                    if (material->baseColorTexture)
                        affected.push_back(material);
            if (affected.empty()) throw std::runtime_error("No material to audit");
            const auto compare = [&](const char* effect, size_t minimum)
            {
                render();
                std::vector<uint8_t> shaded;
                if (!dx->ReadExportFrameRGBA(shaded, true) ||
                    shaded.size() != baseline.size())
                    throw std::runtime_error("PMX material capture failed");
                size_t changedPixels = 0;
                for (size_t pixel = 0; pixel < shaded.size(); pixel += 4)
                    if (std::abs(int(shaded[pixel]) - int(baseline[pixel])) +
                        std::abs(int(shaded[pixel + 1]) - int(baseline[pixel + 1])) +
                        std::abs(int(shaded[pixel + 2]) - int(baseline[pixel + 2])) > 24)
                        ++changedPixels;
                if (changedPixels < minimum)
                    throw std::runtime_error(std::string(effect) +
                        " did not change the rendered image");
                std::cout << effect << " pixels: " << changedPixels << '\n';
                return shaded;
            };
            if (!sphereOnlyAudit)
            {
                for (auto* material : affected)
                {
                    material->edgeEnabled = true;
                    material->edgeColor = { 0.f, 0.f, 0.f, 1.f };
                    material->edgeWidth = 6.f;
                }
                compare("PMX edge", 100);
            }
            for (auto* material : affected)
            {
                material->edgeEnabled = false;
                material->SetSphereTexture(
                    material->baseColorTexture->GetFilePath());
                material->sphereMode = 1;
                material->sphereTint = { .7f, .8f, 1.f, 1.f };
            }
            const auto multiplied = compare("PMX sphere multiply", 1000);
            for (auto* material : affected) material->sphereMode = 2;
            const auto added = compare("PMX sphere additive", 1000);
            for (auto* material : affected) material->sphereMode = 3;
            const auto additionalUv = compare("PMX sphere additional UV", 1000);
            const auto modeDifference = [](const auto& first, const auto& second)
            {
                size_t changed = 0;
                for (size_t pixel = 0; pixel < first.size(); pixel += 4)
                    if (std::abs(int(first[pixel]) - int(second[pixel])) +
                        std::abs(int(first[pixel + 1]) - int(second[pixel + 1])) +
                        std::abs(int(first[pixel + 2]) - int(second[pixel + 2])) > 24)
                        ++changed;
                return changed;
            };
            if (modeDifference(multiplied, added) < 1000 ||
                modeDifference(multiplied, additionalUv) < 1000)
                throw std::runtime_error("PMX sphere modes rendered identically");
            if (!sphereOnlyAudit)
            {
                for (auto* material : affected)
                {
                    material->sphereMode = 0;
                    material->SetToonTexture(
                        material->baseColorTexture->GetFilePath());
                    material->toonTint = { 1.f, .6f, .6f, 1.f };
                }
                compare("PMX toon", 1000);
            }
            return 0;
        }
        if (std::getenv("MMD_RENDER_VISUAL_AUDIT"))
        {
            bool flipped = false;
            for (int side : {1, -1})
                for (bool flip : {false, true})
                {
                    if (flip != flipped)
                    {
                        for (const auto& object : scene.GetObjects())
                            if (auto* mesh = object->GetComponent<Engine::Components::Mesh>())
                            {
                                auto vertices = mesh->GetVertices();
                                for (auto& vertex : vertices)
                                {
                                    vertex.uv[1] = 1.f - vertex.uv[1];
                                    vertex.uv1[1] = 1.f - vertex.uv1[1];
                                    vertex.tangent[3] *= -1.f;
                                }
                                mesh->UpdateAuthoredVertices(std::move(vertices));
                            }
                        flipped = flip;
                    }
                    auto* camera = scene.FindGameCamera();
                    camera->Owner->transform.position.z = side * 2.7f;
                    render();
                    std::vector<uint8_t> rgba;
                    if (!dx->ReadExportFrameRGBA(rgba, true)) throw std::runtime_error("Audit capture failed");
                    const std::string path = std::string(".local/test-output/mmd-render/audit-") +
                        (side > 0 ? "positive-z" : "negative-z") + (flip ? "-flipped.bmp" : "-original.bmp");
                    SaveBmp(rgba, width, height, path.c_str());
                    std::cout << "Captured " << path << '\n' << std::flush;
                }
            return 0;
        }
        auto* bullet = dynamic_cast<Engine::Physics::BulletPhysicsAdapter*>(
            &scene.GetPhysics().GetAdapter());
        if (!bullet) throw std::runtime_error("MMD scene lacks Bullet physics");
        uint64_t initialJointBuilds = 0;
        double updateMilliseconds = 0.0;
        double maxUpdateMilliseconds = 0.0;
        for (unsigned frame = 0; frame < 660; ++frame)
        {
            const auto updateBegin = std::chrono::steady_clock::now();
            scene.Update(1.f / 60.f);
            const double updateMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - updateBegin).count();
            if (frame >= 60)
            {
                updateMilliseconds += updateMs;
                maxUpdateMilliseconds = std::max(maxUpdateMilliseconds, updateMs);
            }
            if (frame == 0)
                initialJointBuilds = bullet->GetImportedJointRebuildCount();
            if (frame != 0 && frame != 30 && frame != 89 && frame != 320 && frame != 644) continue;
            render();
            std::vector<uint8_t> rgba;
            if (!dx->ReadExportFrameRGBA(rgba, true) || rgba.size() != width * height * 4) throw std::runtime_error("Frame readback failed");
            const auto path = ".local/test-output/mmd-render/frame-" + std::to_string(frame) + ".bmp";
            SaveBmp(rgba, width, height, path.c_str());
            Checkpoint("Captured animated frame");
        }
        if (initialJointBuilds == 0)
            throw std::runtime_error("MMD joints were not built");
        if (bullet->GetImportedJointRebuildCount() != initialJointBuilds)
            throw std::runtime_error("MMD joints rebuilt during steady playback");
        if (std::getenv("MMD_PROFILE"))
            std::cout << "Scene update (animation + physics), frames 60-659: mean "
                << updateMilliseconds / 600.0 << " ms, max "
                << maxUpdateMilliseconds << " ms\n";
        for (unsigned cycle = 0; cycle < 3; ++cycle)
        {
            if (!scene.LoadFromString(snapshot)) throw std::runtime_error("Play snapshot restore failed");
            scene.Start();
            for (unsigned frame = 0; frame < 6; ++frame)
            {
                scene.Update(1.f / 60.f);
                render();
                std::vector<uint8_t> rgba;
                if (!dx->ReadExportFrameRGBA(rgba, true) || rgba.size() != width * height * 4)
                    throw std::runtime_error("Repeated Play frame readback failed");
            }
            Checkpoint("Repeated Play and rendering after snapshot restore");
        }
        std::cout << "DirectX11 MMD playback and Play snapshot restore passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "MMD rendered Play failure: " << error.what() << '\n';
        return 1;
    }
}
