#pragma once
#include "Core/component.h"
#include "Core/PropertyMacros.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Model/MeshData.h"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace Engine::Components
{
class Mesh : public Engine::Core::Component
{
public:
    using Vertex = Engine::Model::Vertex;
    using MorphTarget = Engine::Model::MorphTarget;
    using JsonValue = Engine::Serialization::JsonValue;
    using IGraphicsBuffer = Engine::Graphics::IGraphicsBuffer;
    using IGraphicsBufferFactory = Engine::Graphics::IGraphicsBufferFactory;
    using IGraphicsProvider = Engine::Graphics::IGraphicsProvider;

    Mesh();
    ~Mesh() = default;

    // Load vertex data from a triangulated OBJ file (CPU side only).
    void LoadFromFile(const std::string& path);
    // Resolves portable Assets/... paths against bundled sandbox assets.
    static std::string ResolveFilePath(const std::string& path);
    static bool SaveNativeFile(const std::string& path, const std::vector<Vertex>& vertices);

    // Create a graphics buffer from loaded vertex data.
    // bufferFactory: creates GPU vertex buffers (API-agnostic)
    void CreateBuffer(IGraphicsBufferFactory* bufferFactory);

    // Get the underlying graphics buffer (API-agnostic)
    IGraphicsBuffer* GetGraphicsBuffer() const { return m_vertexBuffer.get(); }
    
    uint32_t GetVertexCount() const { return static_cast<uint32_t>(m_vertices.size()); }
    const std::vector<Vertex>& GetVertices() const { return m_vertices; }
    // Returns true only when CPU vertices, bounds, and the GPU buffer changed.
    bool SetDeformedVertices(const std::vector<Vertex>& vertices);
    uint32_t GetVertexStride() const { return sizeof(Vertex); }
    bool     IsReady()        const { return m_ready; }
    const std::string& GetFilePath() const { return m_filePath; }
    bool HasBounds() const { return m_hasBounds; }
    const glm::vec3& GetBoundsMin() const { return m_boundsMin; }
    const glm::vec3& GetBoundsMax() const { return m_boundsMax; }
    unsigned GetMorphNodeIndex() const { return m_morphNodeIndex; }
    void SetMorphData(unsigned nodeIndex, std::vector<MorphTarget> targets,
        std::vector<float> weights);
    bool SetMorphWeights(const std::vector<float>& weights);
    const std::vector<MorphTarget>& GetMorphTargets() const { return m_morphTargets; }
    const std::vector<float>& GetMorphWeights() const { return m_morphWeights; }
    std::vector<float>& GetMorphWeights() { return m_morphWeights; }
    uint64_t GetMorphWeightsRevision() const;
    bool HasMorphTargets() const { return !m_morphTargets.empty(); }

    using SliceResult = std::pair<std::vector<Vertex>, std::vector<Vertex>>;
    static SliceResult SliceByPlane(const std::vector<Vertex>& vertices,
        const glm::vec3& planePoint, const glm::vec3& planeNormal);

    bool        DrawProperties(::Engine::Editor::IEditorUi& ui) override;
    JsonValue   Serialize() const override;
    void        Deserialize(const JsonValue& v) override;
    void        DeserializeLegacyMorphTargets(const JsonValue& value);
    void        OnAfterDeserialize(IGraphicsProvider* graphicsProvider) override;

private:
    void ObserveMorphWeights() const;
    void AdvanceMorphWeightsRevision() const;
    void UpdateBounds();
    std::string m_filePath;
    std::vector<Vertex> m_vertices;
    std::unique_ptr<IGraphicsBuffer> m_vertexBuffer;
    bool m_ready = false;
    bool m_hasBounds = false;
    glm::vec3 m_boundsMin{};
    glm::vec3 m_boundsMax{};
    unsigned m_morphNodeIndex = 0;
    std::vector<MorphTarget> m_morphTargets;
    std::vector<float> m_morphWeights;
    mutable std::vector<float> m_observedMorphWeights;
    mutable uint64_t m_morphWeightsRevision = 1;
    mutable bool m_morphWeightsObserved = false;
};
}
