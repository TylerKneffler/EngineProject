#include "Core/Compoonents/Particles/GpuParticleNeighborGrid.h"
#include "Core/Compoonents/Particles/ParticleNeighborGrid.h"
#include "Core/Renderers/DX11/DX11GraphicsProvider.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
using Engine::Components::GpuParticleNeighborGrid;
using Engine::Components::ParticleNeighborGrid;
using Engine::Components::ParticleState;
using Engine::Graphics::IGraphicsBuffer;

struct alignas(16) GpuParticle
{
    float positionAge[4]{};
    float velocityLife[4]{};
    float colorSize[4]{};
    uint32_t meta[4]{};
};
static_assert(sizeof(GpuParticle) == 64);

void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
        const auto create = [&](D3D_DRIVER_TYPE driver)
        {
            return D3D11CreateDevice(nullptr, driver, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &device, nullptr, &immediate);
        };
        if (FAILED(create(D3D_DRIVER_TYPE_HARDWARE)))
        {
            device.Reset(); immediate.Reset();
            Require(SUCCEEDED(create(D3D_DRIVER_TYPE_WARP)),
                "DX11 hardware and WARP compute unavailable");
        }
        Engine::Renderers::D3D11GraphicsProvider provider(device.Get(),
            immediate.Get());
        auto* factory = provider.GetBufferFactory();
        auto* contexts = provider.GetContextFactory();
        contexts->PrepareFrame(0);
        auto context = contexts->CreateContext();
        Require(context != nullptr, "compute context unavailable");

        const float nan = std::numeric_limits<float>::quiet_NaN();
        std::vector<glm::vec3> positions{
            {0.f, 0.f, 0.f}, {.5f, 0.f, 0.f}, {.499f, 0.f, 0.f},
            {-.5f, 0.f, 0.f}, {-.501f, 0.f, 0.f},
            {.5f, .5f, .5f}, {-.5f, -.5f, -.5f},
            {1.f, 1.f, 1.f}, {-1.f, -1.f, -1.f},
            {0.f, .5f, 0.f}, {0.f, -.5f, 0.f},
            {0.f, 0.f, .5f}, {0.f, 0.f, -.5f},
            {1.01f, 0.f, 0.f}, {-1.01f, 0.f, 0.f},
            {0.f, 0.f, 0.f}, {nan, 0.f, 0.f},
            {0.f, 0.f, 0.f}
        };
        positions.insert(positions.end(), 20, glm::vec3(0.f));
        std::vector<ParticleState> cpu(positions.size());
        std::vector<GpuParticle> gpu(positions.size() + 1u);
        gpu[0].meta[0] = static_cast<uint32_t>(positions.size());
        for (size_t i = 0; i < positions.size(); ++i)
        {
            cpu[i].position = positions[i];
            cpu[i].active = i != 17;
            gpu[i + 1].positionAge[0] = positions[i].x;
            gpu[i + 1].positionAge[1] = positions[i].y;
            gpu[i + 1].positionAge[2] = positions[i].z;
            gpu[i + 1].meta[1] = cpu[i].active ? 1u : 0u;
        }
        auto particles = factory->CreateBuffer(
            IGraphicsBuffer::Usage::StorageBuffer,
            IGraphicsBuffer::AccessMode::Default,
            gpu.size() * sizeof(GpuParticle), gpu.data(), sizeof(GpuParticle));
        Require(particles != nullptr, "GPU source buffer unavailable");

        ParticleNeighborGrid reference;
        Require(reference.Build(cpu.data(), cpu.size(), .5f),
            "CPU reference grid failed to build");
        std::vector<GpuParticleNeighborGrid::Query> queries{
            {{0.f, 0.f, 0.f}, 0.f},
            {{0.f, 0.f, 0.f}, .501f},
            {{.5f, 0.f, 0.f}, .1f, 1u},
            {{-.5f, 0.f, 0.f}, .1f},
            {{.5f, .5f, .5f}, .51f},
            {{-.5f, -.5f, -.5f}, .51f},
            {{0.f, 0.f, 0.f}, 1.25f}, // direct-scan path
            {{nan, 0.f, 0.f}, .5f},
            {{0.f, 0.f, 0.f}, -1.f}
        };
        for (int z = -1; z <= 1; ++z)
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x)
                    for (float radius : {0.f, .25f, .5f, .75f, 1.25f})
                        queries.push_back({{x * .5f, y * .5f, z * .5f},
                            radius});
        GpuParticleNeighborGrid grid(&provider);
        const auto compare = [&](const IGraphicsBuffer* source)
        {
            Require(grid.Record(context.get(), source,
                static_cast<uint32_t>(cpu.size()), .5f, queries),
                "GPU grid build or query dispatch failed");
            Require(grid.RequestReadback(context.get()),
                "GPU neighbor readback request failed");
            std::vector<std::vector<uint32_t>> actual;
            Require(grid.ResolveReadback(actual), "GPU neighbor readback failed");
            Require(actual.size() == queries.size(), "query count mismatch");
            std::vector<uint32_t> expected;
            for (size_t i = 0; i < queries.size(); ++i)
            {
                reference.QueryRadius(queries[i].position, queries[i].radius,
                    expected, queries[i].excludeIndex);
                if (actual[i] != expected)
                    throw std::runtime_error("neighbor mismatch at query " +
                        std::to_string(i));
            }
        };
        compare(particles.get());

        // A second snapshot must drop a moved particle from its old boundary cell.
        cpu[0].position = {3.f, 3.f, 3.f};
        gpu[1].positionAge[0] = 3.f;
        gpu[1].positionAge[1] = 3.f;
        gpu[1].positionAge[2] = 3.f;
        Require(reference.Build(cpu.data(), cpu.size(), .5f),
            "CPU reference rebuild failed");
        auto movedParticles = factory->CreateBuffer(
            IGraphicsBuffer::Usage::StorageBuffer,
            IGraphicsBuffer::AccessMode::Default,
            gpu.size() * sizeof(GpuParticle), gpu.data(), sizeof(GpuParticle));
        Require(movedParticles != nullptr, "moved GPU source buffer unavailable");
        compare(movedParticles.get());

        std::cout << "GPU neighbor sets match CPU across boundary and rebuild queries\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
