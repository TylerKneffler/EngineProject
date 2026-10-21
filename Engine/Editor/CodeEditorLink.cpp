#include "pch.h"
#include "Engine/Editor/CodeEditorLink.h"
#include <chrono>
#include <cctype>
#include <fstream>
#include <shellapi.h>
#include <unordered_set>

namespace Engine::Editor
{
bool IsCodeFile(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    static const std::unordered_set<std::string> extensions = {
        ".h", ".hpp", ".cpp", ".cc", ".cxx", ".inl", ".cs",
        ".c", ".m", ".mm", ".py", ".lua", ".js", ".ts",
        ".sh", ".ps1"
    };
    return extensions.find(extension) != extensions.end();
}

bool OpenInCodeEditor(const std::filesystem::path& path, bool directory)
{
    const std::string utf8 = std::filesystem::absolute(path).generic_u8string();
    std::string uri = "vscode://file/";
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char byte : utf8)
    {
        if ((byte < 128 && std::isalnum(byte)) || byte == '-' || byte == '_' ||
            byte == '.' || byte == '~' || byte == '/' || byte == ':')
            uri.push_back(static_cast<char>(byte));
        else
        {
            uri.push_back('%');
            uri.push_back(hex[byte >> 4]);
            uri.push_back(hex[byte & 15]);
        }
    }
    if (directory && !uri.empty() && uri.back() != '/')
        uri.push_back('/');
    const std::wstring wideUri(uri.begin(), uri.end());
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open",
        wideUri.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

void RememberCodeFile(const std::filesystem::path& workspace,
    const std::filesystem::path& file)
{
    std::error_code error;
    if (!IsCodeFile(file) || !std::filesystem::is_regular_file(file, error))
        return;
    std::ofstream output(workspace / ".vscode" / "engine-last-code-file.txt",
        std::ios::trunc);
    if (output)
        output << std::filesystem::absolute(file).generic_u8string() << '\n';
}

std::filesystem::path LastCodeFile(const std::filesystem::path& workspace)
{
    std::ifstream input(workspace / ".vscode" / "engine-last-code-file.txt");
    std::string pathText;
    if (!std::getline(input, pathText) || pathText.empty())
        return {};
    const auto file = std::filesystem::u8path(pathText);
    std::error_code error;
    if (IsCodeFile(file) && std::filesystem::is_regular_file(file, error))
        return file;
    return {};
}

bool RequestCodeEditorAttach(const std::filesystem::path& workspace)
{
    const std::filesystem::path vscodeDirectory = workspace / ".vscode";
    if (!std::filesystem::exists(vscodeDirectory / "launch.json"))
        return false;

    const DWORD pid = GetCurrentProcessId();
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::filesystem::path request =
        vscodeDirectory / "engine-attach-request.json";
    const std::filesystem::path temporary =
        vscodeDirectory / ("engine-attach-request-" +
            std::to_string(pid) + ".tmp");
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return false;
        output << "{\"pid\":" << pid << ",\"requestedAt\":" << timestamp
            << "}\n";
        if (!output) return false;
    }
    if (MoveFileExW(temporary.c_str(), request.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
}
}
