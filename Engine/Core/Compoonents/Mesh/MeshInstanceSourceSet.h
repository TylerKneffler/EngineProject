#pragma once

#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Serialization/Json.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Engine::Components
{
// Shared ownership for the geometry and material state of a reusable
// multi-part instance source. Placement and streaming policy are external.
class MeshInstanceSourceSet
{
public:
    struct Part
    {
        std::string key;
        std::vector<std::shared_ptr<Mesh>> representations;
        Engine::Serialization::JsonValue materialState;
        bool hasMaterialState = false;
        glm::mat4 sourceTransform{1.f};
    };

    void SetPart(Part part)
    {
        m_parts[part.key] = std::move(part);
        ++m_revision;
    }

    void SetRepresentations(const std::string& key,
        std::vector<std::shared_ptr<Mesh>> representations)
    {
        auto& part = m_parts[key];
        part.key = key;
        part.representations = std::move(representations);
        ++m_revision;
    }

    void SetMaterialState(const std::string& key,
        Engine::Serialization::JsonValue materialState)
    {
        auto& part = m_parts[key];
        part.key = key;
        part.materialState = std::move(materialState);
        part.hasMaterialState = true;
        ++m_revision;
    }

    Part* FindPart(const std::string& key)
    {
        const auto found = m_parts.find(key);
        return found == m_parts.end() ? nullptr : &found->second;
    }
    const Part* FindPart(const std::string& key) const
    {
        const auto found = m_parts.find(key);
        return found == m_parts.end() ? nullptr : &found->second;
    }

    const std::map<std::string, Part>& GetParts() const { return m_parts; }
    uint64_t GetRevision() const { return m_revision; }
    bool Empty() const { return m_parts.empty(); }
    void Clear()
    {
        m_parts.clear();
        ++m_revision;
    }

private:
    std::map<std::string, Part> m_parts;
    uint64_t m_revision = 1;
};
}
