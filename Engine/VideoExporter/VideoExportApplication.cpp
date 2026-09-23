#include "VideoExporter/VideoExportApplication.h"

#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Cinematics/CameraTrack.h"
#include "Core/Model/ProjectSettings.h"
#include "Core/Object.h"
#include "Core/ProjectLoader.h"
#include "Core/Scene/Scene.h"
#include "Core/SceneManager.h"
#include "Core/Window.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <shellapi.h>

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
    uint32_t fps = 0;
    float durationSeconds = 0.f;
    float timeoutSeconds = 120.f;
    int quality = 75;
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
    try { destination = static_cast<uint32_t>(std::stoul(value)); return true; }
    catch (...) { return false; }
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
        "  --format mp4|webm|mov  Output type\n"
        "  --codec <ffmpeg codec> Optional codec override\n"
        "  --quality <0-100>      Visual quality (default 75)\n"
        "  --preset <name>        FFmpeg speed preset (default medium)\n"
        "  --width/--height <px>  Even output dimensions\n"
        "  --fps <frames>         Fixed simulation/output rate\n"
        "  --duration <seconds>   Fixed duration; 0 follows camera track\n"
        "  --timeout <seconds>    Mandatory safety limit (default 120)\n"
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
        else if (key == L"--fps") valid = ParseUnsigned(value, options.fps);
        else if (key == L"--duration") valid = ParseFloat(value, options.durationSeconds);
        else if (key == L"--timeout") valid = ParseFloat(value, options.timeoutSeconds);
        else if (key == L"--quality")
        {
            uint32_t quality = 0;
            valid = ParseUnsigned(value, quality);
            options.quality = static_cast<int>(std::min(quality, 100u));
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

    bool Start(const ExportOptions& options, uint32_t width, uint32_t height,
        uint32_t fps, std::string& error)
    {
        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE readPipe = nullptr;
        if (!CreatePipe(&readPipe, &m_writePipe, &security, 1024u * 1024u))
        {
            error = "Could not create the FFmpeg frame pipe.";
            return false;
        }
        SetHandleInformation(m_writePipe, HANDLE_FLAG_INHERIT, 0);

        const int quality = std::clamp(options.quality, 0, 100);
        const int crf = 38 - quality * 26 / 100;
        std::wostringstream command;
        command << Quote(options.ffmpeg)
            << L" -hide_banner -loglevel warning -y -f rawvideo -pix_fmt rgba"
            << L" -video_size " << width << L"x" << height
            << L" -framerate " << fps << L" -i pipe:0 -an ";
        const std::string format = options.format;
        if (!options.codec.empty())
        {
            command << L"-c:v " << Widen(options.codec) << L" -crf " << crf << L' ';
        }
        else if (format == "webm")
        {
            command << L"-c:v libvpx-vp9 -crf " << crf
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
                << L" -pix_fmt yuv420p -movflags +faststart ";
        }
        command << Quote(options.output.wstring());
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
};

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
    options.fps = std::clamp(options.fps ? options.fps :
        (settings.targetFramerate ? settings.targetFramerate : 60u), 1u, 240u);
    options.width = std::clamp(options.width & ~1u, 2u, 7680u);
    options.height = std::clamp(options.height & ~1u, 2u, 4320u);
    options.timeoutSeconds = std::clamp(options.timeoutSeconds, 1.f, 86400.f);
    options.durationSeconds = std::clamp(options.durationSeconds, 0.f,
        options.timeoutSeconds);
    std::transform(options.format.begin(), options.format.end(),
        options.format.begin(), [](unsigned char character)
        { return static_cast<char>(std::tolower(character)); });
    if (options.format != "mp4" && options.format != "webm" &&
        options.format != "mov")
    {
        std::cerr << "Unsupported format. Use mp4, webm, or mov.\n";
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
            (stem + "." + options.format);
    }
    // The selected type owns the container extension. This prevents a WebM
    // request from accidentally being muxed as MP4 because an older output
    // name was left in the editor field.
    options.output.replace_extension(options.format);
    if (!options.output.parent_path().empty())
        fs::create_directories(options.output.parent_path());

    Engine::Core::Window window(instance, L"Engine Video Export",
        options.width, options.height);
    Engine::Renderers::DX11GameRenderer renderer;
    if (!renderer.Init(window.GetHWND(), options.width, options.height))
    {
        std::cerr << "Could not initialize the DirectX 11 export renderer.\n";
        return 6;
    }
    Engine::Scene::Scene scene;
    scene.SetEditorMode2D(
        settings.editorMode == Engine::Model::ProjectSettings::EditorMode::TwoD);
    scene.Init(renderer.GetGraphicsProvider());
    scene.SetDistanceLightingSettings(settings.distanceLighting);
    Engine::Core::SceneManager::SetActiveScene(&scene);
    Engine::Core::SceneManager::SetDefaultScenePath(scenePath);
    if (!scene.Load(scenePath))
    {
        std::cerr << "Could not load scene: " << scenePath << '\n';
        return 7;
    }
    scene.Start();

    std::string encoderError;
    FfmpegPipe encoder;
    if (!encoder.Start(options, options.width, options.height,
        options.fps, encoderError))
    {
        std::cerr << encoderError << '\n';
        return 8;
    }

    const float fixedDelta = 1.f / static_cast<float>(options.fps);
    const uint64_t maximumFrames = static_cast<uint64_t>(std::ceil(
        options.timeoutSeconds * options.fps));
    const uint64_t requestedFrames = options.durationSeconds > 0.f
        ? static_cast<uint64_t>(std::ceil(options.durationSeconds * options.fps))
        : maximumFrames;
    const uint64_t frameLimit = std::min(maximumFrames, requestedFrames);
    bool sawCameraTrack = false;
    bool success = true;
    uint64_t framesWritten = 0;
    std::vector<uint8_t> pixels;
    for (uint64_t frame = 0; frame < frameLimit; ++frame)
    {
        scene.Update(fixedDelta);
        Engine::Core::SceneManager::ProcessPendingSceneLoad();
        scene.PrepareRenderFrame();
        renderer.BeginFrame();
        renderer.Clear(settings.clearColor.r, settings.clearColor.g,
            settings.clearColor.b, settings.clearColor.a);
        std::unique_ptr<Engine::Graphics::IGraphicsContext> context =
            renderer.CreateFrameGraphicsContext();
        Engine::Components::Camera* camera = scene.FindGameCamera();
        if (context && camera)
            scene.Render(context.get(), static_cast<float>(options.width) /
                static_cast<float>(options.height), camera, false);
        if (!renderer.CaptureFrameRGBA(pixels) || !encoder.Write(pixels))
        {
            std::cerr << "Frame capture or FFmpeg pipe failed at frame "
                << frame << ".\n";
            success = false;
            renderer.EndFrame();
            break;
        }
        renderer.EndFrame();
        ++framesWritten;

        CameraTrack* track = camera && camera->Owner
            ? camera->Owner->GetComponent<CameraTrack>() : nullptr;
        sawCameraTrack = sawCameraTrack || track != nullptr;
        if (options.durationSeconds <= 0.f && sawCameraTrack && track &&
            track->IsFinished())
            break;
    }
    renderer.WaitIdle();
    success = encoder.Finish(true) && success;
    Engine::Core::SceneManager::SetActiveScene(nullptr);
    if (!success)
        return 9;
    std::cout << "Video export complete: " << options.output.string()
        << " (" << framesWritten << " frames)\n";
    return 0;
}
}
