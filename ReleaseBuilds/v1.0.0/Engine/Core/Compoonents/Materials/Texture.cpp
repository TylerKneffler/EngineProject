#include "Texture.h"

#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>

namespace
{
std::string TextureIdentity(const std::string& path)
{
    std::error_code error;
    std::filesystem::path identity =
        std::filesystem::absolute(std::filesystem::path(path).lexically_normal(), error);
    if (error)
        identity = std::filesystem::path(path).lexically_normal();
    std::string key = identity.generic_string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
#endif
    return key;
}
}

std::shared_ptr<Texture> Texture::Acquire(const std::string& path)
{
    static std::unordered_map<std::string, std::weak_ptr<Texture>> registry;
    const std::string key = TextureIdentity(path);
    if (auto found = registry.find(key); found != registry.end())
        if (auto existing = found->second.lock())
            return existing;

    auto texture = std::shared_ptr<Texture>(new Texture(
        std::filesystem::path(path).lexically_normal().generic_string()));
    registry[key] = texture;
    return texture;
}

bool Texture::Load()
{
    if (!m_pixels.empty())
        return true;

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
        return false;

    bool success = false;
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;

        const std::wstring path = std::filesystem::path(m_filePath).wstring();
        if (SUCCEEDED(CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) &&
            SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(
                frame.Get(), GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(converter->GetSize(&m_width, &m_height)) &&
            m_width > 0 && m_height > 0)
        {
            const uint64_t size = static_cast<uint64_t>(m_width) * m_height * 4;
            if (size <= UINT32_MAX)
            {
                m_pixels.resize(static_cast<size_t>(size));
                success = SUCCEEDED(converter->CopyPixels(
                    nullptr, m_width * 4, static_cast<UINT>(size), m_pixels.data()));
            }
        }
    }

    if (!success)
    {
        m_pixels.clear();
        m_width = m_height = 0;
    }
    if (uninitialize)
        CoUninitialize();
    return success;
}

bool Texture::Prepare(IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider)
        return false;
    if (m_graphicsTexture && m_preparedProvider == graphicsProvider)
        return true;
    if (!Load())
        return false;
    IGraphicsTextureFactory* factory = graphicsProvider->GetTextureFactory();
    if (!factory)
        return false;
    try
    {
        m_graphicsTexture =
            factory->CreateTexture2D(m_width, m_height, m_pixels.data());
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA(("[Texture] GPU upload failed for " + m_filePath +
            ": " + error.what() + "\n").c_str());
        m_graphicsTexture.reset();
    }
    m_preparedProvider = m_graphicsTexture ? graphicsProvider : nullptr;
    return static_cast<bool>(m_graphicsTexture);
}
