#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Engine::Editor
{
struct DetectedCodeEditor
{
    std::string id;
    std::string name;
    std::filesystem::path executable;
};

std::vector<DetectedCodeEditor> DetectCodeEditors();
// Opens a file or workspace folder in the selected installed editor.
bool IsCodeFile(const std::filesystem::path& path);
bool OpenInCodeEditor(const std::filesystem::path& path,
    const std::string& editorId, bool directory = false);
void RememberCodeFile(const std::filesystem::path& workspace,
    const std::filesystem::path& file);
std::filesystem::path LastCodeFile(const std::filesystem::path& workspace);
bool RequestCodeEditorAttach(const std::filesystem::path& workspace);
}
