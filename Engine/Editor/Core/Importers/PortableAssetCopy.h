#pragma once

#include <filesystem>

namespace Engine::Editor
{
// Copies an OBJ or sprite animation and rewrites its file references to a
// sibling dependency directory. Throws when a referenced file is missing.
void CopyPortableAsset(const std::filesystem::path& source,
    const std::filesystem::path& destination);
}
