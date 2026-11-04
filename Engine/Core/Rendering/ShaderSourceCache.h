#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering
{
// Scene-owner cache. Poll once per preparation, then resolve without filesystem
// work until an authored path or a watched directory changes.
class ShaderSourceCache
{
public:
    struct Source
    {
        std::string path;
        int64_t revision=0;
        bool exists=false, hlsl=false;
    };
    using SearchPaths=std::vector<std::filesystem::path>;
    ShaderSourceCache();
    ~ShaderSourceCache();
    ShaderSourceCache(const ShaderSourceCache&)=delete;
    ShaderSourceCache& operator=(const ShaderSourceCache&)=delete;
    void Poll();
    const Source& Resolve(const std::string& location, const std::string& requested,
        const std::function<SearchPaths()>& searchPaths);
    // Also used for the built-in source selected by an empty overlay path.
    const Source& Resolve(const std::string& path);
    struct Statistics { uint64_t resolutions=0, metadataChecks=0, notifications=0; };
    Statistics GetStatistics() const;
private:
    struct State;
    std::unique_ptr<State> m_state;
};
}
