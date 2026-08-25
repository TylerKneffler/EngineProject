#ifdef VULKAN
[[vk::binding(0, 0)]] Texture2D fontAtlas;
[[vk::binding(6, 0)]] SamplerState fontSampler;
#else
Texture2D fontAtlas : register(t0);
SamplerState fontSampler : register(s0);
#endif

struct VSInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 PSImage(PSInput input) : SV_TARGET
{
    return fontAtlas.Sample(fontSampler, input.uv) * input.color;
}

// Offline Vulkan shader packaging expects the conventional entry point.
float4 PSMain(PSInput input) : SV_TARGET
{
    return PSImage(input);
}
