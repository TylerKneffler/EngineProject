#pragma once

#include <memory>
#include <string>

// Shared identity for a prefab resource. Scene objects remain independently
// owned instances; instances created from the same prefab share this handle.
class PrefabAsset
{
public:
    static std::shared_ptr<const PrefabAsset> Acquire(const std::string& path);

    const std::string& GetPath() const { return m_path; }

private:
    explicit PrefabAsset(std::string path) : m_path(std::move(path)) {}
    std::string m_path;
};
