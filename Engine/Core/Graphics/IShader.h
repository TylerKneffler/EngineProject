#pragma once
#include <memory>
#include <vector>

// ---------------------------------------------------------------------------
// IShader — Opaque shader bytecode handle
// 
// Represents compiled shader bytecode in a graphics-API-neutral way.
// The actual data is managed by the implementing graphics backend.
// ---------------------------------------------------------------------------
class IShader
{
public:
    virtual ~IShader() = default;

    // Get the raw bytecode buffer for this shader
    virtual const void* GetBytecode() const = 0;
    
    // Get the size of the bytecode in bytes
    virtual size_t GetBytecodeSize() const = 0;
};

// ---------------------------------------------------------------------------
// IShaderCompiler — Compiles shaders to bytecode
// 
// Different renderers have different compilation pipelines:
// - D3D12: D3DCompileFromFile → D3DBlob
// - Vulkan: glslc, shaderc, or other tools
// - Metal: Metal compiler
// ---------------------------------------------------------------------------
class IShaderCompiler
{
public:
    virtual ~IShaderCompiler() = default;

    enum class ShaderType
    {
        Vertex,
        Pixel,      // D3D term; equivalent to Fragment in other APIs
        Compute,
    };

    enum class CompileProfile
    {
        VS_5_0,
        PS_5_0,
        CS_5_0,
    };

    // Compile shader from file path
    // Returns nullptr on compilation failure (check GetLastError)
    virtual std::unique_ptr<IShader> CompileFromFile(
        const std::string& filePath,
        const char* entryPoint,
        CompileProfile profile) = 0;

    // Get the last compilation error message
    virtual std::string GetLastError() const = 0;
};
