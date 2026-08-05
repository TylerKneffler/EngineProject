#pragma once
#include <memory>
#include <vector>
#include <cstdint>
#include "IShader.h"

// ---------------------------------------------------------------------------
// IPipelineState — Graphics pipeline state object
// 
// Encapsulates:
// - Shader programs (VS, PS, etc.)
// - Rasterizer state (fill mode, cull mode)
// - Blend state
// - Depth/stencil state
// - Root signature / descriptor layout
// - Render target formats
// ---------------------------------------------------------------------------
class IPipelineState
{
public:
    virtual ~IPipelineState() = default;
    virtual void* GetNativeHandle() const = 0;
};

// ---------------------------------------------------------------------------
// IPipelineStateBuilder — Fluent builder for pipeline state
// 
// Allows API-neutral pipeline construction:
//   builder->SetVertexShader(vs)
//           ->SetPixelShader(ps)
//           ->SetBlendEnable(true)
//           ->Build();
// ---------------------------------------------------------------------------
class IPipelineStateBuilder
{
public:
    virtual ~IPipelineStateBuilder() = default;

    // Shader stages
    virtual IPipelineStateBuilder& SetVertexShader(const IShader* shader) = 0;
    virtual IPipelineStateBuilder& SetPixelShader(const IShader* shader) = 0;

    // Rasterizer state
    virtual IPipelineStateBuilder& SetFillMode(bool wireframe) = 0;
    virtual IPipelineStateBuilder& SetCullMode(bool cullBackFaces) = 0;
    virtual IPipelineStateBuilder& SetFrontCounterClockwise(bool ccw) = 0;
    virtual IPipelineStateBuilder& SetDepthClipEnable(bool enable) = 0;

    // Blend state
    virtual IPipelineStateBuilder& SetBlendEnable(bool enable) = 0;
    virtual IPipelineStateBuilder& SetSrcBlend(int mode) = 0;  // D3D12_BLEND enum
    virtual IPipelineStateBuilder& SetDestBlend(int mode) = 0;
    virtual IPipelineStateBuilder& SetBlendOp(int op) = 0;     // D3D12_BLEND_OP enum
    virtual IPipelineStateBuilder& SetSrcBlendAlpha(int mode) = 0;
    virtual IPipelineStateBuilder& SetDestBlendAlpha(int mode) = 0;
    virtual IPipelineStateBuilder& SetBlendOpAlpha(int op) = 0;

    // Depth/stencil state
    virtual IPipelineStateBuilder& SetDepthEnable(bool enable) = 0;
    virtual IPipelineStateBuilder& SetDepthWriteEnable(bool enable) = 0;
    virtual IPipelineStateBuilder& SetDepthFunc(int func) = 0;  // D3D12_COMPARISON_FUNC enum

    // Input layout (vertex format)
    struct VertexElement
    {
        const char* semanticName;
        uint32_t semanticIndex;
        int format;              // DXGI_FORMAT enum
        uint32_t inputSlot;
        uint32_t alignedByteOffset;
        bool perInstance;
    };
    virtual IPipelineStateBuilder& SetInputLayout(
        const VertexElement* elements, uint32_t elementCount) = 0;

    // Primitive topology
    enum class PrimitiveTopology
    {
        PointList,
        LineList,
        LineStrip,
        TriangleList,
        TriangleStrip,
    };
    virtual IPipelineStateBuilder& SetPrimitiveTopology(PrimitiveTopology topology) = 0;

    // Render target formats
    virtual IPipelineStateBuilder& SetRenderTargetFormat(int format, int depthFormat = -1) = 0;

    // Build the final pipeline state
    // Returns nullptr on error (call GetLastError)
    virtual std::unique_ptr<IPipelineState> Build() = 0;
    virtual std::string GetLastError() const = 0;
};

// ---------------------------------------------------------------------------
// IPipelineStateFactory — Creates pipeline state builders
// ---------------------------------------------------------------------------
class IPipelineStateFactory
{
public:
    virtual ~IPipelineStateFactory() = default;
    virtual std::unique_ptr<IPipelineStateBuilder> CreateBuilder() = 0;
};
