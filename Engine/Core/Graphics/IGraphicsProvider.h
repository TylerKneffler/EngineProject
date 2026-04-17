#pragma once
#include <memory>
#include "IShader.h"
#include "IGraphicsBuffer.h"
#include "IPipelineState.h"
#include "IGraphicsContext.h"

// ---------------------------------------------------------------------------
// IGraphicsProvider — Access point for all graphics abstractions
// 
// The renderer implements this interface to provide graphics services
// to the engine. Scene and other engine classes request services through
// a GraphicsProvider rather than calling D3D12 directly.
// ---------------------------------------------------------------------------
class IGraphicsProvider
{
public:
    virtual ~IGraphicsProvider() = default;

    // Access shader compiler for the current graphics API
    virtual IShaderCompiler* GetShaderCompiler() = 0;

    // Access buffer factory
    virtual IGraphicsBufferFactory* GetBufferFactory() = 0;

    // Access pipeline state factory
    virtual IPipelineStateFactory* GetPipelineStateFactory() = 0;

    // Access graphics context factory
    virtual IGraphicsContextFactory* GetContextFactory() = 0;
};
