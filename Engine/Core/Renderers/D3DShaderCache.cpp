#include <chrono>
#include "pch.h"
#include "Core/Renderers/D3DShaderCache.h"
#include "Core/Renderers/HlslFileInclude.h"
#include "Core/Graphics/ShaderCompileProgress.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
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
    hasher.Add("d3d-bytecode-v1");
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
    const HRESULT hr = D3DCompileFromFile(widePath.c_str(), nullptr, &includes,
        entryPoint, target, flags, 0, &bytecode, &errors);
    Engine::Graphics::ShaderCompileProgress::End(progressTicket, SUCCEEDED(hr));
    if (logCompiles)
        std::fprintf(stderr, "[ShaderCompile] %s %s %s after %.1f s\n", FAILED(hr) ? "FAILED" : "done",
            filePath.c_str(), entryPoint ? entryPoint : "",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - compileStart).count());
    if (FAILED(hr))
    {
        if (errors)
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
}
