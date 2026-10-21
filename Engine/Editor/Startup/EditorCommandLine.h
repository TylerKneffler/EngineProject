#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Engine::Editor
{
    class EditorCommandLine
    {
    public:
        EditorCommandLine();

        // Returns an exit code only when an import command was requested.
        std::optional<int> RunImportCommand() const;
        std::string ResolveProject() const;
        bool HasArgument(const wchar_t* argument) const;
        std::string RendererOverride() const;

    private:
        std::vector<std::wstring> arguments_;
    };
}
