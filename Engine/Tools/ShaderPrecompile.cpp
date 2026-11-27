// Compiles the shaders the engine has asked for into the shader cache ahead
// of the next launch, so a shader edit costs build time rather than a stall
// at startup (Object.hlsl takes minutes per variant at full optimization).
//
// The requests come from the cache's requests.txt, which the engine writes
// as it compiles (D3DShaderCache): each with the working directory, path,
// entry point, target and flags it used, since the cache key includes the
// preprocessed text and that names files as the request did. Requests whose
// file is gone are skipped; ones already cached cost a preprocess. They run
// in parallel. Runs after Editor and Game build (CMakeLists.txt);
// ENGINE_SHADER_PRECOMPILE=0 skips it.
//
// Usage: ShaderPrecompile [--list]

#include "Core/Renderers/D3DShaderCache.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using Engine::Renderers::D3DShaderCache;

int main(int argc, char** argv)
{
    char skip[8]{};
    if (GetEnvironmentVariableA("ENGINE_SHADER_PRECOMPILE", skip, sizeof(skip)) > 0 && skip[0] == '0')
        return 0;
    const bool list = argc > 1 && std::strcmp(argv[1], "--list") == 0;
    D3DShaderCache::SetRecordRequests(false);
    auto requests = D3DShaderCache::RecordedRequests();
    // Paths are resolved against each request's working directory.
    std::vector<D3DShaderCache::Request> present;
    for (auto& request : requests)
    {
        std::error_code error;
        const std::filesystem::path path = std::filesystem::path(request.filePath).is_absolute()
            ? std::filesystem::path(request.filePath)
            : std::filesystem::path(request.directory) / request.filePath;
        if (std::filesystem::exists(path, error)) present.push_back(std::move(request));
    }
    // DX12 compiles shader model 6 with DXC (D3DShaderCache::DxcAvailable):
    // precompile the 6.0 twin of every recorded 5.0 request, as the engine's
    // own warm-up does, instead of the fxc 5.0 variants (minutes each for
    // Object.hlsl). ENGINE_SHADER_DXC=0 keeps the recorded targets (DX11).
    char dxcSetting[8]{};
    const bool dxcOff = GetEnvironmentVariableA("ENGINE_SHADER_DXC", dxcSetting, sizeof(dxcSetting)) > 0 &&
        dxcSetting[0] == '0';
    if (!dxcOff && D3DShaderCache::DxcAvailable())
    {
        std::vector<D3DShaderCache::Request> twins;
        std::set<std::string> listed;
        for (auto& request : present)
        {
            if (request.target.size() == 6 && request.target.compare(2, 4, "_5_0") == 0)
                request.target = request.target.substr(0, 2) + "_6_0";
            if (listed.insert(request.directory + '|' + request.filePath + '|' + request.entryPoint + '|' +
                request.target).second)
                twins.push_back(std::move(request));
        }
        present = std::move(twins);
    }
    if (list)
    {
        for (const auto& r : present)
            std::printf("%s %s %s (in %s) flags 0x%x\n", r.filePath.c_str(), r.entryPoint.c_str(),
                r.target.c_str(), r.directory.c_str(), r.flags);
        return 0;
    }
    if (present.empty())
    {
        std::printf("ShaderPrecompile: no recorded shader requests in %s (run the engine once)\n",
            D3DShaderCache::CacheDirectoryPath().c_str());
        return 0;
    }
    // The working directory is process-wide: group by it, and compile each
    // group's requests in parallel from there.
    std::map<std::string, std::vector<const D3DShaderCache::Request*>> byDirectory;
    for (const auto& request : present) byDirectory[request.directory].push_back(&request);
    const auto start = std::chrono::steady_clock::now();
    std::atomic<int> failed{0}, done{0};
    std::mutex print;
    const unsigned workers = std::max(1u, std::thread::hardware_concurrency());
    for (const auto& [directory, group] : byDirectory)
    {
        std::error_code error;
        std::filesystem::current_path(directory, error);
        if (error) continue;
        std::atomic<size_t> next{0};
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < std::min<size_t>(workers, group.size()); ++t)
            threads.emplace_back([&] {
                for (size_t i; (i = next++) < group.size();)
                {
                    const auto& r = *group[i];
                    const auto begin = std::chrono::steady_clock::now();
                    std::string message;
                    const bool ok = static_cast<bool>(D3DShaderCache::CompileFromFile(
                        r.filePath, r.entryPoint.c_str(), r.target.c_str(), r.flags, message));
                    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
                    ++done;
                    if (!ok) ++failed;
                    // Only real compiles (or failures) are worth a line.
                    if (!ok || seconds > 1.0)
                    {
                        std::lock_guard lock(print);
                        std::printf("ShaderPrecompile: %s %s %s %s (%.1f s)\n", ok ? "compiled" : "FAILED",
                            r.filePath.c_str(), r.entryPoint.c_str(), r.target.c_str(), seconds);
                        if (!ok) std::printf("%s\n", message.c_str());
                    }
                }
            });
        for (auto& thread : threads) thread.join();
    }
    std::printf("ShaderPrecompile: %d shader variants ready in %.1f s%s\n", done.load(),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
        failed ? " (some failed: the engine reports them at launch)" : "");
    // A failed shader is the engine's to report; never fail the build here.
    return 0;
}
