#include "pch.h"
#include "Engine/Editor/CodeEditorLink.h"
#include <chrono>
#include <cctype>
#include <fstream>
#include <shellapi.h>
#include <unordered_set>

namespace Engine::Editor
{
namespace
{
std::filesystem::path EnvironmentPath(const wchar_t* name)
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return buffer;
}

std::filesystem::path FindOnPath(const wchar_t* name)
{
    const auto searchPath = EnvironmentPath(L"PATH");
    if (searchPath.empty())
        return {};
    std::wstring buffer(32768, L'\0');
    const DWORD length = SearchPathW(searchPath.c_str(), name, nullptr,
        static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return buffer;
}

std::filesystem::path FindInstalledEditor(const wchar_t* executable,
    const wchar_t* directory, const wchar_t* cli = nullptr)
{
    if (cli)
    {
        const auto cliPath = FindOnPath(cli);
        if (!cliPath.empty())
        {
            const auto sibling = cliPath.parent_path().parent_path() / executable;
            std::error_code error;
            if (std::filesystem::is_regular_file(sibling, error))
                return sibling;
        }
    }
    if (const auto onPath = FindOnPath(executable); !onPath.empty())
        return onPath;
    for (const wchar_t* variable : { L"LOCALAPPDATA", L"ProgramFiles",
        L"ProgramFiles(x86)" })
    {
        const auto root = EnvironmentPath(variable);
        if (root.empty()) continue;
        const auto candidate = (std::wstring(variable) == L"LOCALAPPDATA"
            ? root / L"Programs" : root) / directory / executable;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error))
            return candidate;
    }
    return {};
}
}

std::vector<DetectedCodeEditor> DetectCodeEditors()
{
    std::vector<DetectedCodeEditor> editors;
    const auto add = [&](const char* id, const char* name,
        const std::filesystem::path& executable)
    {
        if (!executable.empty())
            editors.push_back({ id, name, executable });
    };
    add("VSCode", "Visual Studio Code", FindInstalledEditor(L"Code.exe",
        L"Microsoft VS Code", L"code.cmd"));
    add("VSCodeInsiders", "Visual Studio Code Insiders", FindInstalledEditor(
        L"Code - Insiders.exe", L"Microsoft VS Code Insiders",
        L"code-insiders.cmd"));
    add("SublimeText", "Sublime Text", FindInstalledEditor(
        L"sublime_text.exe", L"Sublime Text"));
    add("NotepadPlusPlus", "Notepad++", FindInstalledEditor(
        L"notepad++.exe", L"Notepad++"));
    return editors;
}

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

bool OpenInCodeEditor(const std::filesystem::path& path,
    const std::string& editorId, bool directory)
{
    if (editorId == "None")
        return false;
    const auto editors = DetectCodeEditors();
    const auto selected = std::find_if(editors.begin(), editors.end(),
        [&](const DetectedCodeEditor& editor) { return editor.id == editorId; });
    if (selected == editors.end())
        return false;

    std::wstring arguments;
    if (!(directory && editorId == "NotepadPlusPlus"))
    {
        if (editorId == "VSCode" || editorId == "VSCodeInsiders")
            arguments = L"--reuse-window ";
        arguments += L"\"" + std::filesystem::absolute(path).wstring() + L"\"";
    }
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open",
        selected->executable.c_str(),
        arguments.empty() ? nullptr : arguments.c_str(), nullptr,
        SW_SHOWNORMAL)) > 32;
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
