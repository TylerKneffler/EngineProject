float4 SkinQuatMultiply(float4 a, float4 b)
{
    return float4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz),
        a.w * b.w - dot(a.xyz, b.xyz));
}

float3 SkinQuatRotate(float4 q, float3 v)
{
    float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

float4 SkinMatrixQuat(float4x4 m)
{
    float4 q;
    float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0)
    {
        float s = sqrt(trace + 1.0) * 2.0;
        q = float4((m[2][1] - m[1][2]) / s,
            (m[0][2] - m[2][0]) / s,
            (m[1][0] - m[0][1]) / s, 0.25 * s);
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        float s = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
        q = float4(0.25 * s, (m[0][1] + m[1][0]) / s,
            (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s);
    }
    else if (m[1][1] > m[2][2])
    {
        float s = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
        q = float4((m[0][1] + m[1][0]) / s, 0.25 * s,
            (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s);
    }
    else
    {
        float s = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
        q = float4((m[0][2] + m[2][0]) / s,
            (m[1][2] + m[2][1]) / s, 0.25 * s,
            (m[1][0] - m[0][1]) / s);
    }
    return normalize(q);
}

void SkinDeform(inout float4 position, inout float3 normal,
    inout float3 tangent, float4 joints0, float4 weights0,
    float4 joints1, float4 weights1, float4 centerAndMode,
    float4 radius0, float4 radius1, uint paletteOffset, uint jointCount)
{
    uint mode = (uint)centerAndMode.w;
    if (mode == 3u && (uint)joints0.x < jointCount &&
        (uint)joints0.y < jointCount)
    {
        float4x4 bone0 = boneMatrices[paletteOffset + (uint)joints0.x];
        float4x4 bone1 = boneMatrices[paletteOffset + (uint)joints0.y];
        float weight = weights0.x;
        float4 q0 = SkinMatrixQuat(bone0);
        float4 q1 = SkinMatrixQuat(bone1);
        if (dot(q0, q1) < 0.0) q1 = -q1;
        float cosine = clamp(dot(q1, q0), -1.0, 1.0);
        float4 rotation;
        if (cosine > 0.9995)
            rotation = normalize(lerp(q1, q0, weight));
        else
        {
            float angle = acos(cosine);
            rotation = (sin((1.0 - weight) * angle) * q1 +
                sin(weight * angle) * q0) / sin(angle);
        }
        float3 center = centerAndMode.xyz;
        position.xyz = weight * mul(bone0,
            float4(position.xyz - center + radius0.xyz, 1.0)).xyz +
            (1.0 - weight) * mul(bone1,
                float4(position.xyz - center + radius1.xyz, 1.0)).xyz +
            SkinQuatRotate(rotation, center - weight * radius0.xyz -
                (1.0 - weight) * radius1.xyz);
        normal = SkinQuatRotate(rotation, normal);
        tangent = SkinQuatRotate(rotation, tangent);
        return;
    }
    if (mode == 4u)
    {
        float4 real = 0.0, dual = 0.0, reference = float4(0, 0, 0, 1);
        bool hasReference = false;
        [unroll] for (uint influence = 0; influence < 4; ++influence)
        {
            uint joint = (uint)joints0[influence];
            float weight = weights0[influence];
            if (joint >= jointCount || weight == 0.0) continue;
            float4x4 bone = boneMatrices[paletteOffset + joint];
            float4 rotation = SkinMatrixQuat(bone);
            if (!hasReference) { reference = rotation; hasReference = true; }
            if (dot(reference, rotation) < 0.0) rotation = -rotation;
            float3 translation = float3(bone[0][3], bone[1][3], bone[2][3]);
            real += weight * rotation;
            dual += weight * 0.5 * SkinQuatMultiply(
                float4(translation, 0.0), rotation);
        }
        float magnitude = length(real);
        if (magnitude > 1e-7)
        {
            real /= magnitude;
            dual /= magnitude;
            float3 translation = 2.0 * SkinQuatMultiply(dual,
                float4(-real.xyz, real.w)).xyz;
            position.xyz = SkinQuatRotate(real, position.xyz) + translation;
            normal = SkinQuatRotate(real, normal);
            tangent = SkinQuatRotate(real, tangent);
            return;
        }
    }
    float4x4 skin = (float4x4)0;
    [unroll] for (uint influence = 0; influence < 4; ++influence)
    {
        uint joint0 = (uint)joints0[influence];
        uint joint1 = (uint)joints1[influence];
        if (joint0 < jointCount) skin += boneMatrices[paletteOffset + joint0] * weights0[influence];
        if (joint1 < jointCount) skin += boneMatrices[paletteOffset + joint1] * weights1[influence];
    }
    position = mul(skin, position);
    normal = mul((float3x3)skin, normal);
    tangent = mul((float3x3)skin, tangent);
}
