#pragma once

#include "Core/Component.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Engine::Components
{
// Imported-model metadata owned by the model root. Node bindings are stored as
// child-index paths relative to the root, keeping generic Object free of model
// importer fields and avoiding one marker component per imported node.
class Model : public Engine::Core::Component
{
public:
    Model();
    void BindNode(unsigned index, Object* object);
    Object* ResolveNode(unsigned index) const;
    const std::vector<Object*>& ResolveNodes() const;
    size_t GetNodeCount() const { return m_nodePaths.size(); }
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;

private:
    void RefreshNodeCache() const;
    std::vector<std::string> m_nodePaths;
    mutable std::vector<Object*> m_resolvedNodes;
    mutable uint64_t m_cachedStructureRevision = 0;
};
}
