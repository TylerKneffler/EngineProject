#include "pch.h"
#include "DX11GraphicsTexture.h"

namespace Engine::Renderers
{
std::shared_ptr<Engine::Graphics::IGraphicsTexture> D3D11TextureFactory::CreateTexture2D(
    uint32_t width,
    uint32_t height,
    const uint8_t* rgbaPixels,
    uint32_t mipLevels,
    Engine::Graphics::GraphicsTextureFormat format,
    bool srgb)
{
    if (!m_device || !width || !height || !rgbaPixels || !mipLevels)
        return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = mipLevels;
    desc.ArraySize = 1;
    desc.Format = format == Engine::Graphics::GraphicsTextureFormat::Rgba32Float
        ? DXGI_FORMAT_R32G32B32A32_FLOAT
        : (srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::vector<D3D11_SUBRESOURCE_DATA> initial(mipLevels);
    const uint32_t bytesPerPixel = GraphicsTextureBytesPerPixel(format);
    size_t offset = 0;
    uint32_t mipWidth = width, mipHeight = height;
    for (uint32_t mip = 0; mip < mipLevels; ++mip)
    {
        initial[mip].pSysMem = rgbaPixels + offset;
        initial[mip].SysMemPitch = mipWidth * bytesPerPixel;
        offset += static_cast<size_t>(mipWidth) * mipHeight * bytesPerPixel;
        mipWidth = std::max(1u, mipWidth / 2);
        mipHeight = std::max(1u, mipHeight / 2);
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(m_device->CreateTexture2D(&desc, initial.data(), &texture)) ||
        FAILED(m_device->CreateShaderResourceView(texture.Get(), nullptr, &view)))
        return nullptr;
    return std::make_shared<D3D11GraphicsTexture>(std::move(texture), std::move(view));
}

std::shared_ptr<Engine::Graphics::IGraphicsTexture>
D3D11TextureFactory::CreateDepthTexture2D(uint32_t width, uint32_t height)
{
    if (!m_device || !width || !height)
        return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &texture)))
        return nullptr;

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthView;
    if (FAILED(m_device->CreateDepthStencilView(
            texture.Get(), &dsvDesc, &depthView)))
        return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(m_device->CreateShaderResourceView(texture.Get(), &srvDesc, &view)))
        return nullptr;

    return std::make_shared<D3D11GraphicsTexture>(
        std::move(texture), std::move(view), std::move(depthView));
}
}
