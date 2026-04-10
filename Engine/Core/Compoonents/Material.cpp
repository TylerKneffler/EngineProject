#include "Material.h"

namespace
{
    JsonValue J3(const glm::vec3& v)
    {
        return JsonValue::MakeArray().Push(JsonValue(v.x)).Push(JsonValue(v.y)).Push(JsonValue(v.z));
    }
    glm::vec3 from3(const JsonValue& v, glm::vec3 def = {})
    {
        if (!v.IsArray() || v.ArraySize() < 3) return def;
        return { v.ArrayAt(0).AsFloat(), v.ArrayAt(1).AsFloat(), v.ArrayAt(2).AsFloat() };
    }
}

JsonValue Material::Serialize() const
{
    JsonValue node = JsonValue::MakeObject();
    node.Set("type",     JsonValue(std::string("Material")));
    node.Set("diffuse",  J3(diffuseColor));
    node.Set("ambient",  J3(ambientColor));
    node.Set("specular", J3(specularColor));
    node.Set("shininess",JsonValue(shininess));
    return node;
}

void Material::Deserialize(const JsonValue& v)
{
    diffuseColor  = from3(v["diffuse"],  diffuseColor);
    ambientColor  = from3(v["ambient"],  ambientColor);
    specularColor = from3(v["specular"], specularColor);
    if (v.Has("shininess")) shininess = v["shininess"].AsFloat();
}
