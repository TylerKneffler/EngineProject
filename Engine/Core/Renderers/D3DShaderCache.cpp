#include "pch.h"
#include "Core/Renderers/D3DShaderCache.h"
#include "Core/Renderers/HlslFileInclude.h"
#include "Core/Graphics/ShaderCompileProgress.h"

#include <dxcapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <set>
#include <mutex>
#include <vector>

namespace Engine::Renderers
{
namespace
{
namespace fs = std::filesystem;

bool CacheEnabled()
{
    static const bool enabled = [] {
        char value[8]{};
        return GetEnvironmentVariableA("ENGINE_SHADER_CACHE", value, sizeof(value)) == 0 ||
            value[0] != '0';
    }();
    return enabled;
}

const fs::path& CacheDirectory()
{
    static const fs::path directory = [] {
        char value[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableA("ENGINE_SHADER_CACHE_DIR", value, MAX_PATH);
        if (length > 0 && length < MAX_PATH) return fs::path(value);
        std::error_code error;
        const auto temp = fs::temp_directory_path(error);
        return error ? fs::path() : temp / "EngineShaderCache";
    }();
    return directory;
}

// requests.txt: one request per line, tab separated (flags as hex, target,
// entry point, working directory, path), each listed once.
std::string RequestLine(const std::string& directory, const std::string& filePath,
    const char* entryPoint, const char* target, UINT flags)
{
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%x", flags);
    return std::string(hex) + '\t' + (target ? target : "") + '\t' + (entryPoint ? entryPoint : "") +
        '\t' + directory + '\t' + filePath;
}

std::atomic<bool> g_recordRequests{true};

void RecordRequest(const std::string& filePath, const char* entryPoint, const char* target, UINT flags)
{
    if (!g_recordRequests) return;
    static std::mutex mutex;
    static std::set<std::string> listed;
    static bool loaded = false;
    std::error_code error;
    const std::string directory = fs::current_path(error).string();
    const std::string line = RequestLine(directory, filePath, entryPoint, target, flags);
    std::lock_guard lock(mutex);
    const fs::path manifest = CacheDirectory() / "requests.txt";
    if (!loaded)
    {
        loaded = true;
        std::ifstream in(manifest);
        for (std::string existing; std::getline(in, existing);)
            if (!existing.empty()) listed.insert(existing);
    }
    if (!listed.insert(line).second) return;
    fs::create_directories(CacheDirectory(), error);
    std::ofstream(manifest, std::ios::app) << line << '\n';
}

// Two independent 64-bit FNV-1a streams form a 128-bit key.
struct Hasher
{
    uint64_t a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
    void Add(const void* data, size_t size)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            a = (a ^ bytes[i]) * 0x100000001b3ull;
            b = (b ^ bytes[i]) * 0x00000100000001b3ull + 0x9e3779b97f4a7c15ull;
        }
    }
    void Add(const std::string& text) { Add(text.data(), text.size()); Add("\0", 1); }
    std::string Hex() const
    {
        char text[33];
        std::snprintf(text, sizeof(text), "%016llx%016llx",
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
        return text;
    }
};

std::wstring Wide(const std::string& text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    wide.pop_back();
    return wide;
}

// DXC (Shader Model 6, DXIL), loaded at run time: dxcompiler.dll next to the
// executable, else the newest Windows SDK's (its dxil.dll, which signs the
// output, is then found through SetDllDirectory). Null when unavailable.
DxcCreateInstanceProc DxcFactory()
{
    static const DxcCreateInstanceProc proc = []() -> DxcCreateInstanceProc {
        HMODULE module = LoadLibraryW(L"dxcompiler.dll");
        if (!module)
        {
            std::error_code error;
            fs::path best;
            const fs::path kits = L"C:\\Program Files (x86)\\Windows Kits\\10\\bin";
            for (const auto& entry : fs::directory_iterator(kits, error))
            {
                const fs::path candidate = entry.path() / L"x64" / L"dxcompiler.dll";
                if (fs::exists(candidate, error) && fs::exists(entry.path() / L"x64" / L"dxil.dll", error) &&
                    (best.empty() || entry.path().filename() > best.parent_path().parent_path().filename()))
                    best = candidate;
            }
            if (!best.empty())
            {
                SetDllDirectoryW(best.parent_path().c_str());
                module = LoadLibraryW(best.c_str());
            }
        }
        return module ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance")) : nullptr;
    }();
    return proc;
}

bool IsDxcTarget(const char* target)
{
    return target && std::strlen(target) >= 4 && target[3] == '6';
}

// One DXC compile (instances per call: DXC objects are not thread-safe).
HRESULT CompileWithDxc(const std::wstring& widePath, const char* entryPoint, const char* target,
    UINT flags, Microsoft::WRL::ComPtr<ID3DBlob>& bytecode, std::string& error)
{
    const auto factory = DxcFactory();
    if (!factory) { error = "dxcompiler.dll not found"; return E_FAIL; }
    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    if (FAILED(factory(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
        FAILED(factory(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
    { error = "DXC could not be created"; return E_FAIL; }
    Microsoft::WRL::ComPtr<IDxcIncludeHandler> includes;
    utils->CreateDefaultIncludeHandler(&includes);
    Microsoft::WRL::ComPtr<IDxcBlobEncoding> source;
    if (FAILED(utils->LoadFile(widePath.c_str(), nullptr, &source)))
    { error = "Shader source not readable"; return E_FAIL; }
    const std::wstring entry = Wide(entryPoint ? entryPoint : "main");
    const std::wstring profile = Wide(target);
    const std::wstring directory = fs::path(widePath).parent_path().wstring();
    std::vector<LPCWSTR> arguments = {widePath.c_str(), L"-E", entry.c_str(), L"-T", profile.c_str(),
        L"-I", directory.c_str(), L"-Qstrip_debug", L"-Qstrip_reflect"};
    arguments.push_back((flags & D3DCOMPILE_SKIP_OPTIMIZATION) ? L"-Od" : L"-O3");
    DxcBuffer buffer{source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_ACP};
    Microsoft::WRL::ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(&buffer, arguments.data(), static_cast<UINT32>(arguments.size()),
        includes.Get(), IID_PPV_ARGS(&result))) || !result)
    { error = "DXC compile call failed"; return E_FAIL; }
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    Microsoft::WRL::ComPtr<IDxcBlobUtf8> messages;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&messages), nullptr);
    if (FAILED(status))
    {
        error = messages && messages->GetStringLength() ? messages->GetStringPointer() : "DXC compile failed";
        return status;
    }
    Microsoft::WRL::ComPtr<IDxcBlob> object;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
    if (!object || FAILED(D3DCreateBlob(object->GetBufferSize(), &bytecode)))
    { error = "DXC produced no object"; return E_FAIL; }
    std::memcpy(bytecode->GetBufferPointer(), object->GetBufferPointer(), object->GetBufferSize());
    return S_OK;
}

// Key from the preprocessed source; empty when preprocessing fails (the
// compile then runs uncached and reports the real error).
std::string CacheKey(const std::string& filePath, const char* entryPoint,
    const char* target, UINT flags)
{
    std::ifstream stream(filePath, std::ios::binary);
    if (!stream) return {};
    const std::string source((std::istreambuf_iterator<char>(stream)), {});
    HlslFileInclude includes(filePath);
    Microsoft::WRL::ComPtr<ID3DBlob> preprocessed, errors;
    if (FAILED(D3DPreprocess(source.data(), source.size(), filePath.c_str(),
        nullptr, &includes, &preprocessed, &errors)) || !preprocessed)
        return {};
    Hasher hasher;
    hasher.Add(IsDxcTarget(target) ? "dxil-v1" : "d3d-bytecode-v1");
    hasher.Add(std::to_string(D3D_COMPILER_VERSION));
    hasher.Add(entryPoint ? entryPoint : "");
    hasher.Add(target ? target : "");
    hasher.Add(&flags, sizeof(flags));
    hasher.Add(preprocessed->GetBufferPointer(), preprocessed->GetBufferSize());
    return hasher.Hex();
}

Microsoft::WRL::ComPtr<ID3DBlob> Load(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return nullptr;
    const auto size = stream.tellg();
    if (size <= 0) return nullptr;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    if (FAILED(D3DCreateBlob(static_cast<SIZE_T>(size), &blob))) return nullptr;
    stream.seekg(0);
    if (!stream.read(static_cast<char*>(blob->GetBufferPointer()), size)) return nullptr;
    return blob;
}

void Store(const fs::path& path, ID3DBlob* blob)
{
    // Write a unique temporary file, then rename: concurrent compiles of the
    // same shader on other workers or processes never see a partial file.
    static std::atomic<uint64_t> sequence{ 0 };
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ostringstream suffix;
    suffix << ".tmp" << GetCurrentProcessId() << '_' << std::this_thread::get_id()
        << '_' << sequence++;
    const fs::path temporary = path.string() + suffix.str();
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return;
        stream.write(static_cast<const char*>(blob->GetBufferPointer()),
            static_cast<std::streamsize>(blob->GetBufferSize()));
        if (!stream) { stream.close(); fs::remove(temporary, error); return; }
    }
    fs::rename(temporary, path, error);
    if (error) fs::remove(temporary, error);
}
}

Microsoft::WRL::ComPtr<ID3DBlob> D3DShaderCache::CompileFromFile(
    const std::string& filePath, const char* entryPoint, const char* target,
    UINT flags, std::string& error)
{
    // The first request warms every recorded one in parallel (see header).
    // An atomic, not call_once: the warm's own workers come back through
    // here and must pass straight on.
    static std::atomic<bool> warmStarted{false};
    if (g_recordRequests && !warmStarted.exchange(true))
        WarmRecordedRequests();
    error.clear();
    if (CacheEnabled() && !CacheDirectory().empty())
        RecordRequest(filePath, entryPoint, target, flags);
    const std::wstring widePath = Wide(filePath);
    if (widePath.empty())
    {
        error = "Invalid shader path";
        return nullptr;
    }

    fs::path cached;
    if (CacheEnabled() && !CacheDirectory().empty())
    {
        const std::string key = CacheKey(filePath, entryPoint, target, flags);
        if (!key.empty())
        {
            cached = CacheDirectory() / (key + ".dxbc");
            if (auto blob = Load(cached))
                return blob;
        }
    }

    Microsoft::WRL::ComPtr<ID3DBlob> bytecode, errors;
    HlslFileInclude includes(filePath);
    // ENGINE_SHADER_COMPILE_LOG=1: every compile that missed the cache, how
    // long it took and how it ended (stderr: unbuffered, survives a kill).
    static const bool logCompiles = [] {
        const char* value = std::getenv("ENGINE_SHADER_COMPILE_LOG");
        return value && value[0] == '1';
    }();
    const auto compileStart = std::chrono::steady_clock::now();
    // Loading screens show the compile (StartupSplash).
    static const bool progressStore = [] {
        Engine::Graphics::ShaderCompileProgress::SetStoreDirectory(
            CacheEnabled() ? CacheDirectory().string() : std::string());
        return true;
    }();
    (void)progressStore;
    const int progressTicket = Engine::Graphics::ShaderCompileProgress::Begin(filePath, entryPoint, target);
    if (logCompiles)
        std::fprintf(stderr, "[ShaderCompile] start %s %s %s flags 0x%x cached-as %s\n", filePath.c_str(),
            entryPoint ? entryPoint : "", target ? target : "", flags, cached.string().c_str());
    std::string dxcError;
    const HRESULT hr = IsDxcTarget(target)
        ? CompileWithDxc(widePath, entryPoint, target, flags, bytecode, dxcError)
        : D3DCompileFromFile(widePath.c_str(), nullptr, &includes,
            entryPoint, target, flags, 0, &bytecode, &errors);
    Engine::Graphics::ShaderCompileProgress::End(progressTicket, SUCCEEDED(hr));
    if (logCompiles)
        std::fprintf(stderr, "[ShaderCompile] %s %s %s after %.1f s\n", FAILED(hr) ? "FAILED" : "done",
            filePath.c_str(), entryPoint ? entryPoint : "",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - compileStart).count());
    if (FAILED(hr))
    {
        if (!dxcError.empty())
            error = dxcError;
        else if (errors)
            error.assign(static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        else
        {
            std::ostringstream out;
            out << "D3DCompileFromFile failed with HRESULT 0x" << std::hex << hr;
            error = out.str();
        }
        return nullptr;
    }
    if (!cached.empty())
        Store(cached, bytecode.Get());
    return bytecode;
}

void D3DShaderCache::SetRecordRequests(bool record) { g_recordRequests = record; }

std::vector<D3DShaderCache::Request> D3DShaderCache::RecordedRequests()
{
    std::vector<Request> requests;
    std::set<std::string> seen;
    std::ifstream in(CacheDirectory() / "requests.txt");
    for (std::string line; std::getline(in, line);)
    {
        std::vector<std::string> fields;
        size_t start = 0;
        for (size_t tab; (tab = line.find('\t', start)) != std::string::npos; start = tab + 1)
            fields.push_back(line.substr(start, tab - start));
        fields.push_back(line.substr(start));
        if (fields.size() != 5) continue;
        Request request;
        request.flags = static_cast<UINT>(std::strtoul(fields[0].c_str(), nullptr, 16));
        request.target = fields[1];
        request.entryPoint = fields[2];
        request.directory = fields[3];
        request.filePath = fields[4];
        // One per file, entry point, target and flags (the first working
        // directory listed: the same folder can be listed in other case).
        if (!seen.insert(fields[0] + '\t' + fields[1] + '\t' + fields[2] + '\t' + fields[4]).second) continue;
        requests.push_back(std::move(request));
    }
    return requests;
}

std::string D3DShaderCache::CacheDirectoryPath() { return CacheDirectory().string(); }

bool D3DShaderCache::DxcAvailable() { return DxcFactory() != nullptr; }

namespace { std::atomic<bool> g_preferModel6{false}; }
void D3DShaderCache::PreferShaderModel6() { g_preferModel6 = true; }

void D3DShaderCache::WarmRecordedRequests()
{
    static const bool enabled = [] {
        const char* value = std::getenv("ENGINE_SHADER_WARM");
        return !(value && value[0] == '0');
    }();
    if (!enabled || !CacheEnabled() || CacheDirectory().empty()) return;
    std::error_code pathError;
    const std::string directory = fs::current_path(pathError).string();
    if (pathError) return;
    std::vector<Request> requests;
    std::set<std::string> listed;
    for (auto& request : RecordedRequests())
    {
        std::error_code error;
        if (!fs::equivalent(request.directory, directory, error) || error ||
            !fs::exists(request.filePath, error))
            continue;
        if (g_preferModel6 && request.target.size() == 6 && request.target.compare(2, 4, "_5_0") == 0)
            request.target = request.target.substr(0, 2) + "_6_0";
        if (listed.insert(request.filePath + '	' + request.entryPoint + '	' + request.target).second)
            requests.push_back(std::move(request));
    }
    if (requests.empty()) return;
    const auto start = std::chrono::steady_clock::now();
    std::atomic<size_t> next{0};
    std::atomic<int> compiled{0};
    const unsigned workers = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(),
        static_cast<unsigned>(requests.size())));
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < workers; ++t)
        threads.emplace_back([&] {
            for (size_t i; (i = next++) < requests.size();)
            {
                const auto& request = requests[i];
                const auto begin = std::chrono::steady_clock::now();
                std::string message;
                // Recorded already; a failure is reported by the real request.
                CompileFromFile(request.filePath, request.entryPoint.c_str(),
                    request.target.c_str(), request.flags, message);
                if (std::chrono::steady_clock::now() - begin > std::chrono::seconds(1)) ++compiled;
            }
        });
    for (auto& thread : threads) thread.join();
    if (compiled)
        std::fprintf(stderr, "[ShaderCache] warmed %zu recorded shaders (%d compiled) in %.1f s on %u threads\n",
            requests.size(), compiled.load(),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), workers);
}
}
