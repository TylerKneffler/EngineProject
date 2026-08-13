#include "ScriptFileWatcher.h"
#include <algorithm>

namespace Engine::Editor
{
void ScriptFileWatcher::Initialize(std::string directory)
{
    m_directory = std::filesystem::path(std::move(directory));
    m_files = Scan();
}

std::unordered_map<std::string, ScriptFileWatcher::Stamp>
ScriptFileWatcher::Scan() const
{
    std::unordered_map<std::string, Stamp> files;
    std::error_code error;
    if (m_directory.empty() || !std::filesystem::exists(m_directory, error))
        return files;
    for (std::filesystem::recursive_directory_iterator iterator(m_directory, error), end;
         iterator != end && !error; iterator.increment(error))
    {
        if (!iterator->is_regular_file(error)) continue;
        std::string extension = iterator->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension != ".cpp" && extension != ".c" && extension != ".cc" &&
            extension != ".cxx" && extension != ".h" && extension != ".hpp" &&
            extension != ".inl")
            continue;
        files.emplace(iterator->path().lexically_normal().string(),
            iterator->last_write_time(error));
    }
    return files;
}

bool ScriptFileWatcher::Poll()
{
    auto current = Scan();
    if (current == m_files) return false;
    m_files = std::move(current);
    return true;
}
}
