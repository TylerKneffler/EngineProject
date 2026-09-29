#ifdef VULKAN
[[vk::binding(0, 0)]] Texture2D<float4> sceneColor;
[[vk::binding(1, 0)]] SamplerState sceneSampler;
struct PostProcessParameters { float exposure; uint toneMappingOperator; float2 padding; };
[[vk::push_constant]] PostProcessParameters parameters;
#define exposure parameters.exposure
#define toneMappingOperator parameters.toneMappingOperator
#else
Texture2D<float4> sceneColor : register(t0);
SamplerState sceneSampler : register(s0);
cbuffer PostProcessCB : register(b0)
{
    float exposure;
    uint toneMappingOperator;
    float2 padding;
};
#endif

struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) +
        float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float3 ToneMap(float3 color)
{
    color = max(color * max(exposure, 0.0), 0.0);
    if (toneMappingOperator == 0) return color;
    if (toneMappingOperator == 2) return color / (1.0 + color);
    return saturate((color * (2.51 * color + 0.03)) /
        (color * (2.43 * color + 0.59) + 0.14));
}

float3 LinearToSrgb(float3 color)
{
    color = max(color, 0.0);
    return lerp(color * 12.92,
        1.055 * pow(color, 1.0 / 2.4) - 0.055,
        step(0.0031308, color));
}

float4 PSMain(VertexOutput input) : SV_Target
{
    const float4 source = sceneColor.Sample(sceneSampler, input.uv);
    return float4(LinearToSrgb(ToneMap(source.rgb)), source.a);
}
