#include "Core/Serialization/SceneSerializerInternal.h"
#include <pugixml.hpp>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace Engine::Serialization
{
// ---- Local GLM helpers ------------------------------------------------------
namespace Detail
{

namespace SceneXml
{
constexpr const char* ComponentPrefix = "components";
constexpr const char* ComponentNamespace = "urn:engine-components";
constexpr const char* TypeField = "type";
constexpr const char* ArrayItem = "item";

// Files written before arrays became self-describing used semantic collection
// element names. Keep those names here only as an input compatibility rule;
// newly serialized arrays do not depend on them.
bool IsLegacyCollection(const std::string& name)
{
    return name == "objects" || name == "components" || name == "children";
}
}

bool SplitQualifiedName(const std::string& name, std::string& prefix, std::string& localName)
{
    const std::size_t colon = name.find(':');
    if (colon == std::string::npos)
        return false;
    prefix = name.substr(0, colon);
    localName = name.substr(colon + 1);
    return true;
}

const SceneSerializer::Factory* ComponentRegistryBehavior::FindFactory(
    const std::unordered_map<std::string, SceneSerializer::Factory>& registry,
    const std::string& typeName)
{
    const auto exact = registry.find(typeName);
    if (exact != registry.end())
        return &exact->second;

    // Older XML writers normalized component element casing. Preserve input
    // compatibility without changing the class-defined serialized type name.
    const auto sameIgnoringCase = [&typeName](const auto& entry)
    {
        if (entry.first.size() != typeName.size())
            return false;
        return std::equal(entry.first.begin(), entry.first.end(), typeName.begin(),
            [](unsigned char left, unsigned char right)
            {
                return std::tolower(left) == std::tolower(right);
            });
    };
    const auto legacy = std::find_if(registry.begin(), registry.end(), sameIgnoringCase);
    return legacy == registry.end() ? nullptr : &legacy->second;
}

std::string StripOwnedPrefix(const std::string& childName)
{
    const std::size_t dot = childName.rfind('.');
    if (dot == std::string::npos)
        return childName;
    return childName.substr(dot + 1);
}

JsonValue ParseScalarText(const std::string& text)
{
    if (text == "true")
        return JsonValue(true);
    if (text == "false")
        return JsonValue(false);

    if (!text.empty())
    {
        char* end = nullptr;
        errno = 0;
        const double number = std::strtod(text.c_str(), &end);
        if (end && *end == '\0' && end != text.c_str() && errno != ERANGE)
            return JsonValue(number);
    }

    return JsonValue(text);
}

bool IsScalarValue(const JsonValue& value)
{
    const JsonValue::Type type = value.GetType();
    return type == JsonValue::Type::Null ||
        type == JsonValue::Type::Bool ||
        type == JsonValue::Type::Number ||
        type == JsonValue::Type::String;
}

std::string ScalarToText(const JsonValue& value)
{
    switch (value.GetType())
    {
    case JsonValue::Type::Null:
        return std::string();
    case JsonValue::Type::Bool:
        return value.AsBool() ? "true" : "false";
    case JsonValue::Type::Number:
    {
        std::ostringstream stream;
        stream << value.AsDouble();
        return stream.str();
    }
    case JsonValue::Type::String:
        return value.AsString();
    default:
        return std::string();
    }
}

JsonValue SceneXmlBehavior::ReadNode(const pugi::xml_node& node)
{
    const std::string name = node.name();
    const std::string fieldName = StripOwnedPrefix(name);
    std::string namespacePrefix;
    std::string localName;
    const bool isQualified =
        SplitQualifiedName(name, namespacePrefix, localName);

    if (isQualified && namespacePrefix == SceneXml::ComponentPrefix)
    {
        JsonValue component = JsonValue::MakeObject();
        component.Set(SceneXml::TypeField, JsonValue(localName));
        for (const pugi::xml_attribute& attribute : node.attributes())
            component.Set(StripOwnedPrefix(attribute.name()),
                ParseScalarText(attribute.value()));
        for (const pugi::xml_node& child : node.children())
            if (child.type() == pugi::node_element)
                component.Set(StripOwnedPrefix(child.name()), ReadNode(child));
        return component;
    }

    bool hasElementChildren = false;
    const bool hasAttributes = node.first_attribute() != nullptr;
    bool allItems = true;
    bool allComponents = true;
    for (const pugi::xml_node& child : node.children())
    {
        if (child.type() != pugi::node_element)
            continue;
        hasElementChildren = true;
        allItems = allItems && std::string(child.name()) == SceneXml::ArrayItem;
        std::string childPrefix;
        std::string childLocalName;
        allComponents = allComponents &&
            SplitQualifiedName(child.name(), childPrefix, childLocalName) &&
            childPrefix == SceneXml::ComponentPrefix;
    }

    if (SceneXml::IsLegacyCollection(fieldName) ||
        (hasElementChildren && (allItems || allComponents)))
    {
        JsonValue array = JsonValue::MakeArray();
        for (const pugi::xml_node& child : node.children())
            if (child.type() == pugi::node_element)
                array.Push(ReadNode(child));
        return array;
    }

    if (!hasElementChildren && !hasAttributes)
        return ParseScalarText(node.child_value());

    JsonValue object = JsonValue::MakeObject();
    for (const pugi::xml_attribute& attribute : node.attributes())
        object.Set(StripOwnedPrefix(attribute.name()),
            ParseScalarText(attribute.value()));
    for (const pugi::xml_node& child : node.children())
    {
        if (child.type() != pugi::node_element)
            continue;
        object.Set(StripOwnedPrefix(child.name()), ReadNode(child));
    }
    return object;
}

void SceneXmlBehavior::WriteArrayItems(pugi::xml_node parent, const JsonValue& value, const char* itemName)
{
    for (std::size_t i = 0; i < value.ArraySize(); ++i)
    {
        const JsonValue& item = value.ArrayAt(i);
        if (item.IsObject() && item[SceneXml::TypeField].IsString() &&
            !item[SceneXml::TypeField].AsString().empty())
        {
            const std::string elementName = std::string(SceneXml::ComponentPrefix) +
                ":" + item[SceneXml::TypeField].AsString();
            pugi::xml_node child = parent.append_child(elementName.c_str());
            WriteObjectFields(child, item, SceneXml::TypeField,
                item[SceneXml::TypeField].AsString().c_str());
        }
        else
        {
            pugi::xml_node child = parent.append_child(itemName);
            WriteNode(child, item);
        }
    }
}

void SceneXmlBehavior::WriteObjectFields(pugi::xml_node parent, const JsonValue& value,
    const char* skipKey, const char* ownerName)
{
    for (std::size_t i = 0; i < value.ObjectSize(); ++i)
    {
        const std::string key = value.ObjectKey(i);
        if (skipKey && key == skipKey)
            continue;
        const std::string fieldName = ownerName && *ownerName
            ? std::string(ownerName) + "." + key
            : key;
        const JsonValue& fieldValue = value.ObjectValue(i);
        if (IsScalarValue(fieldValue))
        {
            const std::string text = ScalarToText(fieldValue);
            parent.append_attribute(fieldName.c_str()) = text.c_str();
        }
        else
            WriteField(parent, fieldName.c_str(), fieldValue);
    }
}

void SceneXmlBehavior::WriteField(pugi::xml_node parent, const char* name, const JsonValue& value)
{
    pugi::xml_node child = parent.append_child(name ? name : "");
    if (name && std::string(name) == "settings" && value.IsObject())
    {
        for (std::size_t index = 0; index < value.ObjectSize(); ++index)
        {
            const std::string key = value.ObjectKey(index);
            const JsonValue& fieldValue = value.ObjectValue(index);
            if (IsScalarValue(fieldValue))
            {
                const std::string text = ScalarToText(fieldValue);
                child.append_attribute(key.c_str()) = text.c_str();
            }
            else
                WriteField(child, key.c_str(), fieldValue);
        }
        return;
    }
    if (value.IsArray())
    {
        WriteArrayItems(child, value, SceneXml::ArrayItem);
        return;
    }
    if (value.IsObject() && value[SceneXml::TypeField].IsString() &&
        !value[SceneXml::TypeField].AsString().empty())
    {
        WriteObjectFields(child, value, SceneXml::TypeField,
            value[SceneXml::TypeField].AsString().c_str());
        return;
    }
    WriteNode(child, value);
}

void SceneXmlBehavior::WriteNode(pugi::xml_node node, const JsonValue& value)
{
    switch (value.GetType())
    {
    case JsonValue::Type::Null:
        node.text().set("");
        break;
    case JsonValue::Type::Bool:
        node.text().set(value.AsBool() ? "true" : "false");
        break;
    case JsonValue::Type::Number:
    {
        std::ostringstream stream;
        stream << value.AsDouble();
        node.text().set(stream.str().c_str());
        break;
    }
    case JsonValue::Type::String:
        node.text().set(value.AsString().c_str());
        break;
    case JsonValue::Type::Array:
        WriteArrayItems(node, value, "item");
        break;
    case JsonValue::Type::Object:
        WriteObjectFields(node, value);
        break;
    }
}

std::string SceneXmlBehavior::WriteDocument(const JsonValue& value, const char* rootName)
{
    pugi::xml_document document;
    pugi::xml_node root = document.append_child(rootName);
    root.append_attribute("xmlns:components") = SceneXml::ComponentNamespace;
    if (value.IsObject())
        WriteObjectFields(root, value);
    else
        WriteNode(root, value);

    std::ostringstream stream;
    document.save(stream, "  ");
    return stream.str();
}

bool SceneXmlBehavior::LoadDocument(Engine::Scene::Scene& scene, const std::string& source,
    Engine::Graphics::IGraphicsProvider* graphicsProvider,
    const std::unordered_map<std::string, SceneSerializer::Factory>& registry,
    bool isFile)
{
    pugi::xml_document document;
    pugi::xml_parse_result result = isFile
        ? document.load_file(source.c_str())
        : document.load_string(source.c_str());
    if (!result)
        return false;

    const pugi::xml_node root = document.document_element();
    if (!root)
        return false;

    return SceneDocumentBehavior::DeserializeScene(scene, ReadNode(root), graphicsProvider, registry);
}

} // namespace Detail

}
