// Synthetic GPU load for hardware simulation (Core/Rendering/HardwareSimulation.h).
// Every thread runs a dependent arithmetic chain of `iterations` steps; the
// CPU calibrates the count from this pass's measured time. The result is
// written so the compiler cannot remove the work.
struct LoadConstants { uint iterations; uint3 padding; };
#ifdef VULKAN
[[vk::push_constant]] LoadConstants constants;
[[vk::binding(0, 0)]] StructuredBuffer<float4> seeds;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> sink;
#else
cbuffer Constants : register(b0) { LoadConstants constants; };
StructuredBuffer<float4> seeds : register(t0);
RWStructuredBuffer<float4> sink : register(u0);
#endif

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    float4 value = seeds[id.x & 63u] + float(id.x) * 1.0e-6;
    [loop] for (uint step = 0; step < constants.iterations; ++step)
        value = value * 0.9997 + sin(value.yzwx) * 0.0003;
    sink[id.x & 1023u] = value;
}
