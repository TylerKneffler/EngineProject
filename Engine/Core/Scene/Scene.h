#pragma once
#include <DirectXMath.h>
#include "Core/Object.h"

// ---------------------------------------------------------------------------
// Scene
//
// Owns all game objects for one scene and manages scene-wide rendering
// helpers such as the editor grid.  Only the editor uses Init()/Render();
// the game runtime will call Start()/Update() instead.
//
// ---- Typical editor usage ----
//
//   scene.Init(device);
//
//   // inside SceneView drawFn:
//   scene.Render(cmd, view, proj);
// ---------------------------------------------------------------------------

struct SceneSettings
{
    // Grid
    bool  showGrid       = true;
    int   gridHalfSize   = 10;          // lines extend ±gridHalfSize on X and Z
    float gridCellSize   = 1.f;         // spacing between lines (world units)
    float gridOpacity    = 0.4f;        // 0 = invisible, 1 = fully opaque
    DirectX::XMFLOAT3 gridColor        = { 0.45f, 0.45f, 0.45f };
    DirectX::XMFLOAT3 gridOriginColor  = { 0.30f, 0.50f, 0.80f }; // X/Z axis lines

    // Ambient light
    DirectX::XMFLOAT3 ambientColor = { 0.12f, 0.12f, 0.12f };
};

class Scene
{
public:
    Scene()  = default;
    ~Scene() = default;

    // --- Editor Settings ---
    Object editorCamera; // not used by the game runtime, used for editor scene view navigation

    // Build GPU resources (grid vertex buffer, PSO, root sig).
    // Must be called once before the first Render() call.
    void Init(ID3D12Device* device);

    // Draw all objects and scene helpers (grid) using the supplied VP matrices.
    // Call inside the SceneView drawFn.
    void Render(ID3D12GraphicsCommandList* cmd, float aspect);
    void SetSelectedObject(Object* obj) { m_selectedObject = obj; }

    // Object management
    Object* AddObject();                     // create an empty Object owned by this scene
    Object* AddObject(const std::string& name);
    void    RemoveObject(Object* obj);
    const std::vector<std::unique_ptr<Object>>& GetObjects() const { return m_objects; }

    SceneSettings settings;

private:
    // ---- Object list ----
    std::vector<std::unique_ptr<Object>> m_objects;

    // ---- Grid resources ----
    void BuildGridBuffers(ID3D12Device* device);
    void BuildGridPipeline(ID3D12Device* device);

    struct GridVertex { float x, y, z, r, g, b, a; };

    Microsoft::WRL::ComPtr<ID3D12Resource>      m_gridVB;
    D3D12_VERTEX_BUFFER_VIEW                    m_gridVBV{};
    uint32_t                                    m_gridVertexCount = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_gridRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_gridPSO;

    // Shared 256-byte constant buffer: just a float4x4 MVP.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_gridCB;
    void*                                  m_gridCBMapped = nullptr;

    // ---- Object rendering resources ----
    void BuildObjectPipeline(ID3D12Device* device);

    static constexpr UINT kMaxObjects = 64;
    static constexpr UINT kCBStride   = 256;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_objectRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_objectPSO;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_objectCB;
    void*                                       m_objectCBMapped = nullptr;

    Object* m_selectedObject = nullptr;

    ID3D12Device* m_device = nullptr; // non-owning, valid for the lifetime of the scene
};
