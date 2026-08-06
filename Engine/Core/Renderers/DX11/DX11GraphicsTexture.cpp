#include "pch.h"
#include "DX11GraphicsTexture.h"

std::shared_ptr<IGraphicsTexture> D3D11TextureFactory::CreateTexture2D(
    uint32_t width,
    uint32_t height,
    const uint8_t* rgbaPixels,
    bool srgb)
{
    if (!m_device || !width || !height || !rgbaPixels)
        return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = srgb
        ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = rgbaPixels;
    initial.SysMemPitch = width * 4;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(m_device->CreateTexture2D(&desc, &initial, &texture)) ||
        FAILED(m_device->CreateShaderResourceView(texture.Get(), nullptr, &view)))
        return nullptr;
    return std::make_shared<D3D11GraphicsTexture>(std::move(texture), std::move(view));
}
