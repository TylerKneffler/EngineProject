#include "VideoExporter/VideoExportApplication.h"

#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Audio/AudioSource.h"
#include "Core/Compoonents/Cinematics/CameraTrack.h"
#include "Core/Audio/AudioMixer.h"
#include "Core/Audio/Audio.h"
#include "Core/Model/ProjectSettings.h"
#include "Core/Object.h"
#include "Core/ProjectLoader.h"
#include "Core/Scene/Scene.h"
#include "Core/SceneManager.h"
#include "Core/Window.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/PostProcess.h"
#include "Core/Rendering/ExportFrameSequence.h"
#include "VideoExporter/SimulationBakeCache.h"
#include "VideoExporter/ExportFrameRate.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <shellapi.h>
#include <wincodec.h>
#include <tinyexr.h>

#ifndef PROJECT_FILE
#define PROJECT_FILE ""
#endif
#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

namespace Engine::Video
{
namespace
{
namespace fs = std::filesystem;

struct ExportOptions
{
    fs::path projectFile;
    std::string scene;
    fs::path output;
    std::wstring ffmpeg = L"ffmpeg.exe";
    std::string format = "mp4";
    std::string codec;
    std::string preset = "medium";
    uint32_t width = 0;
    uint32_t height = 0;
    Engine::Video::ExportFrameRate frameRate{};
    bool frameRateExplicit = false;
    uint32_t physicsFps = 120;
    uint32_t maximumPhysicsSubsteps = 8;
    uint32_t physicsSolverIterations = 10;
    float durationSeconds = 0.f;
    float timeoutSeconds = 120.f;
    int encoderQuality = 75;
    uint32_t renderQuality = 1;
    uint32_t msaaSamples = 1;
    uint32_t exportQuality = 0;
    bool hdrOutput = false;
    uint32_t hdrBits = 16u;
    uint32_t samplesPerFrame = 1u;
    bool subframeSampling = true;
    float shutter = 0.5f;
    float exportExposure = 1.f;
    uint32_t exportToneMapping = 1u;
    bool resume = false;
    bool encodeSequence = false;
    bool simulationCache = false;
    fs::path encodeOutput;
    uint64_t frameStart = 0u;
    uint64_t frameEnd = UINT64_MAX;
};

std::string Narrow(const std::wstring& value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(std::max(0, size)), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
            static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Widen(const std::string& value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(std::max(0, size)), L'\0');
    if (size > 0)
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
            static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring Quote(const std::wstring& value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t character : value)
    {
        if (character == L'\\')
        {
            ++slashes;
            continue;
        }
        if (character == L'\"')
            result.append(slashes * 2u + 1u, L'\\');
        else
            result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2u, L'\\');
    result.push_back(L'\"');
    return result;
}

bool ParseUnsigned(const std::wstring& value, uint32_t& destination)
{
    try
    {
        size_t consumed = 0;
        const unsigned long parsed = std::stoul(value, &consumed);
        if (consumed != value.size() || parsed > UINT32_MAX) return false;
        destination = static_cast<uint32_t>(parsed);
        return true;
    }
    catch (...) { return false; }
}

bool ParseUnsigned64(const std::wstring& value, uint64_t& destination)
{
    try
    {
        size_t consumed = 0;
        const unsigned long long parsed = std::stoull(value, &consumed);
        if (consumed != value.size()) return false;
        destination = static_cast<uint64_t>(parsed);
        return true;
    }
    catch (...) { return false; }
}

bool ParseFrameRate(const std::wstring& value,
    Engine::Video::ExportFrameRate& rate)
{
    const size_t separator = value.find(L'/');
    if (separator == std::wstring::npos)
    {
        rate.denominator = 1u;
        return ParseUnsigned(value, rate.numerator) && rate.IsValid();
    }
    if (value.find(L'/', separator + 1u) != std::wstring::npos ||
        !ParseUnsigned(value.substr(0, separator), rate.numerator) ||
        !ParseUnsigned(value.substr(separator + 1u), rate.denominator) ||
        !rate.IsValid())
        return false;
    rate.Normalize();
    return true;
}

bool ParseFloat(const std::wstring& value, float& destination)
{
    try { destination = std::stof(value); return std::isfinite(destination); }
    catch (...) { return false; }
}

void PrintUsage()
{
    std::cout <<
        "VideoExporter options:\n"
        "  --project <file.proj>  Project to load\n"
        "  --scene <file.scene>   Starting scene (default: project default)\n"
        "  --output <file>        Output video path\n"
        "  --format mp4|webm|mov|png|exr Output type or image sequence\n"
        "  --codec <ffmpeg codec> Optional codec override\n"
        "  --encoder-quality <0-100> Encoded-file quality (default 75)\n"
        "  --render-quality <1-4> Spatial render scale (default 1)\n"
        "  --msaa <1|2|4|8>      Multisample anti-aliasing (default 1)\n"
        "  --export-quality <0-3> 0 project, 1 low, 2 medium, 3 high\n"
        "  --hdr                  10-bit HDR10/PQ output when supported\n"
        "  --hdr-precision <16|32> FP16 or FP32 intermediate/render target\n"
        "  --export-exposure <0-64> Export composition exposure\n"
        "  --export-tone-map <0-2> 0 none, 1 ACES, 2 Reinhard\n"
        "  --samples-per-frame <1-64> Temporal samples to average\n"
        "  --subframe-sampling <0|1> Evaluate scene between output frames\n"
        "  --shutter <0.01-1> Exposure duration as a fraction of a frame\n"
        "  --preset <name>        FFmpeg speed preset (default medium)\n"
        "  --width/--height <px>  Even output dimensions\n"
        "  --fps <rate>           Output rate, e.g. 60 or 30000/1001\n"
        "  --physics-fps <steps>  Independent rigid-body rate (default 120)\n"
        "  --max-physics-substeps Maximum physics steps per output frame (default 8)\n"
        "  --physics-solver-iterations Bullet solver passes per step (default 10)\n"
        "  --duration <seconds>   Fixed duration; 0 follows camera track\n"
        "  --timeout <seconds>    Mandatory safety limit (default 120)\n"
        "  --format png|exr       Write an image sequence into --output directory\n"
        "  --resume               Resume matching sequence manifest\n"
        "  --frame-range <a:b>    Render selected frames [a,b)\n"
        "  --encode-sequence      Encode the completed selected image range\n"
        "  --encode-output <file> Final video path for separate encoding\n"
        "  --simulation-cache     Record/reuse a fixed-duration simulation bake\n"
        "  --ffmpeg <path>        FFmpeg executable\n";
}

bool ParseOptions(ExportOptions& options)
{
    options.projectFile = PROJECT_FILE;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return false;
    bool valid = true;
    for (int index = 1; index < count && valid; ++index)
    {
        const std::wstring key = arguments[index];
        if (key == L"--help" || key == L"-h")
        {
            PrintUsage();
            LocalFree(arguments);
            return false;
        }
        if (key == L"--hdr")
        {
            options.hdrOutput = true;
            continue;
        }
        if (key == L"--resume")
        {
            options.resume = true;
            continue;
        }
        if (key == L"--encode-sequence")
        {
            options.encodeSequence = true;
            continue;
        }
        if (key == L"--simulation-cache")
        {
            options.simulationCache = true;
            continue;
        }
        if (index + 1 >= count)
        {
            std::cerr << "Missing value for " << Narrow(key) << '\n';
            valid = false;
            break;
        }
        const std::wstring value = arguments[++index];
        if (key == L"--project") options.projectFile = value;
        else if (key == L"--scene") options.scene = Narrow(value);
        else if (key == L"--output") options.output = value;
        else if (key == L"--format") options.format = Narrow(value);
        else if (key == L"--codec") options.codec = Narrow(value);
        else if (key == L"--preset") options.preset = Narrow(value);
        else if (key == L"--ffmpeg") options.ffmpeg = value;
        else if (key == L"--width") valid = ParseUnsigned(value, options.width);
        else if (key == L"--height") valid = ParseUnsigned(value, options.height);
        else if (key == L"--fps")
        {
            valid = ParseFrameRate(value, options.frameRate);
            options.frameRateExplicit = valid;
        }
        else if (key == L"--physics-fps")
            valid = ParseUnsigned(value, options.physicsFps);
        else if (key == L"--max-physics-substeps")
            valid = ParseUnsigned(value, options.maximumPhysicsSubsteps);
        else if (key == L"--physics-solver-iterations")
            valid = ParseUnsigned(value, options.physicsSolverIterations);
        else if (key == L"--duration") valid = ParseFloat(value, options.durationSeconds);
        else if (key == L"--timeout") valid = ParseFloat(value, options.timeoutSeconds);
        else if (key == L"--encoder-quality" || key == L"--quality")
        {
            uint32_t quality = 0;
            valid = ParseUnsigned(value, quality);
            options.encoderQuality = static_cast<int>(std::min(quality, 100u));
        }
        else if (key == L"--render-quality")
            valid = ParseUnsigned(value, options.renderQuality) &&
                options.renderQuality >= 1u && options.renderQuality <= 4u;
        else if (key == L"--msaa")
            valid = ParseUnsigned(value, options.msaaSamples) &&
                (options.msaaSamples == 1u || options.msaaSamples == 2u ||
                 options.msaaSamples == 4u || options.msaaSamples == 8u);
        else if (key == L"--export-quality")
            valid = ParseUnsigned(value, options.exportQuality) &&
                options.exportQuality <= 3u;
        else if (key == L"--export-exposure")
            valid = ParseFloat(value, options.exportExposure) &&
                options.exportExposure >= 0.f && options.exportExposure <= 64.f;
        else if (key == L"--export-tone-map")
            valid = ParseUnsigned(value, options.exportToneMapping) &&
                options.exportToneMapping <= 2u;
        else if (key == L"--hdr-precision")
        {
            valid = ParseUnsigned(value, options.hdrBits) &&
                (options.hdrBits == 16u || options.hdrBits == 32u);
            options.hdrOutput = valid;
        }
        else if (key == L"--samples-per-frame")
            valid = ParseUnsigned(value, options.samplesPerFrame) &&
                options.samplesPerFrame >= 1u && options.samplesPerFrame <= 64u;
        else if (key == L"--subframe-sampling")
        {
            uint32_t enabled = 0;
            valid = ParseUnsigned(value, enabled) && enabled <= 1u;
            options.subframeSampling = enabled != 0u;
        }
        else if (key == L"--shutter")
            valid = ParseFloat(value, options.shutter) &&
                options.shutter >= 0.01f && options.shutter <= 1.f;
        else if (key == L"--encode-output") options.encodeOutput = value;
        else if (key == L"--frame-range")
        {
            const size_t separator = value.find(L':');
            if (separator == std::wstring::npos) valid = false;
            else
            {
                valid = ParseUnsigned64(value.substr(0, separator),
                    options.frameStart) &&
                    ParseUnsigned64(value.substr(separator + 1u),
                        options.frameEnd) && options.frameStart < options.frameEnd;
            }
        }
        else
        {
            std::cerr << "Unknown option: " << Narrow(key) << '\n';
            valid = false;
        }
    }
    LocalFree(arguments);
    return valid;
}

class FfmpegPipe
{
public:
    ~FfmpegPipe() { Finish(false); }
    const fs::path& VideoOnlyPath() const { return m_videoOnlyPath; }

    bool Start(const ExportOptions& options, uint32_t width, uint32_t height,
        const std::string& fps, std::string& error)
    {
        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE readPipe = nullptr;
        if (!CreatePipe(&readPipe, &m_writePipe, &security, 1024u * 1024u))
        {
            error = "Could not create the FFmpeg frame pipe.";
            return false;
        }
        SetHandleInformation(m_writePipe, HANDLE_FLAG_INHERIT, 0);

        const int quality = std::clamp(options.encoderQuality, 0, 100);
        const int crf = 38 - quality * 26 / 100;
        m_videoOnlyPath = options.output.parent_path() /
            (options.output.stem().wstring() + L".video-only" +
             options.output.extension().wstring());
        std::wostringstream command;
        command << Quote(options.ffmpeg)
            << L" -hide_banner -loglevel warning -y -f rawvideo -pix_fmt "
            << (options.hdrOutput ? L"rgba64le" : L"rgba")
            << L" -video_size " << width << L"x" << height
            << L" -framerate " << Widen(fps) << L" -i pipe:0 ";
        const std::string format = options.format;
        if (!options.codec.empty())
        {
            command << L"-c:v " << Widen(options.codec) << L" -crf " << crf;
            if (options.hdrOutput)
                command << L" -pix_fmt yuv420p10le";
            command << L' ';
        }
        else if (format == "webm")
        {
            command << L"-c:v libvpx-vp9 ";
            if (options.hdrOutput) command << L"-profile:v 2 -pix_fmt yuv420p10le ";
            command << L"-crf " << crf
                << L" -b:v 0 -row-mt 1 ";
        }
        else if (format == "mov")
        {
            const int profile = quality >= 90 ? 4 : quality >= 65 ? 3 : 2;
            command << L"-c:v prores_ks -profile:v " << profile
                << L" -pix_fmt yuv422p10le ";
        }
        else
        {
            command << L"-c:v libx264 -preset " << Widen(options.preset)
                << L" -crf " << crf
                << (options.hdrOutput ? L" -pix_fmt yuv420p10le " : L" -pix_fmt yuv420p ")
                << L" -movflags +faststart ";
        }
        if (options.hdrOutput)
            command << L"-color_primaries bt2020 -color_trc smpte2084 "
                << L"-colorspace bt2020nc ";
        command << Quote(m_videoOnlyPath.wstring());
        std::wstring mutableCommand = command.str();

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdInput = readPipe;
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(nullptr, mutableCommand.data(),
            nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
            &startup, &process);
        CloseHandle(readPipe);
        if (!created)
        {
            CloseHandle(m_writePipe);
            m_writePipe = nullptr;
            error = "Could not start FFmpeg. Install it or set --ffmpeg <path>.";
            return false;
        }
        m_process = process.hProcess;
        CloseHandle(process.hThread);
        return true;
    }

    bool Write(const std::vector<uint8_t>& pixels)
    {
        size_t offset = 0;
        while (offset < pixels.size())
        {
            const DWORD request = static_cast<DWORD>(std::min<size_t>(
                pixels.size() - offset, 16u * 1024u * 1024u));
            DWORD written = 0;
            if (!WriteFile(m_writePipe, pixels.data() + offset,
                request, &written, nullptr) || written == 0)
                return false;
            offset += written;
        }
        return true;
    }

    bool Finish(bool waitForEncoder)
    {
        if (m_writePipe)
        {
            CloseHandle(m_writePipe);
            m_writePipe = nullptr;
        }
        bool success = true;
        if (m_process)
        {
            if (waitForEncoder)
            {
                const DWORD wait = WaitForSingleObject(m_process, 30000);
                if (wait == WAIT_TIMEOUT)
                {
                    TerminateProcess(m_process, 2);
                    success = false;
                }
                DWORD exitCode = 1;
                GetExitCodeProcess(m_process, &exitCode);
                success = success && exitCode == 0;
            }
            CloseHandle(m_process);
            m_process = nullptr;
        }
        return success;
    }

private:
    HANDLE m_writePipe = nullptr;
    HANDLE m_process = nullptr;
    fs::path m_videoOnlyPath;
};

bool MuxOfflineAudio(const ExportOptions& options, const fs::path& videoPath,
    const fs::path& audioPath, std::string& error)
{
    std::wostringstream command;
    command << Quote(options.ffmpeg) << L" -hide_banner -loglevel warning -y"
        << L" -i " << Quote(videoPath.wstring())
        << L" -f f32le -ar 48000 -ac 2 -i " << Quote(audioPath.wstring())
        << L" -map 0:v:0 -map 1:a:0 -c:v copy ";
    if (options.format == "webm")
        command << L"-c:a libopus -b:a 160k ";
    else
        command << L"-c:a aac -b:a 192k ";
    command << L"-shortest ";
    if (options.format == "mp4") command << L"-movflags +faststart ";
    command << Quote(options.output.wstring());
    std::wstring mutableCommand = command.str();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    {
        error = "Could not start FFmpeg to mux offline audio.";
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || exitCode != 0)
    {
        error = "FFmpeg failed while muxing offline audio.";
        return false;
    }
    return true;
}

std::wstring FrameName(uint64_t frame, const wchar_t* extension)
{
    std::wostringstream name;
    name << L"frame_" << std::setw(8) << std::setfill(L'0') << frame
        << L'.' << extension;
    return name.str();
}

bool SavePng(const fs::path& path, uint32_t width, uint32_t height,
    const std::vector<uint8_t>& rgba)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    const fs::path temporary = path.wstring() + L".tmp";
    bool success = false;
    do
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) break;
        Microsoft::WRL::ComPtr<IWICStream> stream;
        if (FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE))) break;
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
            &encoder)) || FAILED(encoder->Initialize(stream.Get(),
                WICBitmapEncoderNoCache))) break;
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        Microsoft::WRL::ComPtr<IPropertyBag2> properties;
        if (FAILED(encoder->CreateNewFrame(&frame, &properties)) ||
            FAILED(frame->Initialize(properties.Get())) ||
            FAILED(frame->SetSize(width, height))) break;
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppRGBA;
        if (FAILED(frame->SetPixelFormat(&pixelFormat)) ||
            !IsEqualGUID(pixelFormat, GUID_WICPixelFormat32bppRGBA) ||
            FAILED(frame->WritePixels(height, width * 4u,
                static_cast<UINT>(rgba.size()),
                const_cast<BYTE*>(rgba.data()))) ||
            FAILED(frame->Commit()) || FAILED(encoder->Commit())) break;
        frame.Reset(); encoder.Reset(); stream.Reset();
        success = MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    } while (false);
    if (!success) fs::remove(temporary);
    if (uninitialize) CoUninitialize();
    return success;
}

bool SaveExr(const fs::path& path, uint32_t width, uint32_t height,
    uint32_t bits, const std::vector<float>& rgba)
{
    const fs::path temporary = path.wstring() + L".tmp.exr";
    const std::string outputPath = temporary.string();
    const char* error = nullptr;
    const int result = SaveEXR(rgba.data(), static_cast<int>(width),
        static_cast<int>(height), 4, bits == 16u ? 1 : 0,
        outputPath.c_str(), &error);
    if (result != TINYEXR_SUCCESS)
    {
        if (error) FreeEXRErrorMessage(error);
        fs::remove(temporary);
        return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

uint64_t HashText(const std::string& text)
{
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : text)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string HexText(const std::string& text)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(text.size() * 2u);
    for (const unsigned char byte : text)
    {
        encoded.push_back(digits[byte >> 4u]);
        encoded.push_back(digits[byte & 0xfu]);
    }
    return encoded;
}

bool LoadCompletedFrames(const fs::path& manifest, bool resume,
    uint64_t compatibility, std::set<uint64_t>& completed,
    std::string& error)
{
    if (!fs::exists(manifest))
    {
        if (resume)
        {
            error = "Cannot resume: the export manifest does not exist.";
            return false;
        }
        return true;
    }
    if (!resume)
    {
        error = "Export manifest already exists; use --resume or choose a new output directory.";
        return false;
    }
    std::ifstream input(manifest);
    std::string line;
    bool sawCompatibility = false;
    while (std::getline(input, line))
    {
        if (line.rfind("compatibility=", 0) == 0)
        {
            sawCompatibility = true;
            if (line.substr(14) != std::to_string(compatibility))
            {
                error = "Cannot resume: export settings or scene compatibility changed.";
                return false;
            }
        }
        else if (line.rfind("completed=", 0) == 0)
        {
            std::istringstream frames(line.substr(10));
            std::string value;
            while (std::getline(frames, value, ','))
            {
                try { completed.insert(std::stoull(value)); }
                catch (...) { error = "Export manifest has an invalid completed-frame list."; return false; }
            }
        }
    }
    if (!sawCompatibility)
    {
        error = "Cannot resume: export manifest is malformed.";
        return false;
    }
    return true;
}

bool WriteManifest(const fs::path& manifest, uint64_t compatibility,
    const std::string& format, uint32_t width, uint32_t height, const std::string& fps,
    uint64_t frameStart, uint64_t frameEnd, const std::string& settings,
    const std::set<uint64_t>& completed, const char* status)
{
    const fs::path temporary = manifest.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return false;
        output << "version=1\nstatus=" << status << "\ncompatibility="
            << compatibility << "\nformat=" << format << "\nwidth=" << width
            << "\nheight=" << height << "\nfps=" << fps
            << "\nselected_frame_start=" << frameStart
            << "\nselected_frame_end_exclusive=" << frameEnd
            << "\nsettings_hex=" << HexText(settings) << "\ncompleted=";
        bool first = true;
        for (const uint64_t frame : completed)
        {
            if (!first) output << ',';
            output << frame;
            first = false;
        }
        output << '\n';
        if (!output) return false;
    }
    return MoveFileExW(temporary.c_str(), manifest.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool EncodeSequence(const ExportOptions& options, const fs::path& directory,
    uint64_t startFrame, uint64_t endFrame, std::string& error)
{
    if (options.encodeOutput.empty())
    {
        error = "--encode-sequence requires --encode-output <file>.";
        return false;
    }
    const std::string format = options.encodeOutput.extension() == ".webm"
        ? "webm" : options.encodeOutput.extension() == ".mov" ? "mov" : "mp4";
    const bool exr = options.format == "exr";
    std::wstring patternName = L"frame_%08d";
    patternName += exr ? L".exr" : L".png";
    const std::wstring pattern = (directory / patternName).wstring();
    std::wostringstream command;
    command << Quote(options.ffmpeg) << L" -hide_banner -loglevel warning -y"
        << L" -framerate " << Widen(options.frameRate.ToString())
        << L" -start_number " << startFrame
        << L" -i " << Quote(pattern) << L" -frames:v " << endFrame - startFrame;
    const int crf = 38 - std::clamp(options.encoderQuality, 0, 100) * 26 / 100;
    if (format == "webm")
        command << L" -c:v libvpx-vp9 -crf " << crf << L" -b:v 0";
    else if (format == "mov")
        command << L" -c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le";
    else
        command << L" -c:v libx264 -preset " << Widen(options.preset)
            << L" -crf " << crf << L" -pix_fmt yuv420p -movflags +faststart";
    command << L' ' << Quote(options.encodeOutput.wstring());
    std::wstring mutableCommand = command.str();
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    {
        error = "Could not start FFmpeg for image-sequence encoding.";
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || exitCode != 0)
    {
        error = "FFmpeg failed while encoding the image sequence.";
        return false;
    }
    return true;
}

fs::path DiscoverProject(const fs::path& configured)
{
    if (!configured.empty() && fs::is_regular_file(configured))
        return fs::weakly_canonical(configured);
    std::vector<fs::path> projects;
    for (const auto& entry : fs::directory_iterator(fs::current_path()))
        if (entry.is_regular_file() && entry.path().extension() == ".proj")
            projects.push_back(entry.path());
    return projects.size() == 1u ? fs::weakly_canonical(projects.front())
        : fs::path{};
}
}

int VideoExportApplication::Run(HINSTANCE instance)
{
    ExportOptions options;
    if (!ParseOptions(options))
        return 2;
    options.projectFile = DiscoverProject(options.projectFile);
    Engine::Model::ProjectSettings settings;
    if (!options.projectFile.empty())
    {
        try
        {
            settings = Engine::Core::ProjectLoader{}.LoadProject(
                options.projectFile.string());
            fs::current_path(options.projectFile.parent_path());
        }
        catch (const std::exception& error)
        {
            std::cerr << "Could not load project: " << error.what() << '\n';
            return 3;
        }
    }
    else
    {
        settings.viewportWidth = 1280;
        settings.viewportHeight = 720;
        settings.gameWindowWidth = 1280;
        settings.gameWindowHeight = 720;
        settings.targetFramerate = 60;
        settings.clearColor = { 0.1f, 0.1f, 0.1f, 1.f };
        settings.defaultScene = std::string(ENGINE_ASSETS_PATH) +
            "Scenes/Cinematics/camera_track_demo.scene";
    }

    options.width = options.width ? options.width :
        (settings.gameWindowWidth ? settings.gameWindowWidth : settings.viewportWidth);
    options.height = options.height ? options.height :
        (settings.gameWindowHeight ? settings.gameWindowHeight : settings.viewportHeight);
    if (!options.frameRateExplicit)
        options.frameRate.numerator = std::clamp(
            settings.targetFramerate ? settings.targetFramerate : 60u, 1u, 240u);
    options.frameRate.Normalize();
    if (!options.frameRate.IsValid())
    {
        std::cerr << "Output frame rate must be between 1 and 240 FPS.\n";
        return 4;
    }
    options.physicsFps = std::clamp(options.physicsFps, 1u, 1000u);
    options.maximumPhysicsSubsteps = std::clamp(
        options.maximumPhysicsSubsteps, 1u, 1024u);
    options.physicsSolverIterations = std::clamp(
        options.physicsSolverIterations, 1u, 256u);
    const uint32_t requiredPhysicsSubsteps = static_cast<uint32_t>(
        (static_cast<uint64_t>(options.physicsFps) * options.frameRate.denominator +
            options.frameRate.numerator - 1u) / options.frameRate.numerator);
    if (options.maximumPhysicsSubsteps < requiredPhysicsSubsteps)
    {
        std::cerr << "Maximum physics substeps must be at least "
            << requiredPhysicsSubsteps << " for " << options.physicsFps
            << " physics steps/s at " << options.frameRate.ToString()
            << " output FPS.\n";
        return 4;
    }
    options.width = std::clamp(options.width & ~1u, 2u, 7680u);
    options.height = std::clamp(options.height & ~1u, 2u, 4320u);
    options.timeoutSeconds = std::clamp(options.timeoutSeconds, 1.f, 86400.f);
    options.durationSeconds = std::clamp(options.durationSeconds, 0.f,
        options.timeoutSeconds);
    std::transform(options.format.begin(), options.format.end(),
        options.format.begin(), [](unsigned char character)
        { return static_cast<char>(std::tolower(character)); });
    if (options.format != "mp4" && options.format != "webm" &&
        options.format != "mov" && options.format != "png" &&
        options.format != "exr")
    {
        std::cerr << "Unsupported format. Use mp4, webm, mov, png, or exr.\n";
        return 4;
    }
    const bool imageSequence = options.format == "png" || options.format == "exr";
    if (options.format == "exr")
        options.hdrOutput = true;
    if (options.format == "png" && options.hdrOutput)
    {
        std::cerr << "HDR is stored as linear float by EXR; PNG sequence output is SDR.\n";
        return 4;
    }
    if ((options.resume || options.frameEnd != UINT64_MAX || options.frameStart != 0u ||
        options.encodeSequence) && !imageSequence)
    {
        std::cerr << "Resume, frame ranges, and sequence encoding require --format png or exr.\n";
        return 4;
    }
    if (options.encodeSequence && options.encodeOutput.empty())
    {
        std::cerr << "--encode-sequence requires --encode-output <file>.\n";
        return 4;
    }
    const std::string scenePath = options.scene.empty()
        ? settings.defaultScene : options.scene;
    if (scenePath.empty())
    {
        std::cerr << "No scene was selected and the project has no default scene.\n";
        return 5;
    }
    if (options.output.empty())
    {
        const std::string stem = fs::path(scenePath).stem().string();
        options.output = fs::path("VideoExports") /
            (imageSequence ? stem : stem + "." + options.format);
    }
    // The selected type owns the container extension. This prevents a WebM
    // request from accidentally being muxed as MP4 because an older output
    // name was left in the editor field.
    if (!imageSequence)
        options.output.replace_extension(options.format);
    if (imageSequence)
        fs::create_directories(options.output);
    else if (!options.output.parent_path().empty())
        fs::create_directories(options.output.parent_path());
    if (options.encodeSequence &&
        !options.encodeOutput.parent_path().empty())
        fs::create_directories(options.encodeOutput.parent_path());

    auto shadowSettings = settings.realtimeShadows;
    auto lightingSettings = settings.distanceLighting;
    if (options.exportQuality > 0u)
    {
        const uint32_t q = options.exportQuality;
        shadowSettings.directionalResolution = q == 1u ? 512u : q == 2u ? 1024u : 2048u;
        shadowSettings.directionalCascadeCount = q == 1u ? 1u : q == 2u ? 2u : 4u;
        shadowSettings.portalAtlasResolution = q == 1u ? 256u : q == 2u ? 512u : 1024u;
        for (auto& band : lightingSettings.bands)
        {
            band.maxRealtimeLights = q == 1u ? 2u : q == 2u ? 8u : 32u;
            band.normalMapping = q >= 2u;
            band.parallaxMapping = q >= 3u;
            band.environmentDiffuse = q >= 2u;
            band.reflections = q >= 2u;
            band.realtimeShadows = q >= 2u;
        }
        lightingSettings.maximumRealtimeLights = q == 1u ? 4u : q == 2u ? 16u : 64u;
        lightingSettings.clusteredLighting = q >= 2u;
    }
    Engine::Graphics::SetPostProcessSettings({
        options.exportExposure, options.exportToneMapping });

    const uint32_t renderWidth = options.width * options.renderQuality;
    const uint32_t renderHeight = options.height * options.renderQuality;
    if (renderWidth > 16384u || renderHeight > 16384u)
    {
        std::cerr << "Render scale exceeds the 16384-pixel internal target limit.\n";
        return 6;
    }
    Engine::Core::Window window(instance, L"Engine Video Export",
        renderWidth, renderHeight);
    Engine::Renderers::DX11GameRenderer renderer;
    if (!renderer.Init(window.GetHWND(), renderWidth, renderHeight))
    {
        std::cerr << "Could not initialize the DirectX 11 export renderer.\n";
        return 6;
    }
    if (!renderer.EnableOffscreenExport(options.width, options.height,
        options.msaaSamples, 3u, options.hdrOutput, options.hdrBits,
        options.format == "exr"))
    {
        std::cerr << "Could not create the off-screen export target or readback queue.\n";
        return 6;
    }
    Engine::Scene::Scene scene;
    if (!imageSequence &&
        !Engine::Audio::AudioMixer::Get().ConfigureOffline(48000u, 2u))
    {
        std::cerr << "Could not initialize the offline audio mixer.\n";
        return 6;
    }
    scene.Init(renderer.GetGraphicsProvider());
    scene.SetRealtimeShadowSettings(shadowSettings);
    scene.SetDistanceLightingSettings(lightingSettings);
    Engine::Core::SceneManager::SetActiveScene(&scene);
    Engine::Core::SceneManager::SetDefaultScenePath(scenePath);
    if (!scene.Load(scenePath))
    {
        std::cerr << "Could not load scene: " << scenePath << '\n';
        return 7;
    }
    const uint32_t samplesPerOutputFrame = options.subframeSampling
        ? options.samplesPerFrame : 1u;
    const uint32_t simulationTicksPerOutputFrame = samplesPerOutputFrame;
    const uint32_t exposureTicks = options.subframeSampling
        ? std::max(1u, static_cast<uint32_t>(std::ceil(
            options.shutter * simulationTicksPerOutputFrame))) : 1u;
    const double fixedDelta = static_cast<double>(options.frameRate.denominator) /
        (static_cast<double>(options.frameRate.numerator) *
            simulationTicksPerOutputFrame);
    scene.SetFixedTimeStep(fixedDelta);
    scene.GetPhysics().ConfigureFixedStep(
        1.0 / static_cast<double>(options.physicsFps),
        options.maximumPhysicsSubsteps, options.physicsSolverIterations);
    scene.Start();

    const uint64_t maximumFrames = options.frameRate.FramesForDuration(
        options.timeoutSeconds);
    const uint64_t requestedFrames = options.durationSeconds > 0.f
        ? options.frameRate.FramesForDuration(options.durationSeconds)
        : maximumFrames;
    const uint64_t frameLimit = std::min(maximumFrames, requestedFrames);
    const uint64_t selectedFrameStart = options.frameStart;
    const uint64_t selectedFrameEnd = options.frameEnd == UINT64_MAX
        ? frameLimit : std::min(options.frameEnd, frameLimit);
    if (selectedFrameStart >= selectedFrameEnd)
    {
        std::cerr << "Selected frame range is outside the export frame range.\n";
        return 4;
    }
    bool cacheEnabled = options.simulationCache && options.durationSeconds > 0.f;
    if (options.simulationCache && !cacheEnabled)
        std::cerr << "Simulation cache requires a fixed --duration; cache disabled.\n";
    if (cacheEnabled)
    {
        std::vector<Engine::Core::Object*> pending;
        for (const auto& object : scene.GetObjects())
            pending.push_back(object.get());
        bool hasCameraTrack = false;
        bool hasAudioSource = false;
        while (!pending.empty())
        {
            Engine::Core::Object* object = pending.back();
            pending.pop_back();
            if (object->GetComponent<CameraTrack>())
                hasCameraTrack = true;
            if (object->GetComponent<Engine::Components::AudioSource>())
                hasAudioSource = true;
            pending.insert(pending.end(), object->Children.begin(),
                object->Children.end());
        }
        if (hasCameraTrack)
        {
            std::cerr << "Simulation cache cannot replay an animated camera track; "
                << "cache disabled.\n";
            cacheEnabled = false;
        }
        if (hasAudioSource)
        {
            std::cerr << "Simulation cache does not include audio transport state; "
                << "cache disabled.\n";
            cacheEnabled = false;
        }
    }
    const uint64_t expectedCacheSamples = frameLimit * samplesPerOutputFrame;
    std::filesystem::path cachePath = options.output;
    if (imageSequence)
        cachePath /= L"simulation-bake.bin";
    else
        cachePath += L".simcache";
    uint64_t simulationCompatibility =
        Engine::Video::SimulationBakeCache::BuildCompatibilitySignature(scene);
    auto combineCompatibility = [&simulationCompatibility](const auto& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t index = 0; index < sizeof(value); ++index)
        {
            simulationCompatibility ^= bytes[index];
            simulationCompatibility *= 1099511628211ull;
        }
    };
    combineCompatibility(options.frameRate.numerator);
    combineCompatibility(options.frameRate.denominator);
    combineCompatibility(options.physicsFps);
    combineCompatibility(options.maximumPhysicsSubsteps);
    combineCompatibility(options.physicsSolverIterations);
    combineCompatibility(samplesPerOutputFrame);
    combineCompatibility(exposureTicks);
    Engine::Video::SimulationBakeCache simulationCache;
    if (!simulationCache.Initialize(scene, cachePath, simulationCompatibility,
        expectedCacheSamples, cacheEnabled))
    {
        std::cerr << "Could not initialize simulation bake cache: "
            << cachePath.string() << '\n';
        return 4;
    }
    cacheEnabled = simulationCache.GetMode() !=
        Engine::Video::SimulationBakeCache::Mode::Disabled;
    std::set<uint64_t> completedFrames;
    fs::path manifestPath;
    std::string compatibilitySettings;
    uint64_t compatibility = 0u;
    if (imageSequence)
    {
        std::ostringstream signature;
        signature << options.projectFile.string() << '\n' << scenePath << '\n'
            << options.format << '\n' << options.width << 'x' << options.height
            << '\n' << options.frameRate.ToString() << '\n' << options.physicsFps << '\n'
            << options.maximumPhysicsSubsteps << '\n'
            << options.physicsSolverIterations << '\n' << options.renderQuality
            << '\n' << options.msaaSamples << '\n' << options.exportQuality
            << '\n' << options.hdrOutput << ':' << options.hdrBits << '\n'
            << options.samplesPerFrame << ':' << options.subframeSampling << ':'
            << options.shutter << '\n' << options.exportExposure << ':'
            << options.exportToneMapping << '\n' << options.durationSeconds
            << ':' << options.timeoutSeconds;
        std::error_code fileError;
        const fs::path sceneFile = fs::weakly_canonical(scenePath, fileError);
        if (!fileError && fs::exists(sceneFile, fileError))
            signature << '\n' << fs::file_size(sceneFile, fileError) << ':'
                << fs::last_write_time(sceneFile, fileError).time_since_epoch().count();
        if (!options.projectFile.empty() && fs::exists(options.projectFile, fileError))
            signature << '\n' << fs::file_size(options.projectFile, fileError) << ':'
                << fs::last_write_time(options.projectFile, fileError).time_since_epoch().count();
        compatibilitySettings = signature.str();
        compatibility = HashText(compatibilitySettings);
        manifestPath = options.output / L"export-manifest.txt";
        std::string manifestError;
        if (!LoadCompletedFrames(manifestPath, options.resume, compatibility,
            completedFrames, manifestError))
        {
            std::cerr << manifestError << '\n';
            return 4;
        }
        for (uint64_t frame = selectedFrameStart; frame < selectedFrameEnd; ++frame)
        {
            const fs::path framePath = options.output / FrameName(frame,
                options.format == "png" ? L"png" : L"exr");
            if (!options.resume && fs::exists(framePath))
            {
                std::cerr << "Frame output already exists; use --resume or choose another directory.\n";
                return 4;
            }
            if (!fs::exists(framePath)) completedFrames.erase(frame);
        }
        if (!WriteManifest(manifestPath, compatibility, options.format,
            options.width, options.height, options.frameRate.ToString(), selectedFrameStart,
            selectedFrameEnd, compatibilitySettings, completedFrames, "rendering"))
        {
            std::cerr << "Could not write export manifest.\n";
            return 4;
        }
    }

    std::string encoderError;
    FfmpegPipe encoder;
    if (!imageSequence && !encoder.Start(options, options.width, options.height,
        options.frameRate.ToString(), encoderError))
    {
        std::cerr << encoderError << '\n';
        return 8;
    }
    constexpr uint32_t audioSampleRate = 48000u;
    constexpr uint32_t audioChannels = 2u;
    fs::path offlineAudioPath = options.output;
    offlineAudioPath += L".audio.f32le";
    std::ofstream offlineAudio;
    if (!imageSequence)
    {
        offlineAudio.open(offlineAudioPath, std::ios::binary | std::ios::trunc);
        if (!offlineAudio)
        {
            std::cerr << "Could not create temporary offline audio output.\n";
            return 8;
        }
    }
    Engine::Rendering::ExportFrameSequence frameSequence(scene);
    uint64_t audioSimulationTick = 0u;
    uint64_t writtenAudioFrames = 0u;
    const auto advanceSimulation = [&](uint32_t steps)
    {
        if (imageSequence)
        {
            frameSequence.AdvanceFixedSteps(steps);
            return true;
        }
        return frameSequence.AdvanceFixedSteps(steps,
            [&](double sceneTime, double)
            {
                const uint64_t targetAudioFrames = static_cast<uint64_t>(
                    static_cast<long double>(audioSimulationTick + 1u) *
                    audioSampleRate * options.frameRate.denominator /
                    (static_cast<long double>(options.frameRate.numerator) *
                        samplesPerOutputFrame));
                const uint64_t framesToRender = targetAudioFrames - writtenAudioFrames;
                std::vector<float> samples(static_cast<size_t>(framesToRender) *
                    audioChannels);
                scene.GetAudio().SynchronizeToTime(sceneTime);
                if (!Engine::Audio::AudioMixer::Get().ReadOfflineFrames(
                    samples.data(), framesToRender))
                    return false;
                offlineAudio.write(reinterpret_cast<const char*>(samples.data()),
                    static_cast<std::streamsize>(samples.size() * sizeof(float)));
                if (!offlineAudio.good()) return false;
                writtenAudioFrames = targetAudioFrames;
                ++audioSimulationTick;
                return true;
            });
    };
    bool sawCameraTrack = false;
    bool success = true;
    uint64_t framesWritten = 0;
    std::vector<uint8_t> pixels;
    const auto consumeReadback = [&](bool wait)
    {
        if (!renderer.ReadExportFrameRGBA(pixels, wait))
            return !wait;
        if (!encoder.Write(pixels))
            return false;
        ++framesWritten;
        return true;
    };
    const uint64_t simulationFrameLimit = imageSequence && !cacheEnabled
        ? selectedFrameEnd : frameLimit;
    for (uint64_t frame = 0; frame < simulationFrameLimit; ++frame)
    {
        const bool selected = frame >= selectedFrameStart && frame < selectedFrameEnd;
        const bool alreadyComplete = selected && options.resume &&
            completedFrames.find(frame) != completedFrames.end() && fs::exists(options.output /
                FrameName(frame, options.format == "png" ? L"png" : L"exr"));
        Engine::Components::Camera* camera = nullptr;
        uint32_t currentTick = 0u;
        const bool renderFrame = selected && !alreadyComplete;
        if (renderFrame)
            renderer.BeginExportFrame(samplesPerOutputFrame);
        if (renderFrame || cacheEnabled)
        {
            for (uint32_t sample = 0; sample < samplesPerOutputFrame; ++sample)
            {
                const uint32_t sampleTick = samplesPerOutputFrame == 1u ? 0u
                    : static_cast<uint32_t>((static_cast<uint64_t>(sample) *
                        (exposureTicks - 1u)) / (samplesPerOutputFrame - 1u));
                if (sampleTick > currentTick)
                {
                    if (!simulationCache.IsReplaying())
                    {
                        if (!advanceSimulation(sampleTick - currentTick))
                        {
                            std::cerr << "Could not render synchronized offline audio.\n";
                            success = false;
                            break;
                        }
                    }
                    currentTick = sampleTick;
                }
                frameSequence.PrepareCurrentFrame();
                if (simulationCache.IsReplaying() &&
                    !simulationCache.ApplyNext(scene))
                {
                    std::cerr << "Simulation bake is incompatible or corrupt at sample "
                        << frame * samplesPerOutputFrame + sample << ".\n";
                    success = false;
                    break;
                }
                if (simulationCache.GetMode() ==
                    Engine::Video::SimulationBakeCache::Mode::Recording &&
                    !simulationCache.CaptureCurrent(scene))
                {
                    std::cerr << "Simulation state could not be captured at sample "
                        << frame * samplesPerOutputFrame + sample << ".\n";
                    success = false;
                    break;
                }
                if (!renderFrame) continue;
                renderer.BeginFrame();
                std::unique_ptr<Engine::Graphics::IGraphicsContext> context =
                    renderer.CreateFrameGraphicsContext();
                camera = scene.FindGameCamera();
                if (context && camera)
                    scene.Render(context.get(), static_cast<float>(options.width) /
                        static_cast<float>(options.height), camera, false,
                        renderWidth, renderHeight, true);
                renderer.Clear(settings.clearColor.r, settings.clearColor.g,
                    settings.clearColor.b, settings.clearColor.a);
                if (context && camera)
                    scene.Render(context.get(), static_cast<float>(options.width) /
                        static_cast<float>(options.height), camera, false);
                renderer.EndFrame();
            }
        }
        if (!success) break;
        if (!simulationCache.IsReplaying() && currentTick < simulationTicksPerOutputFrame)
        {
            if (!advanceSimulation(simulationTicksPerOutputFrame - currentTick))
            {
                std::cerr << "Could not render synchronized offline audio.\n";
                success = false;
                break;
            }
        }
        camera = scene.FindGameCamera();
        if (selected && !alreadyComplete)
        {
            bool saved = false;
            const fs::path framePath = options.output / FrameName(frame,
                options.format == "png" ? L"png" : L"exr");
            if (imageSequence && options.format == "png")
            {
                if (renderer.ReadExportFrameRGBA(pixels, true))
                    saved = SavePng(framePath, options.width, options.height, pixels);
            }
            else if (imageSequence)
            {
                std::vector<float> linearPixels;
                if (renderer.ReadExportFrameFloatRGBA(linearPixels, true))
                    saved = SaveExr(framePath, options.width, options.height,
                        options.hdrBits, linearPixels);
            }
            else
            {
                saved = consumeReadback(false) &&
                    (renderer.GetPendingExportFrameCount() <
                        renderer.GetExportReadbackCapacity() || consumeReadback(true));
            }
            if (!saved)
            {
                std::cerr << "Could not save or encode output for frame " << frame << ".\n";
                success = false;
                break;
            }
            if (imageSequence)
            {
                completedFrames.insert(frame);
                ++framesWritten;
                success = WriteManifest(manifestPath, compatibility, options.format,
                    options.width, options.height, options.frameRate.ToString(), selectedFrameStart,
                    selectedFrameEnd, compatibilitySettings, completedFrames,
                    "rendering");
                if (!success) break;
            }
        }

        CameraTrack* track = camera && camera->Owner
            ? camera->Owner->GetComponent<CameraTrack>() : nullptr;
        sawCameraTrack = sawCameraTrack || track != nullptr;
        if (options.durationSeconds <= 0.f && sawCameraTrack && track &&
            track->IsFinished())
            break;
    }
    if (success && cacheEnabled && !simulationCache.Complete())
    {
        std::cerr << "Simulation bake did not complete all expected samples.\n";
        success = false;
    }
    while (success && renderer.GetPendingExportFrameCount() > 0u)
    {
        if (!consumeReadback(true))
        {
            std::cerr << "Could not drain the asynchronous export readback queue.\n";
            success = false;
        }
    }
    renderer.WaitIdle();
    success = encoder.Finish(true) && success;
    if (success && !imageSequence && simulationCache.IsReplaying())
    {
        const uint64_t silenceFrames = options.frameRate.AudioFramesAt(
            simulationFrameLimit, audioSampleRate);
        std::vector<float> silence(65536u * audioChannels, 0.f);
        uint64_t remaining = silenceFrames;
        while (remaining > 0u)
        {
            const uint64_t chunk = std::min<uint64_t>(remaining, 65536u);
            offlineAudio.write(reinterpret_cast<const char*>(silence.data()),
                static_cast<std::streamsize>(chunk * audioChannels * sizeof(float)));
            remaining -= chunk;
        }
        success = offlineAudio.good();
    }
    offlineAudio.close();
    if (success && !imageSequence)
    {
        std::string muxError;
        if (!MuxOfflineAudio(options, encoder.VideoOnlyPath(), offlineAudioPath,
            muxError))
        {
            std::cerr << muxError << '\n';
            success = false;
        }
    }
    if (!imageSequence)
    {
        std::error_code ignored;
        fs::remove(encoder.VideoOnlyPath(), ignored);
        fs::remove(offlineAudioPath, ignored);
    }
    bool sequenceComplete = false;
    if (imageSequence)
    {
        sequenceComplete = success;
        for (uint64_t frame = selectedFrameStart;
            sequenceComplete && frame < selectedFrameEnd; ++frame)
        {
            sequenceComplete = completedFrames.find(frame) != completedFrames.end() &&
                fs::is_regular_file(options.output / FrameName(frame,
                    options.format == "png" ? L"png" : L"exr"));
        }
        if (!WriteManifest(manifestPath, compatibility, options.format,
            options.width, options.height, options.frameRate.ToString(), selectedFrameStart,
            selectedFrameEnd, compatibilitySettings, completedFrames,
            sequenceComplete ? (options.encodeSequence ? "encoding" : "complete")
                : "partial"))
            success = false;
        if (!sequenceComplete)
            success = false;
        if (options.encodeSequence)
        {
            std::string sequenceError;
            if (!sequenceComplete || !EncodeSequence(options, options.output,
                selectedFrameStart, selectedFrameEnd, sequenceError))
            {
                if (sequenceError.empty())
                    sequenceError = "Selected image sequence range is incomplete; cannot encode it.";
                std::cerr << sequenceError << '\n';
                WriteManifest(manifestPath, compatibility, options.format,
                    options.width, options.height, options.frameRate.ToString(), selectedFrameStart,
                    selectedFrameEnd, compatibilitySettings, completedFrames,
                    "encode_failed");
                success = false;
            }
            else if (!WriteManifest(manifestPath, compatibility, options.format,
                options.width, options.height, options.frameRate.ToString(), selectedFrameStart,
                selectedFrameEnd, compatibilitySettings, completedFrames,
                "complete"))
                success = false;
        }
    }
    Engine::Core::SceneManager::SetActiveScene(nullptr);
    if (!success)
        return 9;
    std::cout << (imageSequence ? "Image sequence complete: " : "Video export complete: ")
        << options.output.string() << " (" << framesWritten << " frames)\n";
    return 0;
}
}
