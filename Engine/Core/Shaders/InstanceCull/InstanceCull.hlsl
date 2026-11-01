struct InstanceRecord { float4 sphere; uint4 meta; float4 lod; };
struct OutputRecord { uint4 row0; uint4 row1; uint4 row2; };
struct CullConstants
{
    float4x4 viewProjection;
    float4 cameraPosition;
    uint4 counts; // instance count, vertex/index count, indexed, reset phase
    int4 selection; // target LOD (-1 means no LOD filter), reserved
};
#ifdef VULKAN
[[vk::push_constant]] CullConstants constants;
[[vk::binding(0, 0)]] StructuredBuffer<InstanceRecord> inputInstances;
[[vk::binding(1, 0)]] RWStructuredBuffer<OutputRecord> outputInstances;
#else
cbuffer Constants : register(b0) { CullConstants constants; };
StructuredBuffer<InstanceRecord> inputInstances : register(t0);
RWStructuredBuffer<OutputRecord> outputInstances : register(u0);
#endif
[numthreads(64,1,1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (constants.counts.w != 0)
    {
        if (id.x == 0)
        {
            OutputRecord args;
            args.row0 = uint4(constants.counts.y,0,constants.selection.y == 1 ? constants.counts.z : 0,0);
            args.row1 = 0; args.row2 = 0;
            outputInstances[0] = args;
        }
        return;
    }
    if (id.x >= constants.counts.x) return;
    const bool explicitMesh = constants.selection.y == 1;
    InstanceRecord instance = inputInstances[explicitMesh ? id.x * 2 + 1 : id.x];
    float4 center = float4(instance.sphere.xyz,1);
    float4 rows[4];
    [unroll] for (uint row=0;row<4;++row)
        rows[row] = constants.viewProjection[row];
    float4 planes[6] = {rows[3]+rows[0],rows[3]-rows[0],rows[3]+rows[1],rows[3]-rows[1],rows[2],rows[3]-rows[2]};
    [unroll] for (uint plane=0;plane<6;++plane)
        if (dot(planes[plane],center) < -instance.sphere.w * length(planes[plane].xyz)) return;
    if (constants.selection.x >= 0)
    {
        const float3 origin=float3(asfloat(instance.meta.z),asfloat(instance.meta.w),instance.lod.z);
        const float distance = length(origin - constants.cameraPosition.xyz);
        const uint lod = distance < instance.lod.x ? 0 : distance < instance.lod.y ? 1 : 2;
        if (explicitMesh)
        {
            const float width=max(.001,min(.5,(instance.lod.y-instance.lod.x)*.25));
            if ((constants.selection.x == 0 && distance > instance.lod.x+width) ||
                (constants.selection.x == 1 && (distance < instance.lod.x-width || distance > instance.lod.y+width)) ||
                (constants.selection.x == 2 && distance < instance.lod.y-width)) return;
        }
        else if (min(lod,instance.meta.y) != (uint)constants.selection.x) return;
    }

    uint destination;
    InterlockedAdd(outputInstances[0].row0.y,1,destination);
    OutputRecord result;
    result.row0 = uint4(instance.meta.x,0,0,0); result.row1=0; result.row2=0;
    if (explicitMesh)
    {
        InstanceRecord transform=inputInstances[id.x*2];
        OutputRecord affine;
        affine.row0=asuint(transform.sphere); affine.row1=transform.meta; affine.row2=asuint(transform.lod);
        outputInstances[destination*2+1]=affine;
        result.row0=instance.meta;
        outputInstances[destination*2+2]=result;
    }
    else outputInstances[destination+1] = result;
}
