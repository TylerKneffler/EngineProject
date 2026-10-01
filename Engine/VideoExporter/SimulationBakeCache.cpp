#include "VideoExporter/SimulationBakeCache.h"

#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Cinematics/CameraTrack.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Physics/Physics.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/Json.h"

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace Engine::Video
{
namespace
{
template<class T>
bool WriteValue(std::ofstream& stream, const T& value)
{
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return stream.good();
}

template<class T>
bool ReadValue(std::ifstream& stream, T& value)
{
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return stream.good();
}

uint64_t HashString(uint64_t hash, const std::string& value)
{
    for (const unsigned char character : value)
    {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    return hash * 1099511628211ull;
}

bool IsPresentation(const Engine::Core::Object* object);

void Flatten(Engine::Core::Object* object, const std::string& path,
    std::vector<Engine::Core::Object*>& objects,
    std::vector<std::string>& keys)
{
    if (!object) return;
    objects.push_back(object);
    keys.push_back(path);
    for (size_t index = 0; index < object->Children.size(); ++index)
        Flatten(object->Children[index], path + "/" + std::to_string(index),
            objects, keys);
}

uint64_t LayoutHash(Engine::Scene::Scene& scene,
    std::vector<Engine::Core::Object*>& objects, bool& hasCloth)
{
    std::vector<std::string> keys;
    for (size_t index = 0; index < scene.GetObjects().size(); ++index)
        Flatten(scene.GetObjects()[index].get(), std::to_string(index),
            objects, keys);
    uint64_t hash = 1469598103934665603ull;
    for (size_t index = 0; index < objects.size(); ++index)
    {
        const auto* object = objects[index];
        hash = HashString(hash, keys[index]);
        const bool presentation = IsPresentation(object);
        hash = HashString(hash, presentation ? "presentation" : "simulation");
        if (!presentation)
            hash = HashString(hash, object->name);
        if (presentation) continue;
        for (const Engine::Core::Component* component : object->Components)
        {
            hash = HashString(hash, component->GetTypeName());
            if (const auto* body = dynamic_cast<const Engine::Components::RigidBody*>(component))
            {
                std::ostringstream settings;
                settings << body->bodyType << ':' << body->mass << ':'
                    << body->useGravity << ':' << body->gravityScale << ':'
                    << body->gravityDirection.x << ':' << body->gravityDirection.y
                    << ':' << body->gravityDirection.z << ':' << body->linearDamping
                    << ':' << body->angularDamping << ':' << body->friction << ':'
                    << body->restitution << ':' << body->initialLinearVelocity.x
                    << ':' << body->initialLinearVelocity.y << ':'
                    << body->initialLinearVelocity.z << ':' << body->collisionLayer
                    << ':' << body->collisionMask;
                hash = HashString(hash, settings.str());
            }
            else if (const auto* cloth = dynamic_cast<const Engine::Components::Cloth*>(component))
            {
                hasCloth = true;
                std::ostringstream settings;
                settings << cloth->meshPath << ':' << cloth->mass << ':'
                    << cloth->linearStiffness << ':' << cloth->bendingStiffness
                    << ':' << cloth->damping << ':' << cloth->drag << ':'
                    << cloth->gravityScale << ':' << cloth->solverIterations
                    << ':' << cloth->selfCollision << ':' << cloth->windStrength
                    << ':' << cloth->windVelocity.x << ':'
                    << cloth->windVelocity.y << ':' << cloth->windVelocity.z;
                hash = HashString(hash, settings.str());
            }
            else if (const auto* animation = dynamic_cast<const Engine::Components::AnimationManager*>(component))
            {
                std::ostringstream settings;
                settings << animation->clip << ':' << animation->playing << ':'
                    << animation->looping << ':' << animation->speed;
                hash = HashString(hash, settings.str());
            }
            else if (const auto* mesh = dynamic_cast<const Engine::Components::Mesh*>(component))
            {
                hash = HashString(hash, mesh->GetFilePath());
                const uint32_t vertexCount = mesh->GetVertexCount();
                hash = HashString(hash, std::to_string(vertexCount));
            }
        }
    }
    return hash;
}

bool IsPresentation(const Engine::Core::Object* object)
{
    for (const Engine::Core::Object* current = object; current;
        current = current->Parent)
    {
        if (current->GetComponent<Engine::Components::Camera>() ||
            current->GetComponent<Engine::Components::Light>() ||
            current->GetComponent<CameraTrack>())
            return true;
    }
    return false;
}

bool WriteVector3(std::ofstream& stream, const glm::vec3& value)
{
    return WriteValue(stream, value.x) && WriteValue(stream, value.y) &&
        WriteValue(stream, value.z);
}

bool ReadVector3(std::ifstream& stream, glm::vec3& value)
{
    return ReadValue(stream, value.x) && ReadValue(stream, value.y) &&
        ReadValue(stream, value.z);
}
}

SimulationBakeCache::~SimulationBakeCache()
{
    if (m_output.is_open()) m_output.close();
    if (m_mode == Mode::Recording)
    {
        std::error_code ignored;
        std::filesystem::remove(m_temporaryPath, ignored);
    }
}

uint64_t SimulationBakeCache::BuildCompatibilitySignature(
    Engine::Scene::Scene& scene)
{
    std::vector<Engine::Core::Object*> objects;
    bool hasCloth = false;
    LayoutHash(scene, objects, hasCloth);
    uint64_t hash = 1469598103934665603ull;
    for (const Engine::Core::Object* object : objects)
    {
        if (IsPresentation(object)) continue;
        hash = HashString(hash, object->name);
        std::ostringstream transform;
        transform << std::setprecision(9)
            << object->transform.position.x << ':' << object->transform.position.y
            << ':' << object->transform.position.z << ':'
            << object->transform.rotation.x << ':' << object->transform.rotation.y
            << ':' << object->transform.rotation.z << ':'
            << object->transform.scale.x << ':' << object->transform.scale.y
            << ':' << object->transform.scale.z;
        hash = HashString(hash, transform.str());
        for (const Engine::Core::Component* component : object->Components)
            hash = HashString(hash, Engine::Serialization::JsonWrite(
                component->Serialize()));
    }
    return hash;
}

bool SimulationBakeCache::Initialize(Engine::Scene::Scene& scene,
    const std::filesystem::path& path, uint64_t compatibility,
    uint64_t expectedSamples, bool enabled)
{
    m_path = path;
    m_temporaryPath = path.wstring() + L".tmp";
    m_compatibility = compatibility;
    m_expectedSamples = expectedSamples;
    if (!enabled || expectedSamples == 0u)
        return true;
    m_schema = LayoutHash(scene, m_objects, m_hasCloth);

    std::ifstream existing(path, std::ios::binary);
    if (existing)
    {
        char magic[8]{};
        uint32_t version = 0;
        uint64_t key = 0, schema = 0, samples = 0;
        uint32_t objectCount = 0;
        uint8_t hasCloth = 0;
        existing.read(magic, sizeof(magic));
        const bool headerValid = existing.good() &&
            std::memcmp(magic, "SIMBAKE1", 8) == 0 &&
            ReadValue(existing, version) && ReadValue(existing, key) &&
            ReadValue(existing, schema) && ReadValue(existing, samples) &&
            ReadValue(existing, objectCount) && ReadValue(existing, hasCloth);
        if (headerValid && version == 1u && key == m_compatibility &&
            schema == m_schema && samples == m_expectedSamples &&
            objectCount == m_objects.size() && (hasCloth != 0u) == m_hasCloth)
        {
            m_input = std::move(existing);
            m_mode = Mode::Replay;
            return true;
        }
    }

    m_output.open(m_temporaryPath, std::ios::binary | std::ios::trunc);
    if (!m_output)
        return false;
    const char magic[8] = { 'S','I','M','B','A','K','E','1' };
    const uint32_t version = 1u;
    const uint32_t objectCount = static_cast<uint32_t>(m_objects.size());
    const uint8_t hasCloth = m_hasCloth ? 1u : 0u;
    m_output.write(magic, sizeof(magic));
    if (!WriteValue(m_output, version) || !WriteValue(m_output, m_compatibility) ||
        !WriteValue(m_output, m_schema) || !WriteValue(m_output, m_expectedSamples) ||
        !WriteValue(m_output, objectCount) || !WriteValue(m_output, hasCloth))
        return false;
    m_mode = Mode::Recording;
    return true;
}

bool SimulationBakeCache::RefreshObjects(Engine::Scene::Scene& scene)
{
    std::vector<Engine::Core::Object*> objects;
    bool hasCloth = false;
    if (LayoutHash(scene, objects, hasCloth) != m_schema ||
        objects.size() != m_objects.size() || hasCloth != m_hasCloth)
        return false;
    for (size_t index = 0; index < objects.size(); ++index)
        if (objects[index] != m_objects[index])
            return false;
    return true;
}

bool SimulationBakeCache::CaptureCurrent(Engine::Scene::Scene& scene)
{
    if (m_mode != Mode::Recording || !RefreshObjects(scene) ||
        m_sampleCount >= m_expectedSamples)
        return false;
    for (Engine::Core::Object* object : m_objects)
    {
        const bool presentation = IsPresentation(object);
        if (!WriteValue(m_output, presentation)) return false;
        if (!presentation &&
            (!WriteVector3(m_output, object->transform.position) ||
             !WriteVector3(m_output, object->transform.rotation) ||
             !WriteVector3(m_output, object->transform.scale))) return false;
        Engine::Components::RigidBody* body = presentation ? nullptr
            : object->GetComponent<Engine::Components::RigidBody>();
        const bool hasBody = body != nullptr;
        if (!WriteValue(m_output, hasBody)) return false;
        if (hasBody && (!WriteVector3(m_output, body->GetLinearVelocity()) ||
            !WriteVector3(m_output, body->GetAngularVelocity()))) return false;
        const Engine::Components::Mesh* mesh = !presentation
            ? object->GetComponent<Engine::Components::Mesh>() : nullptr;
        const bool hasDeformedMesh = !presentation &&
            (object->GetComponent<Engine::Components::Cloth>() ||
             object->GetComponent<Engine::Components::SkinnedMesh>());
        const uint32_t vertexCount = hasDeformedMesh && mesh &&
            !mesh->UsesTerrainVertexFormat()
            ? mesh->GetVertexCount() : 0u;
        if (!WriteValue(m_output, vertexCount)) return false;
        if (vertexCount)
            m_output.write(reinterpret_cast<const char*>(mesh->GetVertices().data()),
                static_cast<std::streamsize>(vertexCount * sizeof(Engine::Components::Mesh::Vertex)));
        if (!m_output.good()) return false;
        const auto morphWeights = mesh ? mesh->GetMorphWeights()
            : std::vector<float>{};
        const uint32_t morphCount = static_cast<uint32_t>(morphWeights.size());
        if (!WriteValue(m_output, morphCount)) return false;
        if (morphCount)
            m_output.write(reinterpret_cast<const char*>(morphWeights.data()),
                static_cast<std::streamsize>(morphCount * sizeof(float)));
        if (!m_output.good()) return false;
    }
    ++m_sampleCount;
    return true;
}

bool SimulationBakeCache::ApplyNext(Engine::Scene::Scene& scene)
{
    if (m_mode != Mode::Replay || !RefreshObjects(scene) ||
        m_sampleCount >= m_expectedSamples)
        return false;
    for (Engine::Core::Object* object : m_objects)
    {
        bool presentation = false;
        if (!ReadValue(m_input, presentation)) return false;
        glm::vec3 position{}, rotation{}, scale{1.f};
        if (!presentation && (!ReadVector3(m_input, position) ||
            !ReadVector3(m_input, rotation) || !ReadVector3(m_input, scale)))
            return false;
        bool hasBody = false;
        glm::vec3 linearVelocity{}, angularVelocity{};
        if (!ReadValue(m_input, hasBody)) return false;
        if (hasBody && (!ReadVector3(m_input, linearVelocity) ||
            !ReadVector3(m_input, angularVelocity))) return false;
        uint32_t vertexCount = 0;
        if (!ReadValue(m_input, vertexCount) || vertexCount > 100000000u)
            return false;
        std::vector<Engine::Components::Mesh::Vertex> vertices(vertexCount);
        if (vertexCount)
        {
            m_input.read(reinterpret_cast<char*>(vertices.data()),
                static_cast<std::streamsize>(vertexCount * sizeof(vertices[0])));
            if (!m_input.good()) return false;
        }
        uint32_t morphCount = 0;
        if (!ReadValue(m_input, morphCount) || morphCount > 65536u)
            return false;
        std::vector<float> morphWeights(morphCount);
        if (morphCount)
        {
            m_input.read(reinterpret_cast<char*>(morphWeights.data()),
                static_cast<std::streamsize>(morphCount * sizeof(float)));
            if (!m_input.good()) return false;
        }
        if (!presentation)
        {
            object->transform.position = position;
            object->transform.rotation = rotation;
            object->transform.scale = scale;
            object->transform.MarkDirty();
            if (vertexCount)
            {
                auto* mesh = object->GetComponent<Engine::Components::Mesh>();
                if (!mesh)
                    return false;
                mesh->SetDeformedVertices(std::move(vertices));
            }
            if (morphCount)
            {
                auto* mesh = object->GetComponent<Engine::Components::Mesh>();
                if (!mesh) return false;
                mesh->SetMorphWeights(morphWeights);
            }
            if (hasBody)
            {
                auto* body = object->GetComponent<Engine::Components::RigidBody>();
                if (!body) return false;
                body->NotifyEditorTransformChanged();
                body->SetLinearVelocity(linearVelocity);
                body->SetAngularVelocity(angularVelocity);
            }
        }
    }
    ++m_sampleCount;
    return true;
}

bool SimulationBakeCache::Complete()
{
    if (m_mode == Mode::Disabled) return true;
    if (m_sampleCount != m_expectedSamples)
        return false;
    if (m_mode == Mode::Replay) return true;
    m_output.flush();
    m_output.close();
    if (!m_output.good()) return false;
    if (!MoveFileExW(m_temporaryPath.c_str(), m_path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;
    m_mode = Mode::Disabled;
    return true;
}
}
