#include "Core/Serialization/SceneSerializer.h"
#include "Core/Component.h"
#include <string>
#include <vector>

namespace
{
const std::vector<std::string>& ScriptTypes()
{
    static const std::vector<std::string> types =
        Engine::Serialization::SceneSerializer::GetRegisteredScriptTypes();
    return types;
}
}

extern "C" __declspec(dllexport) int EngineScriptTypeCount()
{
    return static_cast<int>(ScriptTypes().size());
}

extern "C" __declspec(dllexport) const char* EngineScriptTypeName(int index)
{
    const auto& types = ScriptTypes();
    return index >= 0 && static_cast<size_t>(index) < types.size()
        ? types[static_cast<size_t>(index)].c_str() : nullptr;
}

extern "C" __declspec(dllexport) Engine::Core::Component* EngineCreateScript(
    const char* typeName)
{
    return typeName ? Engine::Serialization::SceneSerializer::CreateRegisteredComponent(typeName) : nullptr;
}
