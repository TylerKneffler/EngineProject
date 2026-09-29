#include "Model.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <sstream>

namespace Engine::Components
{
namespace
{
std::string RelativePath(const Engine::Core::Object* root, const Engine::Core::Object* object)
{
    std::vector<size_t> indices;
    for (const Engine::Core::Object* current = object; current && current != root;
        current = current->Parent)
    {
        if (!current->Parent) return {};
        const auto found = std::find(current->Parent->Children.begin(),
            current->Parent->Children.end(), current);
        if (found == current->Parent->Children.end()) return {};
        indices.push_back(static_cast<size_t>(
            found - current->Parent->Children.begin()));
    }
    if (!object || (object != root && indices.empty())) return {};
    std::reverse(indices.begin(), indices.end());
    std::ostringstream output;
    for (size_t i = 0; i < indices.size(); ++i)
    {
        if (i) output << '.';
        output << indices[i];
    }
    return output.str();
}
}

Model::Model()
{
    SetTypeName(COMPONENT_TYPE_NAME(Model));
    singlecomponent = true;
    editorAddable = false;
}

void Model::BindNode(unsigned index, Object* object)
{
    if (!Owner || !object) return;
    if (m_nodePaths.size() <= index) m_nodePaths.resize(index + 1);
    m_nodePaths[index] = RelativePath(Owner, object);
    m_resolvedNodes.clear();
    m_cachedStructureRevision = 0;
    MarkConfigurationDirty();
}

Model::Object* Model::ResolveNode(unsigned index) const
{
    const std::vector<Object*>& nodes = ResolveNodes();
    return index < nodes.size() ? nodes[index] : nullptr;
}

const std::vector<Model::Object*>& Model::ResolveNodes() const
{
    const uint64_t revision = Owner && Owner->GetScene()
        ? Owner->GetScene()->GetStructureRevision() : 0;
    if (m_resolvedNodes.size() != m_nodePaths.size() ||
        m_cachedStructureRevision != revision)
        RefreshNodeCache();
    return m_resolvedNodes;
}

void Model::RefreshNodeCache() const
{
    m_resolvedNodes.assign(m_nodePaths.size(), nullptr);
    for (size_t index = 0; Owner && index < m_nodePaths.size(); ++index)
    {
        Object* object = Owner;
        std::istringstream input(m_nodePaths[index]);
        std::string part;
        try
        {
            while (std::getline(input, part, '.'))
            {
                if (part.empty()) continue;
                const size_t child = static_cast<size_t>(std::stoull(part));
                if (child >= object->Children.size())
                {
                    object = nullptr;
                    break;
                }
                object = object->Children[child];
            }
        }
        catch (...) { object = nullptr; }
        m_resolvedNodes[index] = object;
    }
    m_cachedStructureRevision = Owner && Owner->GetScene()
        ? Owner->GetScene()->GetStructureRevision() : 0;
}

Model::JsonValue Model::Serialize() const
{
    JsonValue result = Component::Serialize();
    JsonValue paths = JsonValue::MakeArray();
    for (const std::string& path : m_nodePaths) paths.Push(JsonValue(path));
    return result.Set("nodePaths", std::move(paths));
}

void Model::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    m_nodePaths.clear();
    m_resolvedNodes.clear();
    m_cachedStructureRevision = 0;
    const JsonValue& paths = value["nodePaths"];
    for (size_t i = 0; i < paths.ArraySize(); ++i)
        m_nodePaths.push_back(paths.ArrayAt(i).AsString());
}

bool Model::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    const std::string count = std::to_string(m_nodePaths.size());
    ui.ValueLabel("Imported Nodes", count.c_str());
    return false;
}
}
