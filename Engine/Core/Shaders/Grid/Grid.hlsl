// Grid.hlsl — infinite XZ grid via fullscreen triangle + ray-plane intersection.
// The VS emits a covering triangle from SV_VertexID (no vertex buffer).
// The PS unprojects each NDC pixel to find its Y=0 world position, then draws
// antialiased grid lines analytically and fades them by distance.

// Layout (128 bytes, fits Vulkan's 128-byte push-constant minimum):
//   float4x4 invVP       (64 b)
//   float3   cameraPos   (12 b)
//   float    cellSize    ( 4 b)
//   float4   gridColor   (16 b)
//   float4   axisColor   (16 b, alpha carries the 3D zoom level)
//   float    fadeDistance( 4 b)
//   float    nearPlane   ( 4 b)
//   float    farPlane    ( 4 b)
//   float    mode2D      ( 4 b)

#ifdef VULKAN
struct GridConst
{
    float4x4 invVP;
    float3   cameraPos;
    float    cellSize;
    float4   gridColor;
    float4   axisColor;
    float    fadeDistance;
    float    nearPlane;
    float    farPlane;
    float    mode2D;
};
[[vk::push_constant]] GridConst cb;
#define invVP        cb.invVP
#define cameraPos    cb.cameraPos
#define cellSize     cb.cellSize
#define gridColor    cb.gridColor
#define axisColor    cb.axisColor
#define fadeDistance cb.fadeDistance
#define nearPlane    cb.nearPlane
#define farPlane     cb.farPlane
#define mode2D       cb.mode2D
#else
cbuffer GridCB : register(b0)
{
    float4x4 invVP;
    float3   cameraPos;
    float    cellSize;
    float4   gridColor;    // rgb + opacity
    float4   axisColor;    // rgb + 3D zoom level
    float    fadeDistance;
    float    nearPlane;
    float    farPlane;
    float    mode2D;
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
struct GridPixel
{
    float4 color : SV_TARGET;
    float depth : SV_DEPTH;
};

float GridCoverage(float2 planePosition, float spacing)
{
    float2 coord = planePosition / spacing;
    float2 width = max(fwidth(coord), 0.0001);
    float2 lineDistance = abs(frac(coord - 0.5) - 0.5) / width;
    // Suppress a scale once its lines approach a pixel apart. This also
    // prevents the most distant grid from turning into a solid plane.
    float visibility = 1.0 - smoothstep(0.25, 0.5,
        max(width.x, width.y));
    return (1.0 - saturate(min(lineDistance.x, lineDistance.y))) * visibility;
}

GridPixel PSMain(float4 svPos : SV_POSITION,
                 float2 ndc   : TEXCOORD0)
{
    // Unproject to camera-relative space. The inverse matrix omits camera
    // translation so distant views retain precision near the horizon.
    float4 wNear = mul(invVP, float4(ndc, 0.0, 1.0));
    float4 wFar  = mul(invVP, float4(ndc, 1.0, 1.0));
    wNear /= wNear.w;
    wFar  /= wFar.w;

    float3 dir = wFar.xyz - wNear.xyz;

    // 3D uses the ground XZ plane. 2D uses the canvas XY plane.
    float denominator = mode2D > 0.5 ? dir.z : dir.y;
    if (abs(denominator) < 1e-5) discard;
    float planeOffset = mode2D > 0.5 ? cameraPos.z : cameraPos.y;
    float t = -(planeOffset +
        (mode2D > 0.5 ? wNear.z : wNear.y)) / denominator;
    if (t <= 0.0) discard;

    float3 wp = cameraPos + wNear.xyz + t * dir;

    // The 3D level is chosen once from camera height, not independently for
    // every pixel. Keep primary lines visible while the nine subdivisions
    // fade out; the next decade's subdivisions fade in as the camera moves.
    float2 planePosition = mode2D > 0.5 ? wp.xy : wp.xz;
    float2 cameraPlane = mode2D > 0.5 ? cameraPos.xy : cameraPos.xz;
    float baseSpacing = max(abs(cellSize), 0.000001);
    float lod = mode2D > 0.5 ? 0.0 : axisColor.a;
    float level = floor(lod);
    float blend = frac(lod);
    float fineSpacing = baseSpacing * pow(10.0, level);
    float coarseSpacing = fineSpacing * 10.0;
    float superSpacing = coarseSpacing * 10.0;
    float gridA;
    if (mode2D > 0.5)
    {
        gridA = GridCoverage(planePosition, baseSpacing);
    }
    else
    {
        float fineA = GridCoverage(planePosition, fineSpacing) *
            (0.35 * (1.0 - blend));
        float coarseA = GridCoverage(planePosition, coarseSpacing) *
            lerp(0.8, 0.35, blend);
        float superA = GridCoverage(planePosition, superSpacing) *
            (0.8 * blend);
        gridA = max(fineA, max(coarseA, superA));
    }

    // Keep the authored distance as the start of the fade, then let lines
    // disappear over three more times that distance. Scaling the range with
    // the grid level keeps the visible extent useful as the camera zooms out.
    float dist = length(planePosition - cameraPlane);
    float visibleSpacing = baseSpacing * pow(10.0, lod);
    float adaptiveFadeDistance = max(fadeDistance, 0.0001) *
        max(1.0, visibleSpacing / baseSpacing);
    float fade = 1.0 - smoothstep(adaptiveFadeDistance,
        adaptiveFadeDistance * 4.0, dist);

    float alpha = gridA * fade * gridColor.a;
    // Empty cells and fully faded lines must leave the scene color and alpha
    // untouched when the viewport is composited over the editor background.
    if (alpha <= 0.0) discard;

    // Highlight the world X (red) and Z (blue) axis lines.
    float2 axDeriv = fwidth(planePosition);
    float  xAxis   = clamp(1.0 - abs(planePosition.y) / (axDeriv.y * 2.0), 0.0, 1.0);
    float  zAxis   = clamp(1.0 - abs(planePosition.x) / (axDeriv.x * 2.0), 0.0, 1.0);
    float3 color   = lerp(gridColor.rgb, axisColor.rgb, saturate(xAxis + zAxis));

    // Write the depth of the reconstructed world-space plane rather than the
    // fullscreen triangle's far depth. This keeps grid/object intersections
    // stable while the editor camera changes distance or angle.
    float depthRange = max(farPlane - nearPlane, 1e-5);
    float viewDepth = nearPlane + t * depthRange;
    GridPixel output;
    output.color = float4(color, alpha);
    float planeDepth = saturate(farPlane * (viewDepth - nearPlane) /
        max(depthRange * viewDepth, 1e-5));
    // Put the helper just behind coplanar scene geometry. Its depth otherwise
    // rounds to alternating sides of a floor's depth in distant views.
    output.depth = mode2D > 0.5 ? 0.999 :
        saturate(planeDepth + max(fwidth(planeDepth), 0.0000001));
    return output;
}
