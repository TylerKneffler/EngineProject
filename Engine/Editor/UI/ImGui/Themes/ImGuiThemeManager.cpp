#include "pch.h"
#include "Engine/Editor/UI/ImGui/Themes/ImGuiThemeManager.h"
#include "imgui.h"
#include <pugixml.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <set>
#include <unordered_map>

#ifndef ENGINE_ROOT_PATH
#define ENGINE_ROOT_PATH "."
#endif

namespace Engine::Editor
{
namespace
{
namespace fs = std::filesystem;

struct ThemeFile
{
    std::string name;
    fs::path path;
};

std::vector<ThemeFile> g_themes;
std::vector<std::string> g_themeNames;

bool ReadFloat(const pugi::xml_attribute& attribute, float minimum, float maximum,
    float& value)
{
    if (!attribute) return false;
    const auto parsed = attribute.as_float(std::numeric_limits<float>::quiet_NaN());
    if (!std::isfinite(parsed) || parsed < minimum || parsed > maximum) return false;
    value = parsed;
    return true;
}

int FindColor(const char* name)
{
    for (int color = 0; color < ImGuiCol_COUNT; ++color)
        if (std::strcmp(ImGui::GetStyleColorName(color), name) == 0)
            return color;
    return -1;
}

bool ParseTheme(const fs::path& path, std::string* name, ImGuiStyle* output,
    std::string& error)
{
    pugi::xml_document document;
    const auto result = document.load_file(path.c_str());
    if (!result)
    {
        error = "invalid XML: " + std::string(result.description());
        return false;
    }

    const auto root = document.child("ImGuiTheme");
    if (!root || root.attribute("version").as_int(-1) != 1)
    {
        error = "missing <ImGuiTheme version=\"1\"> root";
        return false;
    }
    const std::string themeName = root.attribute("name").as_string();
    if (themeName.empty())
    {
        error = "theme name is empty";
        return false;
    }
    for (const auto attribute : root.attributes())
        if (std::strcmp(attribute.name(), "name") != 0 &&
            std::strcmp(attribute.name(), "version") != 0)
        {
            error = "unknown ImGuiTheme attribute: " + std::string(attribute.name());
            return false;
        }

    const auto styleNode = root.child("Style");
    const auto colorsNode = root.child("Colors");
    if (!styleNode || !colorsNode)
    {
        error = "theme must contain Style and Colors elements";
        return false;
    }

    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    const std::unordered_map<std::string, std::pair<float*, float>> styleValues = {
        {"Alpha", {&style.Alpha, 1.f}},
        {"DisabledAlpha", {&style.DisabledAlpha, 1.f}},
        {"WindowPaddingX", {&style.WindowPadding.x, 64.f}},
        {"WindowPaddingY", {&style.WindowPadding.y, 64.f}},
        {"WindowRounding", {&style.WindowRounding, 32.f}},
        {"WindowBorderSize", {&style.WindowBorderSize, 8.f}},
        {"WindowBorderHoverPadding", {&style.WindowBorderHoverPadding, 16.f}},
        {"ChildRounding", {&style.ChildRounding, 32.f}},
        {"ChildBorderSize", {&style.ChildBorderSize, 8.f}},
        {"PopupRounding", {&style.PopupRounding, 32.f}},
        {"PopupBorderSize", {&style.PopupBorderSize, 8.f}},
        {"FramePaddingX", {&style.FramePadding.x, 64.f}},
        {"FramePaddingY", {&style.FramePadding.y, 64.f}},
        {"FrameRounding", {&style.FrameRounding, 32.f}},
        {"FrameBorderSize", {&style.FrameBorderSize, 8.f}},
        {"ItemSpacingX", {&style.ItemSpacing.x, 64.f}},
        {"ItemSpacingY", {&style.ItemSpacing.y, 64.f}},
        {"ItemInnerSpacingX", {&style.ItemInnerSpacing.x, 64.f}},
        {"ItemInnerSpacingY", {&style.ItemInnerSpacing.y, 64.f}},
        {"CellPaddingX", {&style.CellPadding.x, 64.f}},
        {"CellPaddingY", {&style.CellPadding.y, 64.f}},
        {"IndentSpacing", {&style.IndentSpacing, 64.f}},
        {"ScrollbarSize", {&style.ScrollbarSize, 64.f}},
        {"ScrollbarRounding", {&style.ScrollbarRounding, 32.f}},
        {"ScrollbarPadding", {&style.ScrollbarPadding, 16.f}},
        {"GrabMinSize", {&style.GrabMinSize, 64.f}},
        {"GrabRounding", {&style.GrabRounding, 32.f}},
        {"ImageRounding", {&style.ImageRounding, 32.f}},
        {"ImageBorderSize", {&style.ImageBorderSize, 8.f}},
        {"TabRounding", {&style.TabRounding, 32.f}},
        {"TabBorderSize", {&style.TabBorderSize, 8.f}},
        {"TabBarBorderSize", {&style.TabBarBorderSize, 8.f}},
        {"TabBarOverlineSize", {&style.TabBarOverlineSize, 8.f}},
        {"TreeLinesSize", {&style.TreeLinesSize, 8.f}},
        {"TreeLinesRounding", {&style.TreeLinesRounding, 16.f}},
        {"MenuItemRounding", {&style.MenuItemRounding, 16.f}},
        {"DragDropTargetRounding", {&style.DragDropTargetRounding, 32.f}},
        {"DragDropTargetBorderSize", {&style.DragDropTargetBorderSize, 8.f}},
        {"DragDropTargetPadding", {&style.DragDropTargetPadding, 16.f}},
        {"SeparatorSize", {&style.SeparatorSize, 8.f}},
        {"DockingSeparatorSize", {&style.DockingSeparatorSize, 8.f}},
        {"HoverStationaryDelay", {&style.HoverStationaryDelay, 2.f}},
        {"HoverDelayShort", {&style.HoverDelayShort, 2.f}},
        {"HoverDelayNormal", {&style.HoverDelayNormal, 2.f}}
    };
    for (const auto attribute : styleNode.attributes())
    {
        const auto found = styleValues.find(attribute.name());
        float value = 0.f;
        if (found == styleValues.end())
        {
            error = "unknown Style attribute: " + std::string(attribute.name());
            return false;
        }
        if (!ReadFloat(attribute, 0.f, found->second.second, value))
        {
            error = "Style attribute is outside its valid range: " +
                std::string(attribute.name());
            return false;
        }
        *found->second.first = value;
    }

    std::set<int> assignedColors;
    for (const auto colorNode : colorsNode.children())
    {
        if (std::strcmp(colorNode.name(), "Color") != 0)
        {
            error = "Colors may only contain Color elements";
            return false;
        }
        const int color = FindColor(colorNode.attribute("name").as_string());
        if (color < 0 || !assignedColors.insert(color).second)
        {
            error = "unknown or duplicate ImGui color: " +
                std::string(colorNode.attribute("name").as_string());
            return false;
        }
        ImVec4 value;
        if (!ReadFloat(colorNode.attribute("r"), 0.f, 1.f, value.x) ||
            !ReadFloat(colorNode.attribute("g"), 0.f, 1.f, value.y) ||
            !ReadFloat(colorNode.attribute("b"), 0.f, 1.f, value.z) ||
            !ReadFloat(colorNode.attribute("a"), 0.f, 1.f, value.w))
        {
            error = "Color values must be numbers from 0 to 1";
            return false;
        }
        for (const auto attribute : colorNode.attributes())
            if (std::strcmp(attribute.name(), "name") != 0 &&
                std::strcmp(attribute.name(), "r") != 0 &&
                std::strcmp(attribute.name(), "g") != 0 &&
                std::strcmp(attribute.name(), "b") != 0 &&
                std::strcmp(attribute.name(), "a") != 0)
            {
                error = "unknown Color attribute: " + std::string(attribute.name());
                return false;
            }
        style.Colors[color] = value;
    }

    const char* required[] = { "Text", "WindowBg", "FrameBg", "Button", "Header" };
    for (const char* requiredName : required)
        if (assignedColors.count(FindColor(requiredName)) == 0)
        {
            error = "theme is missing required color: " + std::string(requiredName);
            return false;
        }

    for (const auto child : root.children())
        if (std::strcmp(child.name(), "Style") != 0 &&
            std::strcmp(child.name(), "Colors") != 0)
        {
            error = "unknown ImGuiTheme element: " + std::string(child.name());
            return false;
        }
    if (name) *name = themeName;
    if (output) *output = style;
    return true;
}
}

std::string ImGuiThemeManager::ThemeDirectory()
{
    return (fs::path(ENGINE_ROOT_PATH) / "Engine" / "Editor" / "UI" /
        "ImGui" / "Themes").lexically_normal().string();
}

bool ImGuiThemeManager::Refresh(std::string* error)
{
    g_themes.clear();
    g_themeNames.clear();
    std::vector<std::string> rejected;
    std::error_code filesystemError;
    const fs::path directory = ThemeDirectory();
    if (!fs::is_directory(directory, filesystemError))
    {
        if (error) *error = "Theme directory was not found: " + directory.string();
        return false;
    }

    for (const auto& entry : fs::directory_iterator(directory, filesystemError))
    {
        if (filesystemError) break;
        if (!entry.is_regular_file() || entry.path().extension() != ".imguitheme") continue;
        std::string name;
        std::string parseError;
        if (!ParseTheme(entry.path(), &name, nullptr, parseError))
        {
            rejected.push_back(entry.path().filename().string() + ": " + parseError);
            continue;
        }
        const bool duplicate = std::any_of(g_themes.begin(), g_themes.end(),
            [&name](const ThemeFile& theme) { return theme.name == name; });
        if (duplicate)
        {
            rejected.push_back(entry.path().filename().string() + ": duplicate theme name");
            continue;
        }
        g_themes.push_back({ std::move(name), entry.path() });
    }
    std::sort(g_themes.begin(), g_themes.end(), [](const ThemeFile& left, const ThemeFile& right)
    {
        return left.name < right.name;
    });
    for (const auto& theme : g_themes) g_themeNames.push_back(theme.name);

    if (error)
    {
        error->clear();
        if (filesystemError) *error = "Could not scan theme directory: " + filesystemError.message();
        else if (!rejected.empty())
        {
            *error = "Ignored invalid theme files: ";
            for (size_t index = 0; index < rejected.size(); ++index)
            {
                if (index) *error += "; ";
                *error += rejected[index];
            }
        }
    }
    return !g_themes.empty();
}

const std::vector<std::string>& ImGuiThemeManager::AvailableThemes()
{
    return g_themeNames;
}

bool ImGuiThemeManager::Apply(const std::string& name, std::string* error)
{
    const auto found = std::find_if(g_themes.begin(), g_themes.end(),
        [&name](const ThemeFile& theme) { return theme.name == name; });
    if (found == g_themes.end())
    {
        if (error) *error = "Theme is not available: " + name;
        return false;
    }
    ImGuiStyle style;
    std::string parsedName;
    std::string parseError;
    if (!ParseTheme(found->path, &parsedName, &style, parseError))
    {
        if (error) *error = found->path.filename().string() + ": " + parseError;
        return false;
    }
    ImGui::GetStyle() = style;
    if (error) error->clear();
    return true;
}
}
