// Grid.hlsl — infinite XZ grid via fullscreen triangle + ray-plane intersection.
// The VS emits a covering triangle from SV_VertexID (no vertex buffer).
// The PS unprojects each NDC pixel to find its Y=0 world position, then draws
// antialiased grid lines analytically and fades them by distance.

// Layout (128 bytes, fits Vulkan's 128-byte push-constant minimum):
//   float4x4 invVP       (64 b)
//   float3   cameraPos   (12 b)
//   float    cellSize    ( 4 b)
//   float4   gridColor   (16 b)
//   float4   axisColor   (16 b)
//   float    fadeDistance( 4 b)
//   float3   _pad        (12 b)

#ifdef VULKAN
struct GridConst
{
    float4x4 invVP;
    float3   cameraPos;
    float    cellSize;
    float4   gridColor;
    float4   axisColor;
    float    fadeDistance;
    float3   _pad;
};
[[vk::push_constant]] GridConst cb;
#define invVP        cb.invVP
#define cameraPos    cb.cameraPos
#define cellSize     cb.cellSize
#define gridColor    cb.gridColor
#define axisColor    cb.axisColor
#define fadeDistance cb.fadeDistance
#else
cbuffer GridCB : register(b0)
{
    float4x4 invVP;
    float3   cameraPos;
    float    cellSize;
    float4   gridColor;    // rgb + opacity
    float4   axisColor;    // rgb + 1
    float    fadeDistance;
    float3   _pad;
};
#endif

// ---------------------------------------------------------------------------
// Vertex shader
// ---------------------------------------------------------------------------
void VSMain(uint id      : SV_VertexID,
            out float4 oPos : SV_POSITION,
            out float2 oNDC : TEXCOORD0)
{
    // Compute fullscreen triangle corners arithmetically — avoids dynamic
    // array indexing which FXC rejects in vertex shaders.
    oNDC.x = (id == 1) ? 3.0 : -1.0;
    oNDC.y = (id == 2) ? 3.0 : -1.0;
    oPos = float4(oNDC, 1.0, 1.0);
}

// ---------------------------------------------------------------------------
// Pixel shader
// ---------------------------------------------------------------------------
float4 PSMain(float4 svPos : SV_POSITION,
              float2 ndc   : TEXCOORD0) : SV_TARGET
{
    // Unproject NDC to world space via the inverse view-projection matrix.
    float4 wNear = mul(invVP, float4(ndc, 0.0, 1.0));
    float4 wFar  = mul(invVP, float4(ndc, 1.0, 1.0));
    wNear /= wNear.w;
    wFar  /= wFar.w;

    float3 dir = wFar.xyz - wNear.xyz;

    // Intersect the view ray with the Y=0 (XZ) plane.
    if (abs(dir.y) < 1e-5) discard;
    float t = -wNear.y / dir.y;
    if (t <= 0.0) discard;

    float3 wp = wNear.xyz + t * dir;

    // Antialiased grid lines using screen-space derivatives.
    float2 coord = wp.xz / cellSize;
    float2 deriv = fwidth(coord);
    float2 g     = abs(frac(coord - 0.5) - 0.5) / max(deriv, 0.0001);
    float  gridLine = min(g.x, g.y);
    float  gridA = 1.0 - clamp(gridLine, 0.0, 1.0);

    // Quadratic distance fade.
    float dist = length(wp.xz - cameraPos.xz);
    float fade = clamp(1.0 - dist / fadeDistance, 0.0, 1.0);
    fade = fade * fade;

    float alpha = gridA * fade * gridColor.a;
    if (alpha < 0.005) discard;

    // Highlight the world X (red) and Z (blue) axis lines.
    float2 axDeriv = fwidth(wp.xz);
    float  xAxis   = clamp(1.0 - abs(wp.z) / (axDeriv.y * 2.0), 0.0, 1.0);
    float  zAxis   = clamp(1.0 - abs(wp.x) / (axDeriv.x * 2.0), 0.0, 1.0);
    float3 color   = lerp(gridColor.rgb, axisColor.rgb, saturate(xAxis + zAxis));

    return float4(color, alpha);
}
