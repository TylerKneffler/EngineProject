#pragma once

#include "Core/Component.h"
#include <string>
#include <vector>

class Object;

// Imported-model metadata owned by the model root. Node bindings are stored as
// child-index paths relative to the root, keeping generic Object free of model
// importer fields and avoiding one marker component per imported node.
class Model : public Component
{
public:
    Model();
    void BindNode(unsigned index, Object* object);
    Object* ResolveNode(unsigned index) const;
    size_t GetNodeCount() const { return m_nodePaths.size(); }
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(IEditorUi& ui) override;

private:
    std::vector<std::string> m_nodePaths;
};
