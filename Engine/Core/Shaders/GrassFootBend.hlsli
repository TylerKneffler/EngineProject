// Capsule proximity steers the existing wind pose toward a root-pinned bend.
// Blend between two above-ground poses instead of adding vertical displacements.
void ApplyGrassFootBend(float4 capsules[8], uint count, float3 root, float3 up,
    float height, float3 restPosition, float3 restNormal, float3 restTangent,
    inout float3 position, inout float3 normal, inout float3 tangent)
{
    if (height <= 0.0 || count == 0) return;
    float3 bend = 0.0;
    [loop] for (uint i = 0; i < min(count, 4u); ++i)
    {
        float4 start = capsules[i * 2];
        float4 end = capsules[i * 2 + 1];
        float3 segment = end.xyz - start.xyz;
        float t = saturate(dot(root - start.xyz, segment) / max(dot(segment, segment), .000001));
        float3 delta = root - (start.xyz + segment * t);
        float outside = max(0.0, length(delta) - start.w);
        if (outside >= end.w) continue;
        float weight = 1.0 - smoothstep(0.0, max(.01, end.w), outside);
        float3 away = delta - up * dot(delta, up);
        if (dot(away, away) < .000001)
        {
            float3 reference = abs(up.x) < .9 ? float3(1, 0, 0) : float3(0, 0, 1);
            away = reference - up * dot(reference, up);
        }
        bend += normalize(away) * weight;
    }
    float magnitude = length(bend);
    if (magnitude < .0001) return;
    float3 direction = bend / magnitude;
    float influence = saturate(magnitude);
    float stiffness = .18;
    float maximumAngle = radians(70.0);
    float angle = maximumAngle * height / (height + stiffness);
    float derivative = maximumAngle * stiffness / ((height + stiffness) * (height + stiffness));
    float sine, cosine;
    sincos(angle, sine, cosine);
    float3 target = restPosition + height * (direction * sine + up * (cosine - 1.0));
    float3 slope = direction * (sine + height * cosine * derivative) +
        up * (cosine - 1.0 - height * sine * derivative);
    float3 targetNormal = normalize(restNormal - up *
        (dot(slope, restNormal) / max(.05, 1.0 + dot(slope, up))));
    position = lerp(position, target, influence);
    normal = normalize(lerp(normal, targetNormal, influence));
    if (dot(restTangent, restTangent) > .000001)
        tangent = normalize(lerp(tangent, normalize(restTangent + slope * dot(up, restTangent)), influence));
}
