#pragma once
#include "Core/component.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include <string>
#include <vector>
#include <memory>

struct Vertex
{
    float pos[3];
    float normal[3];
};

class Mesh : public Component
{
public:
    Mesh() = default;
    ~Mesh() = default;

    // Load vertex data from a triangulated OBJ file (CPU side only).
    void LoadFromFile(const std::string& path);

    // Create a graphics buffer from loaded vertex data.
    // bufferFactory: creates GPU vertex buffers (API-agnostic)
    void CreateBuffer(IGraphicsBufferFactory* bufferFactory);

    // Get the underlying graphics buffer (API-agnostic)
    IGraphicsBuffer* GetGraphicsBuffer() const { return m_vertexBuffer.get(); }
    
    uint32_t GetVertexCount() const { return static_cast<uint32_t>(m_vertices.size()); }
    uint32_t GetVertexStride() const { return sizeof(Vertex); }
    bool     IsReady()        const { return m_ready; }
    const std::string& GetFilePath() const { return m_filePath; }

    // Serialization
    std::string GetTypeName() const override { return "Mesh"; }
    JsonValue   Serialize()   const override;
    void        Deserialize(const JsonValue& v) override;

private:
    std::string m_filePath;
    std::vector<Vertex> m_vertices;
    std::unique_ptr<IGraphicsBuffer> m_vertexBuffer;
    bool m_ready = false;
};
