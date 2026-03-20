#pragma once
#include "Core/component.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

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

    // Create a persistently-mapped upload-heap vertex buffer and copy CPU data into it.
    // Call after LoadFromFile. Safe to call before the first render.
    void CreateBuffer(ID3D12Device* device);

    const D3D12_VERTEX_BUFFER_VIEW& GetVertexBufferView() const { return m_vbView; }
    uint32_t                        GetVertexCount()      const { return static_cast<uint32_t>(m_vertices.size()); }
    bool                            IsReady()             const { return m_ready; }

private:
    std::vector<Vertex>                    m_vertices;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW               m_vbView{};
    bool                                   m_ready = false;
};
