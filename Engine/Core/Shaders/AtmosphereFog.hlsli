// Aerial perspective shared by the object, particle and outline shaders and
// the skybox, in one of two modes selected by fog0.w:
//
// Height fog (SceneSettings::atmosphereFog, ViewLighting::PackAtmosphereFog):
// density(y) = densityAtBase * exp(-falloff * (y - baseHeight))
// T          = exp(-integral of density along the view ray)
// result     = color * T + inscatter * (1 - T)
//
// Physical (SceneSettings::physicalSky, ViewLighting::PackPhysicalAerialPerspective,
// fog0.w >= 100): the physical sky's atmosphere between camera and surface.
// Rayleigh (per-channel) and Mie extinction, each with its own exponential
// height profile, integrated analytically along the ray; single scattering
// of the sun (Rayleigh and Henyey-Greenstein phases, sun illuminance at the
// camera) plus Hillaire's multiple-scattering term:
// result     = color * T + (S_sun + S_ms) * (1 - T) / tau
// where S are scattering optical depths times their sources and tau the
// extinction optical depth (exact for a homogeneous medium). This is the
// analytic counterpart of Hillaire's aerial-perspective volume, valid for a
// camera in the lower atmosphere.
//
// Light shafts: the sun share of the in-scatter is weighted by how much of
// the fogged ray the sun actually reaches. Shaders that can see the cascade
// shadow map (Object.hlsl: ObjectFogSunVisibility) march it along the ray and
// pass the opacity-weighted visibility to ApplyAtmosphereFogShadowed.

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

// Height fog:
// fog0: density at base height, height falloff, base height,
//       w = 0 disabled, 1 enabled; w >= 2 also requests light shafts:
//       floor(w) - 1 shadow samples, frac(w) / 0.999 shaft strength
// fog1: ambient inscatter rgb, start distance
// fog2: sun direction (toward the sun) xyz, anisotropy g
// fog3: sun glow rgb, max opacity
// Physical (w = 100 + the same shaft encoding minus 1):
// fog0: Mie extinction at the ground (per m), 1 / Mie scale height,
//       ground height, w
// fog1: multiple-scattering source rgb, 1 / Rayleigh scale height
// fog2: sun direction xyz, Mie g
// fog3: sun illuminance at the camera rgb, Rayleigh scale

static const float3 AtmosphereRayleighScattering = float3(5.802e-6, 13.558e-6, 33.1e-6);
static const float AtmosphereMieAlbedo = 3.996 / 4.40;
static const float3 AtmosphereLuminance = float3(0.2126, 0.7152, 0.0722);

bool AtmosphereIsPhysical(float4 fog0)
{
    return fog0.w >= 99.5;
}

// The height-fog w encoding, for either mode.
float AtmosphereModeW(float4 fog0)
{
    return AtmosphereIsPhysical(fog0) ? fog0.w - 99.0 : fog0.w;
}

uint AtmosphereShaftSampleCount(float4 fog0)
{
    const float w = AtmosphereModeW(fog0);
    return w >= 1.5 ? (uint)floor(w) - 1u : 0u;
}

float AtmosphereShaftStrength(float4 fog0)
{
    const float w = AtmosphereModeW(fog0);
    return w >= 1.5 ? saturate(frac(w) / 0.999) : 0.0;
}

// Distance from the camera where the fogged segment starts.
float AtmosphereStartDistance(float4 fog0, float4 fog1)
{
    return AtmosphereIsPhysical(fog0) ? 0.0 : fog1.w;
}

// Physical mode: Rayleigh and Mie density-weighted path lengths (metres of
// ground-level air) over `rayLength` from the camera.
float2 AtmospherePhysicalPaths(float3 cameraPos, float3 viewDirection,
    float rayLength, float4 fog0, float4 fog1)
{
    const float altitude = max(cameraPos.y - fog0.z, 0.0);
    const float rayleigh = AtmosphereOpticalDepth(exp(-fog1.w * altitude), fog1.w,
        viewDirection.y, rayLength);
    const float mie = AtmosphereOpticalDepth(exp(-fog0.y * altitude), fog0.y,
        viewDirection.y, rayLength);
    return float2(rayleigh, mie);
}

float3 AtmospherePhysicalOpticalDepth(float2 paths, float4 fog0, float4 fog3)
{
    return AtmosphereRayleighScattering * fog3.w * paths.x + fog0.x * paths.y;
}

// Scalar optical depth over `rayLength` past the start distance, for
// weighting (light-shaft segments, additive particles). Physical mode uses
// the luminance of the per-channel depth.
float AtmosphereScalarOpticalDepth(float3 cameraPos, float3 viewDirection,
    float rayLength, float4 fog0, float4 fog1, float4 fog3)
{
    [branch] if (AtmosphereIsPhysical(fog0))
        return dot(AtmospherePhysicalOpticalDepth(AtmospherePhysicalPaths(cameraPos,
            viewDirection, rayLength, fog0, fog1), fog0, fog3), AtmosphereLuminance);
    const float startHeight = cameraPos.y + viewDirection.y * fog1.w;
    const float densityAtStart = fog0.x * exp(clamp(-fog0.y * (startHeight - fog0.z), -60.0, 60.0));
    return AtmosphereOpticalDepth(densityAtStart, fog0.y, viewDirection.y, rayLength);
}

// Physical aerial perspective over `rayLength` from the camera.
// sunShare scales the sun's single scattering (light shafts).
float3 ApplyPhysicalAerialPerspective(float3 color, float3 cameraPos,
    float3 viewDirection, float rayLength, float4 fog0, float4 fog1, float4 fog2,
    float4 fog3, float sunShare)
{
    const float2 paths = AtmospherePhysicalPaths(cameraPos, viewDirection, rayLength,
        fog0, fog1);
    const float3 rayleighScattering = AtmosphereRayleighScattering * fog3.w * paths.x;
    const float mieScattering = fog0.x * AtmosphereMieAlbedo * paths.y;
    const float3 opticalDepth = AtmospherePhysicalOpticalDepth(paths, fog0, fog3);
    const float3 transmittance = exp(-opticalDepth);
    const float cosTheta = dot(viewDirection, fog2.xyz);
    const float rayleighPhase = 0.0596831 * (1.0 + cosTheta * cosTheta); // 3 / (16 pi)
    const float miePhase = AtmosphereHgPhase(cosTheta, fog2.w) * 0.0795775; // / (4 pi)
    const float3 source = (rayleighScattering * rayleighPhase + mieScattering * miePhase) *
        fog3.rgb * sunShare + (rayleighScattering + mieScattering) * fog1.rgb;
    // Each unit of scattering is seen through, on average, the ray's
    // transmittance up to it: (1 - T) / tau (homogeneous-medium exact).
    const float3 seen = (1.0 - transmittance) / max(opticalDepth, 1e-6);
    return color * transmittance + source * seen;
}

// Fog density where the fogged segment of the ray begins (startDistance
// along viewDirection from the camera).
float AtmosphereDensityAtStart(float3 cameraPos, float3 viewDirection,
    float4 fog0, float4 fog1)
{
    const float startHeight = cameraPos.y + viewDirection.y * fog1.w;
    return fog0.x * exp(clamp(-fog0.y * (startHeight - fog0.z), -60.0, 60.0));
}

// sunVisibility: fraction of the fog's opacity that the sun lights, in
// [0, 1]; 1 reproduces the unshadowed fog.
float3 ApplyAtmosphereFogShadowed(float3 color, float3 worldPos,
    float3 cameraPos, float4 fog0, float4 fog1, float4 fog2, float4 fog3,
    float sunVisibility)
{
    [branch] if (fog0.w < 0.5)
        return color;
    const float3 toSurface = worldPos - cameraPos;
    const float distanceToSurface = length(toSurface);
    const float rayLength = max(distanceToSurface - AtmosphereStartDistance(fog0, fog1), 0.0);
    [branch] if (rayLength <= 0.0)
        return color;
    const float3 viewDirection = toSurface / max(distanceToSurface, 1e-4);
    [branch] if (AtmosphereIsPhysical(fog0))
        return ApplyPhysicalAerialPerspective(color, cameraPos, viewDirection, rayLength,
            fog0, fog1, fog2, fog3, lerp(1.0, saturate(sunVisibility),
                AtmosphereShaftStrength(fog0)));
    const float densityAtStart = AtmosphereDensityAtStart(cameraPos,
        viewDirection, fog0, fog1);
    const float opticalDepth = AtmosphereOpticalDepth(densityAtStart, fog0.y,
        viewDirection.y, rayLength);
    const float opacity = min(1.0 - exp(-opticalDepth), fog3.w);
    const float sunShare = lerp(1.0, saturate(sunVisibility),
        AtmosphereShaftStrength(fog0));
    const float3 inscatter = AtmosphereInscatterColor(viewDirection,
        fog2.xyz, fog1.rgb, fog3.rgb * sunShare, fog2.w);
    return lerp(color, inscatter, opacity);
}

float3 ApplyAtmosphereFog(float3 color, float3 worldPos, float3 cameraPos,
    float4 fog0, float4 fog1, float4 fog2, float4 fog3)
{
    return ApplyAtmosphereFogShadowed(color, worldPos, cameraPos,
        fog0, fog1, fog2, fog3, 1.0);
}

// Fraction of light from worldPos that reaches the camera (1 = no fog). For
// additive blends, which must fade into the fog rather than take its color:
// the fog's own in-scatter is already in the surface behind them.
float AtmosphereFogTransmittance(float3 worldPos, float3 cameraPos,
    float4 fog0, float4 fog1, float4 fog3)
{
    [branch] if (fog0.w < 0.5)
        return 1.0;
    const float3 toSurface = worldPos - cameraPos;
    const float distanceToSurface = length(toSurface);
    const float rayLength = max(distanceToSurface - AtmosphereStartDistance(fog0, fog1), 0.0);
    [branch] if (rayLength <= 0.0)
        return 1.0;
    const float3 viewDirection = toSurface / max(distanceToSurface, 1e-4);
    [branch] if (AtmosphereIsPhysical(fog0))
        return exp(-AtmosphereScalarOpticalDepth(cameraPos, viewDirection, rayLength,
            fog0, fog1, fog3));
    const float opticalDepth = AtmosphereOpticalDepth(
        AtmosphereDensityAtStart(cameraPos, viewDirection, fog0, fog1),
        fog0.y, viewDirection.y, rayLength);
    return 1.0 - min(1.0 - exp(-opticalDepth), fog3.w);
}

#endif
