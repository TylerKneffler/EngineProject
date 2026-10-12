#include "Scripts/Physics/FoxRagdollBlend.h"
#include "Scripts/Physics/SoftTissueDemoDriver.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/SoftTissueSurface.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/ComponentReference.h"
#include "Core/Model/ProjectSettings.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include <algorithm>
#include <cstdio>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

int main()
{
#ifdef _WIN32
    constexpr uint32_t width = 640, height = 360;
    unsigned rendered = 0;
    for (const char* api : { "DirectX11", "DirectX12", "Vulkan" })
    {
        std::string reason;
        if (!Engine::Renderers::RendererFactory::IsRendererAvailable(
                api, &reason))
        {
            std::printf("Soft tissue %s unavailable: %s\n", api,
                reason.c_str());
            continue;
        }
        Engine::Core::Window window(GetModuleHandleW(nullptr),
            L"Soft Tissue Render Regression", width, height);
        Engine::Model::ProjectSettings settings{};
        settings.gameRenderingAPI = api;
        auto renderer = Engine::Renderers::RendererFactory::
            CreateGameRenderer(settings);
        if (!renderer || !renderer->Init(window.GetHWND(), width, height))
            return 1;
        Engine::Scene::Scene scene;
        scene.Init(renderer->GetGraphicsProvider());
        if (!scene.Load(
                "Engine/Core/Assets/Scenes/Physics/skinned_soft_tissue_demo.scene"))
            return 2;
        scene.Start();
        auto* fox = scene.FindObjectByName("Fox");
        auto* controller = scene.FindObjectByName("Soft Tissue Demo Controller");
        auto* tissue = controller ? controller->GetComponent<
            Engine::Components::SoftTissueSurface>() : nullptr;
        auto* mesh = tissue ? Engine::Core::ResolveComponentReference<
            Engine::Components::Mesh>(controller, tissue->meshReference) : nullptr;
        if (!mesh || !tissue || tissue->GetOffsets().empty())
        {
            std::fprintf(stderr, "Soft tissue %s binding: fox=%d mesh=%d controller=%d tissue=%d offsets=%zu\n",
                api, fox != nullptr, mesh != nullptr, controller != nullptr,
                tissue != nullptr, tissue ? tissue->GetOffsets().size() : 0u);
            return 3;
        }
        for (int frame = 0; frame < 90; ++frame)
            scene.Update(1.f / 60.f);
        float largestOffset = 0.f;
        for (const glm::vec3& offset : tissue->GetOffsets())
            largestOffset = std::max(largestOffset, glm::length(offset));
        if (!mesh->GetPosedVertexBuffer() || largestOffset < 0.001f)
        {
            std::fprintf(stderr, "Soft tissue %s lacked a deformed render stream (max offset %.5f)\n",
                api, largestOffset);
            return 4;
        }
        scene.PrepareRenderFrame();
        renderer->BeginFrame();
        auto context = renderer->CreateFrameGraphicsContext();
        auto* camera = scene.FindGameCamera();
        if (!context || !camera) return 5;
        scene.Render(context.get(), static_cast<float>(width) / height,
            camera, false, width, height, true);
        renderer->Clear(0.01f, 0.01f, 0.01f);
        scene.Render(context.get(), static_cast<float>(width) / height,
            camera, false, width, height, false);
        renderer->EndFrame();
        renderer->WaitIdle();
        ++rendered;
        std::printf("Soft tissue %s frame completed\n", api);
    }
    return rendered == 0 ? 6 : 0;
#else
    return 0;
#endif
}
