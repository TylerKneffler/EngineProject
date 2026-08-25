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
}
