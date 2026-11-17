// Analytic exponential height fog ("aerial perspective") shared by the object
// shaders and the skybox. Parameters come from SceneSettings::atmosphereFog,
// packed on the CPU (SceneRendering.cpp / SceneRendererLighting.cpp).
//
// density(y) = densityAtBase * exp(-falloff * (y - baseHeight))
// T          = exp(-integral of density along the view ray)
// result     = color * T + inscatter * (1 - T)

#ifndef ATMOSPHERE_FOG_HLSLI
#define ATMOSPHERE_FOG_HLSLI

// Optical depth of a ray of length `rayLength` starting where the density is
// `densityAtStart`, travelling with vertical direction component `dirY`.
float AtmosphereOpticalDepth(float densityAtStart, float falloff, float dirY,
    float rayLength)
{
    // Integral of exp(-falloff * dirY * t) over [0, rayLength], divided by
    // rayLength. Clamped so long downward rays cannot overflow.
    const float k = clamp(falloff * dirY * rayLength, -60.0, 60.0);
    const float shape = abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0 - 0.5 * k;
    return densityAtStart * rayLength * shape;
}

// Henyey-Greenstein phase scaled by 4*pi, so isotropic scattering is 1.
float AtmosphereHgPhase(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denominator = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    return (1.0 - g2) / (denominator * sqrt(denominator));
}

float3 AtmosphereInscatterColor(float3 viewDirection, float3 sunDirection,
    float3 ambientInscatter, float3 sunGlow, float anisotropy)
{
    const float cosTheta = dot(viewDirection, sunDirection);
    return ambientInscatter + sunGlow * AtmosphereHgPhase(cosTheta, anisotropy);
}

// fog0: density at base height, height falloff, base height, enabled
// fog1: ambient inscatter rgb, start distance
// fog2: sun direction (toward the sun) xyz, anisotropy g
// fog3: sun glow rgb, max opacity
float3 ApplyAtmosphereFog(float3 color, float3 worldPos, float3 cameraPos,
    float4 fog0, float4 fog1, float4 fog2, float4 fog3)
{
    [branch] if (fog0.w < 0.5)
        return color;
    const float3 toSurface = worldPos - cameraPos;
    const float distanceToSurface = length(toSurface);
    const float rayLength = max(distanceToSurface - fog1.w, 0.0);
    [branch] if (rayLength <= 0.0)
        return color;
    const float3 viewDirection = toSurface / max(distanceToSurface, 1e-4);
    // The fogged segment begins at startDistance along the ray.
    const float startHeight = cameraPos.y + viewDirection.y * fog1.w;
    const float densityAtStart = fog0.x *
        exp(clamp(-fog0.y * (startHeight - fog0.z), -60.0, 60.0));
    const float opticalDepth = AtmosphereOpticalDepth(densityAtStart, fog0.y,
        viewDirection.y, rayLength);
    const float opacity = min(1.0 - exp(-opticalDepth), fog3.w);
    const float3 inscatter = AtmosphereInscatterColor(viewDirection,
        fog2.xyz, fog1.rgb, fog3.rgb, fog2.w);
    return lerp(color, inscatter, opacity);
}

#endif
