#include "PngPaintWriter.h"
#include <algorithm>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace Engine::Editor::Paint
{
bool SavePng(const std::filesystem::path& path, uint32_t width,
    uint32_t height, const std::vector<uint8_t>& rgba, std::string& error)
{
    if (!width || !height || rgba.size() !=
        static_cast<size_t>(width) * height * 4)
    { error = "Invalid RGBA image dimensions."; return false; }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    { error = "Could not initialize the PNG encoder."; return false; }
    const bool uninitialize = SUCCEEDED(initialized);
    const std::filesystem::path temporary = path.wstring() + L".paint.tmp";
    bool saved = false;
    const char* stage = "create WIC factory";
    do
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) break;
        stage = "open temporary PNG";
        Microsoft::WRL::ComPtr<IWICStream> stream;
        if (FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(temporary.c_str(),
                GENERIC_WRITE))) break;
        stage = "create PNG encoder";
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                &encoder)) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
            break;
        stage = "create PNG frame";
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        Microsoft::WRL::ComPtr<IPropertyBag2> properties;
        if (FAILED(encoder->CreateNewFrame(&frame, &properties)) ||
            FAILED(frame->Initialize(properties.Get())) ||
            FAILED(frame->SetSize(width, height))) break;
        stage = "write PNG pixels";
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        std::vector<uint8_t> bgra(rgba);
        for (size_t pixel = 0; pixel < bgra.size(); pixel += 4)
            std::swap(bgra[pixel], bgra[pixel + 2]);
        if (FAILED(frame->SetPixelFormat(&format)) ||
            !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) ||
            FAILED(frame->WritePixels(height, width * 4u,
                static_cast<UINT>(bgra.size()), bgra.data())) ||
            FAILED(frame->Commit()) || FAILED(encoder->Commit())) break;
        frame.Reset(); encoder.Reset(); stream.Reset();
        stage = "replace image file";
        saved = MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    } while (false);
    if (!saved)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = std::string("Could not ") + stage + ": " + path.string();
    }
    else error.clear();
    if (uninitialize) CoUninitialize();
    return saved;
}
}
