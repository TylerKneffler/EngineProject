#include "pch.h"
#include "DX11GraphicsTexture.h"

namespace Engine::Renderers
{
namespace
{
DXGI_FORMAT D3D11TextureFormat(Engine::Graphics::GraphicsTextureFormat format, bool srgb)
{
    using Engine::Graphics::GraphicsTextureFormat;
    switch (format)
    {
    case GraphicsTextureFormat::Rgba32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case GraphicsTextureFormat::Bc1: return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    case GraphicsTextureFormat::Bc4: return DXGI_FORMAT_BC4_UNORM;
    case GraphicsTextureFormat::Bc5: return DXGI_FORMAT_BC5_UNORM;
    case GraphicsTextureFormat::Bc7: return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    default: return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}
}

std::shared_ptr<Engine::Graphics::IGraphicsTexture> D3D11TextureFactory::CreateTexture2D(
    uint32_t width,
    uint32_t height,
    const uint8_t* rgbaPixels,
    uint32_t mipLevels,
    Engine::Graphics::GraphicsTextureFormat format,
    bool srgb)
{
    return Create(width, height, 1, false, rgbaPixels, mipLevels, format, srgb);
}

std::shared_ptr<Engine::Graphics::IGraphicsTexture> D3D11TextureFactory::CreateTexture2DArray(
    uint32_t width, uint32_t height, uint32_t layers, const uint8_t* pixels,
    uint32_t mipLevels, Engine::Graphics::GraphicsTextureFormat format, bool srgb)
{
    return Create(width, height, layers, true, pixels, mipLevels, format, srgb);
}

std::shared_ptr<Engine::Graphics::IGraphicsTexture> D3D11TextureFactory::Create(
    uint32_t width, uint32_t height, uint32_t layers, bool asArray,
    const uint8_t* rgbaPixels, uint32_t mipLevels,
    Engine::Graphics::GraphicsTextureFormat format, bool srgb)
{
    if (!m_device || !width || !height || !layers || !rgbaPixels || !mipLevels ||
        !Engine::Graphics::GraphicsTextureIsValidBase(format, width, height))
        return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = mipLevels;
    desc.ArraySize = layers;
    desc.Format = D3D11TextureFormat(format, srgb);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // Subresource index is mip + layer * mipLevels, the source order.
    std::vector<D3D11_SUBRESOURCE_DATA> initial(size_t(mipLevels) * layers);
    size_t offset = 0;
    for (uint32_t layer = 0; layer < layers; ++layer)
    {
        uint32_t mipWidth = width, mipHeight = height;
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            auto& level = initial[size_t(layer) * mipLevels + mip];
            level.pSysMem = rgbaPixels + offset;
            level.SysMemPitch = GraphicsTextureRowBytes(format, mipWidth);
            offset += static_cast<size_t>(GraphicsTextureMipBytes(format, mipWidth, mipHeight));
            mipWidth = std::max(1u, mipWidth / 2);
            mipHeight = std::max(1u, mipHeight / 2);
        }
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.MipLevels = mipLevels;
    srv.Texture2DArray.ArraySize = layers;
    if (FAILED(m_device->CreateTexture2D(&desc, initial.data(), &texture)) ||
        FAILED(m_device->CreateShaderResourceView(texture.Get(),
            asArray ? &srv : nullptr, &view)))
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
