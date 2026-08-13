#pragma once

#include "Core/Component.h"
#include <glm/glm.hpp>
#include <vector>

class Object;

class Skeleton : public Component
{
public:
    Skeleton();
    ComponentReference modelReference { "Model" };
    bool showBones = true;
    unsigned skinIndex = 0;
    std::vector<unsigned> jointNodes;
    std::vector<glm::mat4> inverseBindMatrices;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(IEditorUi& ui) override;
    Object* GetHierarchyRoot() const;
    std::vector<Object*> ResolveJoints() const;
    Object* FindNode(unsigned index) const;
    class Model* ResolveModel() const;
};
