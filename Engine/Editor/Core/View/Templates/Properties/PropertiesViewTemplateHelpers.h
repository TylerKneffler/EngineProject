#pragma once

#include "Core/Object.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace Engine::Editor
{
inline bool ContainsIgnoringCase(const std::string& value,
    const std::string& search)
{
    if (search.empty())
        return true;
    std::string loweredValue = value;
    std::string loweredSearch = search;
    std::transform(loweredValue.begin(), loweredValue.end(), loweredValue.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    std::transform(loweredSearch.begin(), loweredSearch.end(), loweredSearch.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return loweredValue.find(loweredSearch) != std::string::npos;
}

inline std::string FindComponentSubclass(const std::string& path)
{
    std::ifstream input(path);
    if (!input)
        return {};
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::regex declaration(
        R"(\b(class|struct)\s+([A-Za-z_]\w*)\s*(?:final\s*)?:\s*([^\{]+)\{)");
    const std::regex publicComponent(
        R"((?:^|,)\s*public\s+(?:[A-Za-z_]\w*::)*(?:Component|Script)\b)");
    const std::regex componentBase(
        R"((?:^|,)\s*(?:[A-Za-z_]\w*::)*(?:Component|Script)\b)");
    const std::string source = contents.str();
    const std::string preferred = std::filesystem::path(path).stem().string();
    std::string firstMatch;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), declaration);
        it != std::sregex_iterator(); ++it)
    {
        const bool isStruct = (*it)[1].str() == "struct";
        const std::string className = (*it)[2].str();
        const std::string bases = (*it)[3].str();
        if (!std::regex_search(bases, isStruct ? componentBase : publicComponent))
            continue;
        if (className == preferred)
            return className;
        if (firstMatch.empty())
            firstMatch = className;
    }
    return firstMatch;
}

template<typename T>
void ReplaceOrAddComponent(Engine::Core::Object& object, T* component)
{
    component->Owner = &object;
    for (auto it = object.Components.begin(); it != object.Components.end(); ++it)
    {
        if (dynamic_cast<T*>(*it))
        {
            delete *it;
            *it = component;
            return;
        }
    }
    object.Components.push_back(component);
}
}
