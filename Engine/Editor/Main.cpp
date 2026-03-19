#include "Core/Window.h"
#include "Core/Renderers/Editor/DX12EditorRenderer.h"
#include "Core/Object/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Scene/SceneViewport.h"
#include "imgui_internal.h"  // DockBuilder API

// Fallback for IntelliSense — CMake overrides this with the real absolute path.
#ifndef ASSETS_PATH
#define ASSETS_PATH "Assets/"
#endif

// ImGui Win32 WndProc handler — intentionally not declared in imgui_impl_win32.h
// to avoid dragging <windows.h> into that header. Forward-declared here per imgui docs.
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// Embedded HLSL — simple diffuse Phong shader for the cube
// ---------------------------------------------------------------------------

static constexpr char kVS[] = R"(
cbuffer Constants : register(b0) {
    float4x4 mvp;
    float4x4 world;
};

void VSMain(float3 pos    : POSITION,
            float3 normal : NORMAL,
            out float4 oPos    : SV_POSITION,
            out float3 oNormal : NORMAL)
{
    oPos    = mul(mvp, float4(pos, 1.0));
    oNormal = mul((float3x3)world, normal);
}
)";

static constexpr char kPS[] = R"(
cbuffer Constants : register(b0) {
    float4x4 mvp;
    float4x4 world;
    float4   diffuseColor;
    float4   ambientColor;
    float4   specularColor; // w = shininess
};

float4 PSMain(float4 pos    : SV_POSITION,
              float3 normal : NORMAL) : SV_TARGET
{
    float3 lightDir = normalize(float3(1.0, 2.0, -1.0));
    float  diffuse  = saturate(dot(normalize(normal), lightDir));
    float3 color    = ambientColor.rgb + diffuseColor.rgb * diffuse;
    return float4(color, 1.0);
}
)";

// ---------------------------------------------------------------------------
// WinMain — Editor entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_     LPWSTR    /*lpCmdLine*/,
    _In_     int       /*nShowCmd*/)
{
    auto window   = std::make_unique<Window>(hInstance, L"Engine Editor", 1280, 720);
    auto renderer = std::make_unique<DX12EditorRenderer>();
    renderer->Init(window->GetHWND(), window->GetWidth(), window->GetHeight());

    // Scene viewport — renders 3-D content into an offscreen texture (SRV slot 1).
    auto [srvCpu, srvGpu] = renderer->GetSrvSlot(1);
    SceneViewport sceneViewport;
    sceneViewport.Init(renderer->GetDevice(),
                       window->GetWidth(), window->GetHeight(),
                       srvCpu, srvGpu);

    // -----------------------------------------------------------------------
    // Cube pipeline — temporary home in Main; will be extracted later
    // -----------------------------------------------------------------------
    ID3D12Device* device = renderer->GetDevice();

    // Compile shaders at runtime from embedded HLSL source.
    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;

    if (FAILED(D3DCompile(kVS, sizeof(kVS) - 1, "VS",
            nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, &errBlob)))
        throw std::runtime_error(static_cast<char*>(errBlob->GetBufferPointer()));

    if (FAILED(D3DCompile(kPS, sizeof(kPS) - 1, "PS",
            nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, &errBlob)))
        throw std::runtime_error(static_cast<char*>(errBlob->GetBufferPointer()));

    // Root signature: one root CBV at b0, visible to VS only.
    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParam.Descriptor.ShaderRegister = 0;
    rootParam.Descriptor.RegisterSpace  = 0;
    rootParam.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters   = &rootParam;
    rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, sigErr;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigErr));

    ComPtr<ID3D12RootSignature> rootSig;
    ThrowIfFailed(device->CreateRootSignature(
        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
        IID_PPV_ARGS(&rootSig)));

    // Input layout: POSITION (float3) at offset 0, NORMAL (float3) at offset 12.
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // Rasterizer — solid fill, back-face culling. OBJ files use CCW winding
    // (standard in most DCC tools), so tell DX12 to treat CCW as front-facing.
    D3D12_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode              = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode              = D3D12_CULL_MODE_BACK;
    rasterDesc.FrontCounterClockwise = TRUE;
    rasterDesc.DepthClipEnable       = TRUE;

    D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
    rtBlend.BlendEnable           = FALSE;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0] = rtBlend;

    // Depth — standard less-than test with full writes.
    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable    = TRUE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = rootSig.Get();
    psoDesc.VS                    = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS                    = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.InputLayout           = { layout, _countof(layout) };
    psoDesc.RasterizerState       = rasterDesc;
    psoDesc.BlendState            = blendDesc;
    psoDesc.DepthStencilState     = dsDesc;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM; // matches SceneViewport
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc            = { 1, 0 };
    psoDesc.SampleMask            = UINT_MAX;

    ComPtr<ID3D12PipelineState> pso;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)));

    // Create the scene and add the cube as a managed game object.
    Scene scene;
    scene.Init(device);

    Object*            cubeObj = scene.AddObject("Cube");
    Mesh*     mesh    = cubeObj->AddComponent<Mesh>();
    Material* mat     = cubeObj->AddComponent<Material>();
    mesh->LoadFromFile(ASSETS_PATH "cube.obj");
    mesh->CreateBuffer(device);

    // Constant buffer — 256-byte aligned, persistently mapped for per-frame uploads.
    // Layout: float4x4 mvp (64B) + float4x4 world (64B) + 3x float4 material (48B) = 176B -> 256B.
    struct CBData
    {
        DirectX::XMFLOAT4X4 mvp;
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4   diffuseColor;   // xyz = color
        DirectX::XMFLOAT4   ambientColor;   // xyz = color
        DirectX::XMFLOAT4   specularColor;  // xyz = color, w = shininess
    };
    ComPtr<ID3D12Resource> constantBuffer;
    void*                  cbMapped = nullptr;
    {
        constexpr UINT kCBSize = (sizeof(CBData) + 255) & ~255; // = 256 bytes

        D3D12_HEAP_PROPERTIES uploadProps{};
        uploadProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC cbDesc{};
        cbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbDesc.Width            = kCBSize;
        cbDesc.Height           = 1;
        cbDesc.DepthOrArraySize = 1;
        cbDesc.MipLevels        = 1;
        cbDesc.Format           = DXGI_FORMAT_UNKNOWN;
        cbDesc.SampleDesc       = { 1, 0 };
        cbDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(device->CreateCommittedResource(
            &uploadProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&constantBuffer)));

        D3D12_RANGE noRead{ 0, 0 };
        ThrowIfFailed(constantBuffer->Map(0, &noRead, &cbMapped));
    }

    // High-resolution timer for frame delta time.
    LARGE_INTEGER perfFreq, lastCounter;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&lastCounter);

    float yaw   = 0.0f; // cumulative Y-axis rotation (radians)
    float pitch = 0.0f; // cumulative X-axis rotation (radians)

    // -----------------------------------------------------------------------
    // Window callbacks
    // -----------------------------------------------------------------------

    window->OnResize = [&](uint32_t w, uint32_t h)
    {
        renderer->Resize(w, h);           // flushes GPU before recreating swap chain
        sceneViewport.Resize(renderer->GetDevice(), w, h);
        renderer->MarkDirty();
    };

    window->WndProcHook = [](HWND h, UINT m, WPARAM w, LPARAM l) -> bool {
        return ImGui_ImplWin32_WndProcHandler(h, m, w, l) != 0;
    };

    window->OnUpdate = [&]()
    {
        // Compute delta time.
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - lastCounter.QuadPart)
                 / static_cast<float>(perfFreq.QuadPart);
        lastCounter = now;

        renderer->MarkDirty();
        renderer->RenderIfNeeded([&]()
        {
            renderer->Clear(0.18f, 0.18f, 0.18f);

            // Render 3-D scene into the offscreen texture before any ImGui calls
            // so the texture is in SRV state when ImGui samples it.
            sceneViewport.Render(renderer->GetCommandList(), renderer->GetCurrentRTV(),
                [&](ID3D12GraphicsCommandList* cmd)
                {
                    using namespace DirectX;

                    float aspect = sceneViewport.GetAspect();

                    XMMATRIX worldMat = XMMatrixRotationX(pitch) * XMMatrixRotationY(yaw);
                    XMMATRIX view  = XMMatrixLookAtLH(
                        XMVectorSet(0.f,  1.5f, -3.f, 1.f),
                        XMVectorSet(0.f,  0.f,   0.f, 1.f),
                        XMVectorSet(0.f,  1.f,   0.f, 0.f));
                    XMMATRIX proj  = XMMatrixPerspectiveFovLH(
                        XMConvertToRadians(60.f), aspect, 0.01f, 100.f);

                    // XMStoreFloat4x4 writes row-major, which HLSL cbuffer reads as
                    // row-major — no extra transpose needed. mul(M, v) in the shader
                    // then correctly computes WVP * pos as a column-vector multiply.
                    CBData cbData{};
                    XMStoreFloat4x4(&cbData.mvp,   worldMat * view * proj);
                    XMStoreFloat4x4(&cbData.world, worldMat);
                    cbData.diffuseColor  = { mat->diffuseColor.r,  mat->diffuseColor.g,  mat->diffuseColor.b,  1.f };
                    cbData.ambientColor  = { mat->ambientColor.r,  mat->ambientColor.g,  mat->ambientColor.b,  1.f };
                    cbData.specularColor = { mat->specularColor.r, mat->specularColor.g, mat->specularColor.b, mat->shininess };
                    memcpy(cbMapped, &cbData, sizeof(cbData));

                    cmd->SetGraphicsRootSignature(rootSig.Get());
                    cmd->SetPipelineState(pso.Get());
                    cmd->SetGraphicsRootConstantBufferView(
                        0, constantBuffer->GetGPUVirtualAddress());
                    D3D12_VERTEX_BUFFER_VIEW vbv = mesh->GetVertexBufferView();
                    cmd->IASetVertexBuffers(0, 1, &vbv);
                    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    cmd->DrawInstanced(mesh->GetVertexCount(), 1, 0, 0);

                    // Draw scene helpers (grid etc.) after opaque objects.
                    scene.Render(cmd, view, proj);
                });

            ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

            // Build the default layout once — only when no saved layout exists.
            if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr ||
                ImGui::DockBuilderGetNode(dockspaceId)->IsLeafNode())
            {
                ImGui::DockBuilderRemoveNode(dockspaceId);
                ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

                ImGuiID left, center, right;
                ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left,  0.20f, &left,   &center);
                ImGui::DockBuilderSplitNode(center,      ImGuiDir_Right, 0.31f, &right,  &center);

                ImGui::DockBuilderDockWindow("Hierarchy",  left);
                ImGui::DockBuilderDockWindow("Scene",      center);
                ImGui::DockBuilderDockWindow("Properties", right);
                ImGui::DockBuilderFinish(dockspaceId);
            }

            if (ImGui::BeginMainMenuBar())
            {
                if (ImGui::BeginMenu("File"))
                {
                    if (ImGui::MenuItem("Exit")) PostQuitMessage(0);
                    ImGui::EndMenu();
                }
                ImGui::EndMainMenuBar();
            }

            ImGui::Begin("Hierarchy");
            ImGui::End();

            ImGui::Begin("Properties");
            ImGui::End();

            sceneViewport.DrawPanel();

            // Rotate cube by dragging left mouse button inside the Scene panel.
            constexpr float kSensitivity = 0.005f;
            yaw   += sceneViewport.GetDragDeltaX() * kSensitivity;
            pitch += sceneViewport.GetDragDeltaY() * kSensitivity;
            // Clamp pitch so the cube doesn't flip past vertical (~89 degrees).
            constexpr float kPitchLimit = 1.5607963f; // XM_PIDIV2 - 0.01
            if (pitch >  kPitchLimit) pitch =  kPitchLimit;
            if (pitch < -kPitchLimit) pitch = -kPitchLimit;
        });
    };

    return window->Run();
}
