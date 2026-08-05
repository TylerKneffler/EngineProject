#include "Game/Startup/RendererStartup.h"

#include "Core/ProjectLoader.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Serialization/Json.h"
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <optional>

#if defined(_MSC_VER)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace Engine::Game
{
namespace
{
constexpr int RendererButtonBase = 2000;

std::filesystem::path PreferencePath()
{
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
        static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        return std::filesystem::current_path() / "Game.renderer.json";
    executable.resize(length);
    const std::filesystem::path path(executable);
    return path.parent_path() / (path.stem().wstring() + L".renderer.json");
}

std::optional<std::string> LoadPreference(const std::filesystem::path& path)
{
    try
    {
        if (!std::filesystem::is_regular_file(path))
            return std::nullopt;
        const JsonValue root = JsonParseFile(path.string());
        if (!root.Has("always") || !root["always"].AsBool() ||
            !root.Has("renderer") || !root["renderer"].IsString())
            return std::nullopt;
        const std::string renderer = root["renderer"].AsString();
        return RendererFactory::IsRendererAvailable(renderer)
            ? std::optional<std::string>(renderer) : std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool SavePreference(const std::filesystem::path& path, const std::string& renderer)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("version", JsonValue(1));
    root.Set("renderer", JsonValue(renderer));
    root.Set("always", JsonValue(true));
    std::ofstream output(path, std::ios::trunc);
    if (!output)
        return false;
    output << JsonWrite(root);
    return output.good();
}
}

bool SelectStartupRenderer(ProjectSettings& settings)
{
    const std::filesystem::path preferencePath = PreferencePath();
    if (const std::optional<std::string> saved = LoadPreference(preferencePath))
    {
        settings.gameRenderingAPI = *saved;
        return true;
    }

    std::vector<std::string> available;
    std::vector<std::wstring> buttonLabels;
    std::wstring unavailableDetails;
    for (const RendererOption& option : RendererFactory::GetRendererOptions())
    {
        if (option.available)
        {
            available.push_back(option.name);
            buttonLabels.emplace_back(option.name.begin(), option.name.end());
        }
        else
        {
            unavailableDetails += std::wstring(option.name.begin(), option.name.end()) +
                L": " + std::wstring(option.unavailableReason.begin(),
                    option.unavailableReason.end()) + L"\n";
        }
    }
    if (available.empty())
    {
        MessageBoxW(nullptr, L"No supported graphics renderer is available.",
            L"Renderer Selection", MB_OK | MB_ICONERROR);
        return false;
    }

    std::vector<TASKDIALOG_BUTTON> buttons;
    buttons.reserve(available.size());
    int defaultButton = RendererButtonBase;
    for (std::size_t index = 0; index < available.size(); ++index)
    {
        const int id = RendererButtonBase + static_cast<int>(index);
        buttons.push_back({ id, buttonLabels[index].c_str() });
        if (available[index] == settings.gameRenderingAPI)
            defaultButton = id;
    }

    const std::wstring title = settings.name.empty()
        ? L"Renderer Selection"
        : std::wstring(settings.name.begin(), settings.name.end()) + L" - Renderer";
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.dwFlags = TDF_USE_COMMAND_LINKS | TDF_SIZE_TO_CONTENT |
        TDF_ALLOW_DIALOG_CANCELLATION;
    dialog.pszWindowTitle = title.c_str();
    dialog.pszMainInstruction = L"Choose a graphics renderer";
    dialog.pszContent = L"Select the renderer to use for this game session.";
    dialog.cButtons = static_cast<UINT>(buttons.size());
    dialog.pButtons = buttons.data();
    dialog.nDefaultButton = defaultButton;
    dialog.pszVerificationText = L"Always use this renderer";
    if (!unavailableDetails.empty())
    {
        dialog.pszExpandedInformation = unavailableDetails.c_str();
        dialog.pszExpandedControlText = L"Show unavailable renderers";
        dialog.pszCollapsedControlText = L"Hide unavailable renderers";
    }

    int selectedButton = 0;
    BOOL always = FALSE;
    const HRESULT result = TaskDialogIndirect(
        &dialog, &selectedButton, nullptr, &always);
    if (FAILED(result))
    {
        MessageBoxW(nullptr, L"The renderer selection dialog could not be displayed.",
            L"Renderer Selection", MB_OK | MB_ICONERROR);
        return false;
    }
    const int selectedIndex = selectedButton - RendererButtonBase;
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(available.size()))
        return false;

    settings.gameRenderingAPI = available[static_cast<std::size_t>(selectedIndex)];
    if (always && !SavePreference(preferencePath, settings.gameRenderingAPI))
    {
        const std::wstring message = L"The renderer was selected, but the preference "
            L"could not be saved to:\n\n" + preferencePath.wstring();
        MessageBoxW(nullptr, message.c_str(), L"Renderer Preference",
            MB_OK | MB_ICONWARNING);
    }
    return true;
}
}
