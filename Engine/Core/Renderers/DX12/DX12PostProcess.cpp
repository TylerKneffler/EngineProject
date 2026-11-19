#include "pch.h"
#include "DX12PostProcess.h"
#include "Core/Graphics/PostProcess.h"
#include <d3dcompiler.h>
#include <stdexcept>

namespace Engine::Renderers
{
namespace
{
constexpr DXGI_FORMAT HdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
// Exposure results: full float, per-frame adaptation steps are far below
// half precision.
constexpr DXGI_FORMAT ExposureFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr uint32_t ConstantCount = sizeof(Engine::Graphics::PostProcessConstants) / 4u;
constexpr uint32_t TableSize = 3;

// RTV heap: exposure map, histogram, exposure 0, exposure 1 after the bloom
// levels.
constexpr uint32_t ExposureRtv(uint32_t index)
{
    return 1u + Engine::Graphics::MaxBloomLevels + index;
}
constexpr uint32_t RtvCount = ExposureRtv(4);
// SRV heap: three-entry tables after the scene, bloom levels and two nulls.
enum ExposureTableIndex : uint32_t
{
    LuminanceMapTable,  // (map, null, null): histogram input
    AdaptTable0,        // (histogram, exposure 1, null): writes exposure 0
    AdaptTable1,        // (histogram, exposure 0, null): writes exposure 1
    ComposeTable0,      // (scene, bloom 0, exposure 0)
    ComposeTable1,      // (scene, bloom 0, exposure 1)
    ExposureTableCount
};
constexpr uint32_t ExposureTable(uint32_t index)
{
    return 3u + Engine::Graphics::MaxBloomLevels + index * TableSize;
}
constexpr uint32_t SrvCount = ExposureTable(ExposureTableCount);

Microsoft::WRL::ComPtr<ID3DBlob> Compile(const std::string& source,
    const char* entry, const char* target)
{
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    const HRESULT result = D3DCompile(source.data(), source.size(),
        "PostProcess.hlsl", nullptr, nullptr, entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(result))
        throw std::runtime_error(std::string("Post-process shader ") + entry + ": " +
            (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed"));
    return blob;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, UINT subresource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}
}

D3D12_GPU_DESCRIPTOR_HANDLE DX12PostProcess::SrvTable(uint32_t index) const
{
    auto handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * m_srvStride;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12PostProcess::Rtv(uint32_t index) const
{
    auto handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_rtvStride;
    return handle;
}

void DX12PostProcess::Create(ID3D12Device* device, uint32_t width, uint32_t height)
{
    using Engine::Graphics::MaxBloomLevels;
    m_sceneColor.Reset();
    m_bloom.Reset();
    m_width = width; m_height = height;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture{}; texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = width; texture.Height = height; texture.DepthOrArraySize = 1;
    texture.MipLevels = 1; texture.Format = HdrFormat;
    texture.SampleDesc.Count = 1; texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{}; clear.Format = texture.Format; clear.Color[3] = 1.f;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
        IID_PPV_ARGS(&m_sceneColor)));

    m_bloomLevels = Engine::Graphics::BloomLevelCount(width, height);
    D3D12_RESOURCE_DESC bloom = texture;
    bloom.Width = Engine::Graphics::BloomLevelExtent(width, 0);
    bloom.Height = Engine::Graphics::BloomLevelExtent(height, 0);
    bloom.MipLevels = static_cast<UINT16>(m_bloomLevels);
    D3D12_CLEAR_VALUE bloomClear{}; bloomClear.Format = HdrFormat;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &bloom, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &bloomClear,
        IID_PPV_ARGS(&m_bloom)));

    if (!m_rtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtv{}; rtv.NumDescriptors = RtvCount;
        rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&m_rtvHeap)));
        D3D12_DESCRIPTOR_HEAP_DESC srv{}; srv.NumDescriptors = SrvCount;
        srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&m_srvHeap)));
        m_rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        m_srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    const auto srvSlot = [&](uint32_t index)
    {
        auto handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * m_srvStride;
        return handle;
    };
    device->CreateRenderTargetView(m_sceneColor.Get(), nullptr, Rtv(0));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = HdrFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_sceneColor.Get(), &srv, srvSlot(0));
    for (uint32_t level = 0; level < MaxBloomLevels + 2u; ++level)
    {
        // Unused trailing entries hold null views so every three-entry
        // table stays valid.
        const bool exists = level < m_bloomLevels;
        srv.Texture2D.MostDetailedMip = exists ? level : 0;
        device->CreateShaderResourceView(exists ? m_bloom.Get() : nullptr, &srv,
            srvSlot(1 + level));
        if (exists && level < MaxBloomLevels)
        {
            D3D12_RENDER_TARGET_VIEW_DESC rtv{}; rtv.Format = HdrFormat;
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; rtv.Texture2D.MipSlice = level;
            device->CreateRenderTargetView(m_bloom.Get(), &rtv, Rtv(1 + level));
        }
    }

    // Auto exposure targets do not depend on the frame size.
    if (!m_exposureMap)
    {
        using namespace Engine::Graphics;
        const auto create = [&](uint32_t width, uint32_t height, DXGI_FORMAT format,
            Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
        {
            D3D12_RESOURCE_DESC desc = texture;
            desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.Format = format;
            D3D12_CLEAR_VALUE value{}; value.Format = format;
            ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &value,
                IID_PPV_ARGS(&resource)));
        };
        create(ExposureMapWidth, ExposureMapHeight, HdrFormat, m_exposureMap);
        create(ExposureBins, 1, HdrFormat, m_exposureHistogram);
        create(1, 1, ExposureFormat, m_exposure[0]);
        create(1, 1, ExposureFormat, m_exposure[1]);
        m_exposureState.Reset();
    }
    {
        ID3D12Resource* rtvTargets[4] = { m_exposureMap.Get(), m_exposureHistogram.Get(),
            m_exposure[0].Get(), m_exposure[1].Get() };
        for (uint32_t index = 0; index < 4; ++index)
            device->CreateRenderTargetView(rtvTargets[index], nullptr, Rtv(ExposureRtv(index)));
        // Table entries: resource and format, nullptr for a null view.
        struct Entry { ID3D12Resource* resource; DXGI_FORMAT format; };
        const Entry none{ nullptr, HdrFormat };
        const Entry scene{ m_sceneColor.Get(), HdrFormat };
        const Entry bloom0{ m_bloomLevels ? m_bloom.Get() : nullptr, HdrFormat };
        const Entry map{ m_exposureMap.Get(), HdrFormat };
        const Entry histogram{ m_exposureHistogram.Get(), HdrFormat };
        const Entry exposure0{ m_exposure[0].Get(), ExposureFormat };
        const Entry exposure1{ m_exposure[1].Get(), ExposureFormat };
        const Entry tables[ExposureTableCount][TableSize] = {
            { map, none, none },
            { histogram, exposure1, none },
            { histogram, exposure0, none },
            { scene, bloom0, exposure0 },
            { scene, bloom0, exposure1 } };
        for (uint32_t table = 0; table < ExposureTableCount; ++table)
            for (uint32_t entry = 0; entry < TableSize; ++entry)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC view = srv;
                view.Format = tables[table][entry].format;
                view.Texture2D.MostDetailedMip = 0;
                device->CreateShaderResourceView(tables[table][entry].resource, &view,
                    srvSlot(ExposureTable(table) + entry));
            }
    }

    if (!m_rootSignature)
    {
        D3D12_DESCRIPTOR_RANGE range{}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = TableSize; range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[0].Constants.Num32BitValues = ConstantCount;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &range;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister = 0; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC root{}; root.NumParameters = 2; root.pParameters = parameters;
        root.NumStaticSamplers = 1; root.pStaticSamplers = &sampler;
        root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        Microsoft::WRL::ComPtr<ID3DBlob> signature, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
        ThrowIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
        const std::string source = Engine::Graphics::LoadPostProcessShaderSource();
        const auto vs = Compile(source, "VSMain", "vs_5_0");
        const auto ps = Compile(source, "PSMain", "ps_5_0");
        const auto down = Compile(source, "BloomDownsamplePS", "ps_5_0");
        const auto up = Compile(source, "BloomUpsamplePS", "ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = m_rootSignature.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() }; pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX; pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthEnable = FALSE; pso.DepthStencilState.StencilEnable = FALSE;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1; pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pso.SampleDesc.Count = 1;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pipeline)));
        pso.RTVFormats[0] = HdrFormat;
        pso.PS = { down->GetBufferPointer(), down->GetBufferSize() };
        ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_bloomDownsamplePipeline)));
        auto& blend = pso.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = blend.DestBlend = D3D12_BLEND_ONE; blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = blend.DestBlendAlpha = D3D12_BLEND_ONE; blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pso.PS = { up->GetBufferPointer(), up->GetBufferSize() };
        ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_bloomUpsamplePipeline)));
        blend.BlendEnable = FALSE;
        const auto luminance = Compile(source, "ExposureLuminancePS", "ps_5_0");
        const auto histogram = Compile(source, "ExposureHistogramPS", "ps_5_0");
        const auto adapt = Compile(source, "ExposureAdaptPS", "ps_5_0");
        pso.PS = { luminance->GetBufferPointer(), luminance->GetBufferSize() };
        ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_exposureLuminancePipeline)));
        pso.PS = { histogram->GetBufferPointer(), histogram->GetBufferSize() };
        ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_exposureHistogramPipeline)));
        pso.RTVFormats[0] = ExposureFormat;
        pso.PS = { adapt->GetBufferPointer(), adapt->GetBufferSize() };
        ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_exposureAdaptPipeline)));
    }
}

void DX12PostProcess::BindScene(ID3D12GraphicsCommandList* list,
    D3D12_CPU_DESCRIPTOR_HANDLE depth, const float clearColor[4])
{
    const auto barrier = Transition(m_sceneColor.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &barrier);
    const auto target = Rtv(0);
    list->OMSetRenderTargets(1, &target, FALSE, depth.ptr ? &depth : nullptr);
    list->ClearRenderTargetView(target, clearColor, 0, nullptr);
}

void DX12PostProcess::RebindScene(ID3D12GraphicsCommandList* list,
    D3D12_CPU_DESCRIPTOR_HANDLE depth)
{
    const auto target = Rtv(0);
    list->OMSetRenderTargets(1, &target, FALSE, depth.ptr ? &depth : nullptr);
}

void DX12PostProcess::ClearScene(ID3D12GraphicsCommandList* list,
    const float clearColor[4])
{
    list->ClearRenderTargetView(Rtv(0), clearColor, 0, nullptr);
}

void DX12PostProcess::RenderBloom(ID3D12GraphicsCommandList* list)
{
    const uint32_t bloomWidth = Engine::Graphics::BloomLevelExtent(m_width, 0);
    const uint32_t bloomHeight = Engine::Graphics::BloomLevelExtent(m_height, 0);
    const auto drawLevel = [&](uint32_t target, uint32_t sourceTable,
        uint32_t sourceWidth, uint32_t sourceHeight)
    {
        auto barrier = Transition(m_bloom.Get(), target,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &barrier);
        const auto rtv = Rtv(1 + target);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const uint32_t width = std::max(1u, bloomWidth >> target);
        const uint32_t height = std::max(1u, bloomHeight >> target);
        const D3D12_VIEWPORT viewport{ 0.f, 0.f, static_cast<float>(width),
            static_cast<float>(height), 0.f, 1.f };
        const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        const auto values = Engine::Graphics::MakeBloomPassConstants(sourceWidth, sourceHeight);
        list->SetGraphicsRoot32BitConstants(0, ConstantCount, &values, 0);
        list->SetGraphicsRootDescriptorTable(1, SrvTable(sourceTable));
        list->DrawInstanced(3, 1, 0, 0);
        barrier = Transition(m_bloom.Get(), target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &barrier);
    };
    list->SetPipelineState(m_bloomDownsamplePipeline.Get());
    drawLevel(0, 0, m_width, m_height);
    for (uint32_t level = 1; level < m_bloomLevels; ++level)
        drawLevel(level, level, std::max(1u, bloomWidth >> (level - 1)),
            std::max(1u, bloomHeight >> (level - 1)));
    list->SetPipelineState(m_bloomUpsamplePipeline.Get());
    for (uint32_t level = m_bloomLevels; level-- > 1;)
        drawLevel(level - 1, 1 + level, std::max(1u, bloomWidth >> level),
            std::max(1u, bloomHeight >> level));

    const D3D12_VIEWPORT viewport{ 0.f, 0.f, static_cast<float>(m_width),
        static_cast<float>(m_height), 0.f, 1.f };
    const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
}

uint32_t DX12PostProcess::RenderExposure(ID3D12GraphicsCommandList* list,
    const Engine::Graphics::PostProcessSettings& settings)
{
    using namespace Engine::Graphics;
    const ExposureFrame frame = m_exposureState.Next();
    const auto values = MakeExposureConstants(settings, frame.deltaSeconds, frame.history);
    const auto pass = [&](ID3D12PipelineState* pipeline, ID3D12Resource* target,
        uint32_t rtv, uint32_t width, uint32_t height, uint32_t table)
    {
        auto barrier = Transition(target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &barrier);
        const auto handle = Rtv(rtv);
        list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{ 0.f, 0.f, float(width), float(height), 0.f, 1.f };
        const D3D12_RECT scissor{ 0, 0, LONG(width), LONG(height) };
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetPipelineState(pipeline);
        list->SetGraphicsRoot32BitConstants(0, ConstantCount, &values, 0);
        list->SetGraphicsRootDescriptorTable(1, SrvTable(table));
        list->DrawInstanced(3, 1, 0, 0);
        barrier = Transition(target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &barrier);
    };
    // Table 0 starts at the scene.
    pass(m_exposureLuminancePipeline.Get(), m_exposureMap.Get(), ExposureRtv(0),
        ExposureMapWidth, ExposureMapHeight, 0);
    pass(m_exposureHistogramPipeline.Get(), m_exposureHistogram.Get(), ExposureRtv(1),
        ExposureBins, 1, ExposureTable(LuminanceMapTable));
    pass(m_exposureAdaptPipeline.Get(), m_exposure[frame.target].Get(),
        ExposureRtv(2 + frame.target), 1, 1,
        ExposureTable(frame.target ? AdaptTable1 : AdaptTable0));

    const D3D12_VIEWPORT viewport{ 0.f, 0.f, static_cast<float>(m_width),
        static_cast<float>(m_height), 0.f, 1.f };
    const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    return ExposureTable(frame.target ? ComposeTable1 : ComposeTable0);
}

void DX12PostProcess::Compose(ID3D12GraphicsCommandList* list,
    D3D12_CPU_DESCRIPTOR_HANDLE output)
{
    const auto barrier = Transition(m_sceneColor.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &barrier);
    auto settings = Engine::Graphics::GetPostProcessSettings();
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() }; list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(m_rootSignature.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (settings.bloomIntensity > 0.f && m_bloom)
        RenderBloom(list);
    else
        settings.bloomIntensity = 0.f;
    uint32_t composeTable = 0;
    if (settings.autoExposure && m_exposureMap)
        composeTable = RenderExposure(list, settings);
    else
    {
        settings.autoExposure = false;
        m_exposureState.Reset();
    }
    list->OMSetRenderTargets(1, &output, FALSE, nullptr);
    const auto values = Engine::Graphics::MakeComposeConstants(settings, m_bloomLevels);
    list->SetPipelineState(m_pipeline.Get());
    list->SetGraphicsRoot32BitConstants(0, ConstantCount, &values, 0);
    list->SetGraphicsRootDescriptorTable(1, SrvTable(composeTable));
    list->DrawInstanced(3, 1, 0, 0);
}
}
