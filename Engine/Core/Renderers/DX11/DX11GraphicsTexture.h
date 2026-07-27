#pragma once

#include "Core/Graphics/IGraphicsTexture.h"

#include <d3d11.h>
#include <wrl/client.h>

class D3D11GraphicsTexture final : public IGraphicsTexture
{
public:
    D3D11GraphicsTexture(
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view)
        : m_texture(std::move(texture)), m_view(std::move(view)) {}

    void* GetNativeHandle() const override { return m_view.Get(); }
    ID3D11ShaderResourceView* GetView() const { return m_view.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_view;
};

class D3D11TextureFactory final : public IGraphicsTextureFactory
{
public:
    explicit D3D11TextureFactory(ID3D11Device* device) : m_device(device) {}
    std::shared_ptr<IGraphicsTexture> CreateTexture2D(
        uint32_t width, uint32_t height, const uint8_t* rgbaPixels) override;

private:
    ID3D11Device* m_device = nullptr;
};
