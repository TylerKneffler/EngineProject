#include "Core/Renderers/DX11/DX11GraphicsProvider.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <vector>

using Engine::Graphics::IGraphicsBuffer;
using Engine::Graphics::IShaderCompiler;

namespace
{
struct alignas(16) GpuParticle
{
    float positionAge[4]{};
    float velocityLife[4]{};
    float colorSize[4]{};
    uint32_t meta[4]{};
};
static_assert(sizeof(GpuParticle) == 64);
constexpr uint32_t Capacity = 4;

struct alignas(16) Constants
{
    uint32_t u0[4]{};
    float c[7][4]{};
};
static_assert(sizeof(Constants) == 128);

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
    auto create = [&](D3D_DRIVER_TYPE driver)
    {
        return D3D11CreateDevice(nullptr, driver, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &immediate);
    };
    if (FAILED(create(D3D_DRIVER_TYPE_HARDWARE)))
    {
        device.Reset(); immediate.Reset();
        Require(SUCCEEDED(create(D3D_DRIVER_TYPE_WARP)),
            "no DX11 hardware or WARP device for compute test");
    }
    Engine::Renderers::D3D11GraphicsProvider provider(device.Get(),
        immediate.Get());
    auto* compiler = provider.GetShaderCompiler();
    auto* pipelines = provider.GetPipelineStateFactory();
    auto* buffers = provider.GetBufferFactory();
    auto* contexts = provider.GetContextFactory();
    const auto shaderPath = (std::filesystem::path(__FILE__).parent_path()
        .parent_path().parent_path() / "Engine/Core/Shaders/ParticleGpu/ParticleGpu.hlsl")
        .string();
    const auto pipeline = [&](const char* entry)
    {
        auto shader = compiler->CompileFromFile(shaderPath, entry,
            IShaderCompiler::CompileProfile::CS_5_0);
        Require(shader != nullptr, "particle compute shader did not compile");
        auto result = pipelines->CreateComputePipeline(shader.get());
        Require(result != nullptr, "particle compute pipeline creation failed");
        return result;
    };
    auto clear = pipeline("CSClear");
    auto update = pipeline("CSUpdate");
    auto spawn = pipeline("CSSpawn");
    constexpr uint64_t Bytes = (Capacity + 1) * sizeof(GpuParticle);
    auto a = buffers->CreateBuffer(IGraphicsBuffer::Usage::StorageBuffer,
        IGraphicsBuffer::AccessMode::Default, Bytes, nullptr,
        sizeof(GpuParticle));
    auto b = buffers->CreateBuffer(IGraphicsBuffer::Usage::StorageBuffer,
        IGraphicsBuffer::AccessMode::Default, Bytes, nullptr,
        sizeof(GpuParticle));
    auto readback = buffers->CreateBuffer(IGraphicsBuffer::Usage::StorageBuffer,
        IGraphicsBuffer::AccessMode::Readback, Bytes, nullptr,
        sizeof(GpuParticle));
    contexts->PrepareFrame(0);
    auto context = contexts->CreateContext();
    Require(context != nullptr, "no DX11 compute context");
    const auto clearBuffer = [&](IGraphicsBuffer* target)
    {
        Require(context->SetComputePipeline(clear.get()),
            "compute pipeline bind failed");
        context->SetComputeWriteBuffer(target);
        context->DispatchCompute(1);
        context->StorageBarrier(target);
    };
    const auto snapshot = [&](IGraphicsBuffer* source)
    {
        Require(context->CopyBuffer(source, readback.get()),
            "compute readback copy failed");
        const auto* mapped = static_cast<const GpuParticle*>(readback->Map());
        Require(mapped != nullptr, "compute readback map failed");
        std::array<GpuParticle, Capacity + 1> values{};
        std::copy(mapped, mapped + values.size(), values.begin());
        readback->Unmap();
        return values;
    };
    Constants spawnData{};
    spawnData.u0[0] = 2;
    spawnData.u0[1] = Capacity;
    spawnData.u0[2] = 1;
    spawnData.u0[3] = 17;
    spawnData.c[0][0] = 1.f; // world X basis
    spawnData.c[1][1] = 1.f; // world Y basis
    spawnData.c[1][3] = 1.f; // lifetime
    spawnData.c[2][2] = 1.f; // world Z basis
    spawnData.c[4][1] = 1.f; // launch direction
    spawnData.c[5][3] = 0.1f; // size
    spawnData.c[6][1] = 1.f; // green
    clearBuffer(a.get());
    Require(context->SetComputePipeline(spawn.get()),
        "spawn pipeline bind failed");
    context->SetComputeConstants(&spawnData, sizeof(spawnData));
    context->SetComputeWriteBuffer(a.get());
    context->DispatchCompute(1);
    context->StorageBarrier(a.get());
    const auto spawned = snapshot(a.get());
    Require(spawned[0].meta[0] == 2 &&
        ((spawned[1].meta[0] == 1 && spawned[2].meta[0] == 2) ||
         (spawned[1].meta[0] == 2 && spawned[2].meta[0] == 1)),
        "GPU spawn count and stable IDs");
    Constants updateData{};
    updateData.u0[0] = Capacity;
    updateData.c[0][0] = 0.5f;
    updateData.c[1][1] = -1.f;
    clearBuffer(b.get());
    Require(context->SetComputePipeline(update.get()),
        "update pipeline bind failed");
    context->SetComputeConstants(&updateData, sizeof(updateData));
    context->SetComputeReadBuffer(a.get());
    context->SetComputeWriteBuffer(b.get());
    context->DispatchCompute(1);
    context->StorageBarrier(b.get());
    const auto integrated = snapshot(b.get());
    Require(integrated[0].meta[0] == 2 &&
        std::abs(integrated[1].positionAge[3] - 0.5f) < 1e-5f &&
        std::abs(integrated[2].positionAge[3] - 0.5f) < 1e-5f &&
        integrated[1].positionAge[1] < 0.f,
        "GPU integration matches fixed-step age and gravity");
    updateData.c[0][0] = 0.6f;
    clearBuffer(a.get());
    context->SetComputePipeline(update.get());
    context->SetComputeConstants(&updateData, sizeof(updateData));
    context->SetComputeReadBuffer(b.get());
    context->SetComputeWriteBuffer(a.get());
    context->DispatchCompute(1);
    context->StorageBarrier(a.get());
    Require(snapshot(a.get())[0].meta[0] == 0,
        "GPU compaction removes expired particles and frees capacity");
}
