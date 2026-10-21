#pragma once

#include <filesystem>

namespace Engine::Editor
{
// Opens a file or workspace folder through VS Code's registered URI handler.
bool IsCodeFile(const std::filesystem::path& path);
bool OpenInCodeEditor(const std::filesystem::path& path, bool directory = false);
void RememberCodeFile(const std::filesystem::path& workspace,
    const std::filesystem::path& file);
std::filesystem::path LastCodeFile(const std::filesystem::path& workspace);
bool RequestCodeEditorAttach(const std::filesystem::path& workspace);
}
