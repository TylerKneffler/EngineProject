#pragma once

// Every depth buffer the engine renders (camera views, portal views and shadow
// maps) uses reverse-Z: the near plane maps to depth 1, the far plane to 0,
// buffers clear to 0 and nearer surfaces pass with GREATER. With a D32F buffer
// this keeps depth steps roughly proportional to distance instead of
// collapsing beyond a few kilometres (forward Z with near 0.06 m had ~25 m
// steps at 5 km).
//
// Projections are built with near and far swapped (in double where the range
// is large), never converted from a forward matrix in float: 1 - f/(f - n)
// cancels catastrophically in single precision. Camera::GetProjectionMatrix
// stays forward-Z for editor picking and screen math that never reaches a
// depth buffer; Camera::GetRenderProjectionMatrix is the reversed one.
//
// Comparison codes are the IPipelineStateBuilder ones (VkCompareOp order).
namespace Engine::Graphics::Depth
{
inline constexpr float Near = 1.f;
inline constexpr float Far = 0.f;
inline constexpr float ClearValue = Far;

inline constexpr int CompareNever = 0;
inline constexpr int CompareEqual = 2;
inline constexpr int CompareAlways = 7;
// A fragment closer to the camera than the stored depth.
inline constexpr int CompareNearer = 4;          // GREATER
inline constexpr int CompareNearerOrEqual = 6;   // GREATER_EQUAL

// Depth (z / w) one step farther than `depth`, for helpers that sit just
// behind coplanar geometry.
inline constexpr float PushFarther(float depth, float amount)
{
    return depth - amount;
}
}
