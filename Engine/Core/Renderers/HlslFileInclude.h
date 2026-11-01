#pragma once
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Engine::Renderers
{
// Resolve every nested include beside the file that included it. The standard
// D3D handler otherwise resolves asset -> engine -> header chains incorrectly.
class HlslFileInclude final : public ID3DInclude
{
public:
    explicit HlslFileInclude(const std::string& source) : m_source(source) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID parent,
        LPCVOID* data, UINT* size) override
    {
        try
        {
            const auto found = m_files.find(parent);
            const auto directory = (found == m_files.end()
                ? m_source : found->second.path).parent_path();
            File file;
            file.path = (directory / std::filesystem::path(name)).lexically_normal();
            std::ifstream stream(file.path, std::ios::binary | std::ios::ate);
            if (!stream) return E_FAIL;
            const auto length = stream.tellg();
            if (length < 0 || static_cast<uint64_t>(length) > (std::numeric_limits<UINT>::max)())
                return E_FAIL;
            file.bytes.resize(length > 0 ? static_cast<size_t>(length) : 1u);
            stream.seekg(0);
            if (length > 0 && !stream.read(file.bytes.data(), length)) return E_FAIL;
            *data = file.bytes.data();
            *size = static_cast<UINT>(length);
            m_files.emplace(*data, std::move(file));
            return S_OK;
        }
        catch (...) { return E_FAIL; }
    }

    HRESULT __stdcall Close(LPCVOID data) override
    {
        m_files.erase(data);
        return S_OK;
    }

private:
    struct File { std::filesystem::path path; std::vector<char> bytes; };
    std::filesystem::path m_source;
    std::unordered_map<const void*, File> m_files;
};
}
