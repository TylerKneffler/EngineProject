#include "DX12GraphicsProvider.h"

Microsoft::WRL::ComPtr<ID3D12RootSignature>
CreateD3D12MaterialRootSignature(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_RANGE ranges[6]{};
    D3D12_ROOT_PARAMETER parameters[10]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT index = 0; index < 6; ++index)
    {
        ranges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[index].NumDescriptors = 1;
        ranges[index].BaseShaderRegister = index;
        ranges[index].OffsetInDescriptorsFromTableStart = 0;
        parameters[index + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[index + 1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[index + 1].DescriptorTable.pDescriptorRanges = &ranges[index];
        parameters[index + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    for (UINT index = 0; index < 3; ++index)
    {
        parameters[index + 7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[index + 7].Descriptor.ShaderRegister = index + 6;
        parameters[index + 7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = ARRAYSIZE(parameters);
    desc.pParameters = parameters;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(device->CreateRootSignature(
        0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&rootSignature)));
    return rootSignature;
}

D3D12GraphicsProvider::D3D12GraphicsProvider(
    ID3D12Device* device,
    ID3D12CommandQueue* commandQueue,
    ID3D12RootSignature* rootSig)
{
    OutputDebugStringA("[D3D12GraphicsProvider] Creating shader compiler...\n");
    m_shaderCompiler = std::make_unique<D3D12ShaderCompiler>();
    OutputDebugStringA("[D3D12GraphicsProvider] Creating buffer factory...\n");
    m_bufferFactory = std::make_unique<D3D12BufferFactory>(device);
    OutputDebugStringA("[D3D12GraphicsProvider] Creating pipeline factory...\n");
    m_pipelineFactory = std::make_unique<D3D12PipelineStateFactory>(device, rootSig);
    OutputDebugStringA("[D3D12GraphicsProvider] Creating context factory...\n");
    m_contextFactory = std::make_unique<D3D12GraphicsContextFactory>(device, commandQueue, rootSig);
    m_textureFactory = std::make_unique<D3D12TextureFactory>(device, commandQueue);
    OutputDebugStringA("[D3D12GraphicsProvider] All factories created\n");
}
