#include "PmxMaterialMorph.h"

#include "Core/Compoonents/Materials/Material.h"
#include "Core/Object.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>

namespace Engine::Components
{
namespace
{
using Value = Engine::Serialization::JsonValue;

Value VectorValue(const glm::vec4& vector, int size)
{
    Value result = Value::MakeArray();
    for (int i = 0; i < size; ++i) result.Push(Value(vector[i]));
    return result;
}

glm::vec4 ReadVector(const Value& value, int size)
{
    glm::vec4 result(0.f);
    if (value.ArraySize() == static_cast<size_t>(size))
        for (int i = 0; i < size; ++i)
            result[i] = value.ArrayAt(i).AsFloat();
    return result;
}
}

PmxMaterialMorph::PmxMaterialMorph()
{
    SetTypeName(COMPONENT_TYPE_NAME(PmxMaterialMorph));
    editorAddable = false;
    singlecomponent = true;
}

void PmxMaterialMorph::Start()
{
    m_applied = false;
    m_animatedMorphWeights.clear();
}

void PmxMaterialMorph::SetAnimatedMorphWeights(
    const std::unordered_map<std::string, float>& weights)
{
    m_animatedMorphWeights = weights;
}

void PmxMaterialMorph::Apply()
{
    Material* material = Owner ? Owner->GetComponent<Material>() : nullptr;
    if (!material) return;
    const auto capture = [&]()
    {
        State state;
        state.diffuse = material->diffuseColor;
        state.alpha = material->baseColorAlpha;
        state.specular = material->specularColor;
        state.shininess = material->shininess;
        state.ambient = material->ambientColor;
        state.textureTint = glm::vec4(material->baseColorTextureTint,
            material->baseColorTextureTintAlpha);
        state.alphaMode = material->alphaMode;
        return state;
    };
    const auto restore = [&](const State& state)
    {
        material->diffuseColor = state.diffuse;
        material->baseColorAlpha = state.alpha;
        material->specularColor = state.specular;
        material->shininess = state.shininess;
        material->ambientColor = state.ambient;
        material->baseColorTextureTint = glm::vec3(state.textureTint);
        material->baseColorTextureTintAlpha = state.textureTint.a;
        material->alphaMode = state.alphaMode;
    };
    if (m_applied &&
        glm::all(glm::equal(material->diffuseColor, m_output.diffuse)) &&
        material->baseColorAlpha == m_output.alpha &&
        glm::all(glm::equal(material->specularColor, m_output.specular)) &&
        material->shininess == m_output.shininess &&
        glm::all(glm::equal(material->ambientColor, m_output.ambient)) &&
        glm::all(glm::equal(glm::vec4(material->baseColorTextureTint,
            material->baseColorTextureTintAlpha), m_output.textureTint)) &&
        material->alphaMode == m_output.alphaMode)
        restore(m_input);
    m_input = capture();
    m_applied = false;
    glm::vec4 diffuse(material->diffuseColor, material->baseColorAlpha);
    glm::vec4 textureTint(material->baseColorTextureTint,
        material->baseColorTextureTintAlpha);
    for (const auto& morph : morphs)
    {
        const auto animated = m_animatedMorphWeights.find(morph.name);
        const float weight = std::clamp(animated == m_animatedMorphWeights.end()
            ? morph.weight : animated->second, 0.f, 1.f);
        if (weight <= 0.f) continue;
        for (const auto& offset : morph.offsets)
        {
            m_applied = true;
            if (offset.additive)
            {
                diffuse += offset.diffuse * weight;
                material->specularColor += offset.specular * weight;
                material->shininess += offset.shininess * weight;
                material->ambientColor += offset.ambient * weight;
                textureTint += offset.textureTint * weight;
            }
            else
            {
                diffuse *= glm::mix(glm::vec4(1.f), offset.diffuse, weight);
                material->specularColor *= glm::mix(glm::vec3(1.f),
                    offset.specular, weight);
                material->shininess *= glm::mix(1.f, offset.shininess, weight);
                material->ambientColor *= glm::mix(glm::vec3(1.f),
                    offset.ambient, weight);
                textureTint *= glm::mix(glm::vec4(1.f),
                    offset.textureTint, weight);
            }
        }
    }
    material->diffuseColor = glm::vec3(diffuse);
    material->baseColorAlpha = diffuse.a;
    material->baseColorTextureTint = glm::vec3(textureTint);
    material->baseColorTextureTintAlpha = textureTint.a;
    if (material->baseColorAlpha < .999f ||
        material->baseColorTextureTintAlpha < .999f)
        material->alphaMode = "Blend";
    m_output = capture();
}

bool PmxMaterialMorph::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = Component::DrawProperties(ui);
    if (ui.PropertyGroupHeader("PMX Material Morphs"))
    {
        ui.Indent(12.f);
        for (size_t i = 0; i < morphs.size(); ++i)
        {
            const std::string id = std::to_string(i);
            ui.PushId(id.c_str());
            changed |= ui.DragFloat(morphs[i].name.c_str(),
                &morphs[i].weight, .01f, 0.f, 1.f);
            ui.PopId();
        }
        ui.Unindent(12.f);
    }
    if (changed) MarkConfigurationDirty();
    return changed;
}

PmxMaterialMorph::JsonValue PmxMaterialMorph::Serialize() const
{
    Value result = Component::Serialize();
    Value values = Value::MakeArray();
    for (const auto& morph : morphs)
    {
        Value offsets = Value::MakeArray();
        for (const auto& offset : morph.offsets)
            offsets.Push(Value::MakeObject()
                .Set("additive", Value(offset.additive))
                .Set("diffuse", VectorValue(offset.diffuse, 4))
                .Set("specular", VectorValue(glm::vec4(offset.specular, 0.f), 3))
                .Set("shininess", Value(offset.shininess))
                .Set("ambient", VectorValue(glm::vec4(offset.ambient, 0.f), 3))
                .Set("textureTint", VectorValue(offset.textureTint, 4)));
        values.Push(Value::MakeObject()
            .Set("name", Value(morph.name))
            .Set("weight", Value(morph.weight))
            .Set("offsets", std::move(offsets)));
    }
    return result.Set("morphs", std::move(values));
}

void PmxMaterialMorph::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    morphs.clear();
    for (size_t i = 0; i < value["morphs"].ArraySize(); ++i)
    {
        const auto& source = value["morphs"].ArrayAt(i);
        auto& morph = morphs.emplace_back();
        morph.name = source["name"].AsString();
        morph.weight = source["weight"].AsFloat();
        for (size_t j = 0; j < source["offsets"].ArraySize(); ++j)
        {
            const auto& item = source["offsets"].ArrayAt(j);
            auto& offset = morph.offsets.emplace_back();
            offset.additive = item["additive"].AsBool();
            offset.diffuse = ReadVector(item["diffuse"], 4);
            offset.specular = glm::vec3(ReadVector(item["specular"], 3));
            offset.shininess = item["shininess"].AsFloat();
            offset.ambient = glm::vec3(ReadVector(item["ambient"], 3));
            offset.textureTint = item.Has("textureTint")
                ? ReadVector(item["textureTint"], 4)
                : glm::vec4(offset.additive ? 0.f : 1.f);
        }
    }
    Start();
}
}
