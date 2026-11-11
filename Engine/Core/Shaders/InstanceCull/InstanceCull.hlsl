struct InstanceRecord { float4 sphere; uint4 meta; float4 lod; };
struct PersistentInstance { float4 row0; float4 row1; float4 row2; uint4 identity; };
struct OutputRecord { uint4 row0; uint4 row1; uint4 row2; };
struct CullConstants
{
    float4x4 viewProjection;
    float4 cameraPosition;
    uint4 counts; // instance count, vertex/index count, indexed, reset phase
    int4 selection; // target LOD, mode, maximum LOD, flags (1 shadow, 2 per-placement bands)
};
#ifdef VULKAN
[[vk::push_constant]] CullConstants constants;
[[vk::binding(0, 0)]] StructuredBuffer<InstanceRecord> inputInstances;
[[vk::binding(1, 0)]] RWStructuredBuffer<OutputRecord> outputInstances;
[[vk::binding(2, 0)]] StructuredBuffer<PersistentInstance> placementPool;
#else
cbuffer Constants : register(b0) { CullConstants constants; };
StructuredBuffer<InstanceRecord> inputInstances : register(t0);
RWStructuredBuffer<OutputRecord> outputInstances : register(u0);
StructuredBuffer<PersistentInstance> placementPool : register(t1);
#endif
[numthreads(64,1,1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    if (constants.counts.w != 0)
    {
        if (id.x == 0)
        {
            OutputRecord args;
            args.row0 = uint4(constants.counts.y,0,constants.selection.y >= 1 ? constants.counts.z : 0,0);
            args.row1 = 0; args.row2 = 0;
            outputInstances[0] = args;
        }
        return;
    }
    if(constants.selection.y == 2)
    {
        if(group.x >= constants.counts.x) return;
        InstanceRecord world=inputInstances[group.x*3];
        InstanceRecord bounds=inputInstances[group.x*3+1];
        InstanceRecord lodData=inputInstances[group.x*3+2];
        // Shared LOD uses a tile-origin fade. Independent instance bands use
        // per-fragment fades and conservative per-placement selection.
        if(constants.selection.x>=0 && (constants.selection.w & 2)==0)
        {
            const float distance=length(lodData.sphere.xyz-constants.cameraPosition.xyz);
            const float width=max(.001,min(.5,(lodData.lod.y-lodData.lod.x)*.25));
            if((constants.selection.x==0 && distance>lodData.lod.x+width) ||
               (constants.selection.x==1 && (distance<lodData.lod.x-width || distance>lodData.lod.y+width)) ||
               (constants.selection.x==2 && distance<lodData.lod.y-width)) return;
        }
        const float3 localCenter=(bounds.sphere.xyz+bounds.lod.xyz)*.5;
        const float3 localExtent=(bounds.lod.xyz-bounds.sphere.xyz)*.5;
        const float3 worldX=world.sphere.xyz,worldY=asfloat(world.meta.xyz),worldZ=world.lod.xyz;
        const float4 worldRow1=asfloat(world.meta);
        float4 rows[4];
        [unroll] for(uint row=0;row<4;++row) rows[row]=constants.viewProjection[row];
        float4 planes[6]={rows[3]+rows[0],rows[3]-rows[0],rows[3]+rows[1],rows[3]-rows[1],rows[2],rows[3]-rows[2]};
        for(uint index=thread.x;index<bounds.meta.y;index+=64)
        {
            bool isTransitionPlacement=false;
            const uint placementIndex=bounds.meta.x+index;
            PersistentInstance placement=placementPool[placementIndex];
            const float4 center=float4(localCenter,1);
            const float3 instanceCenter=float3(dot(placement.row0,center),dot(placement.row1,center),dot(placement.row2,center));
            const float3 instanceExtent=float3(dot(abs(placement.row0.xyz),localExtent),dot(abs(placement.row1.xyz),localExtent),dot(abs(placement.row2.xyz),localExtent));
            const float4 worldCenter=float4(dot(world.sphere,float4(instanceCenter,1)),dot(worldRow1,float4(instanceCenter,1)),dot(world.lod,float4(instanceCenter,1)),1);
            const float3 worldExtent=float3(dot(abs(worldX),instanceExtent),dot(abs(worldY),instanceExtent),dot(abs(worldZ),instanceExtent));
            const float radius=length(worldExtent);
            const bool shadowPass=(constants.selection.w & 1)!=0;
            const float nearestDistance=shadowPass
                ? length(max(abs(worldCenter.xyz-constants.cameraPosition.xyz)-worldExtent,0.0))
                : max(0.0,length(worldCenter.xyz-constants.cameraPosition.xyz)-radius);
            if(lodData.lod.w>0.0 && nearestDistance>=lodData.lod.w) continue;
            if(constants.selection.x>=0 && (constants.selection.w & 2)!=0)
            {
                // Conservative per-placement bands. Shader dithering resolves the
                // overlap; testing the tile center would lose trees at boundaries.
                const float3 delta=abs(worldCenter.xyz-constants.cameraPosition.xyz);
                // Shadow vertices are static. Color vertices can bend in wind,
                // so retain their original conservative spherical envelope.
                const float nearest=shadowPass ? length(max(delta-worldExtent,0.0))
                    : length(delta)-radius;
                const float farthest=shadowPass ? length(delta+worldExtent)
                    : length(delta)+radius;
                const float width=max(.001,min(.5,(lodData.lod.y-lodData.lod.x)*.25));
                isTransitionPlacement = constants.selection.x>=0 &&
                    ((nearest <= lodData.lod.x+width && farthest >= lodData.lod.x-width) ||
                     (nearest <= lodData.lod.y+width && farthest >= lodData.lod.y-width));
                if ((constants.selection.x==0 && nearest>lodData.lod.x+width) ||
                    (constants.selection.x==1 && (farthest<lodData.lod.x-width || nearest>lodData.lod.y+width)) ||
                    (constants.selection.x==2 && farthest<lodData.lod.y-width)) continue;
            }
            // Range culling is shadow-only. Keep boundary-crossing placements
            // for the per-fragment fade instead of dropping the whole tile.
            if((constants.selection.w & 1)!=0 && lodData.lod.z>0.0 &&
                length(max(abs(worldCenter.xyz-constants.cameraPosition.xyz)-worldExtent,0.0))>=lodData.lod.z) continue;
            bool visible=true;
            [unroll] for(uint plane=0;plane<6;++plane)
                if(dot(planes[plane],worldCenter)<-(shadowPass
                    ? dot(abs(planes[plane].xyz),worldExtent)
                    : radius*length(planes[plane].xyz))) visible=false;
            if(!visible) continue;
            if (isTransitionPlacement) InterlockedAdd(outputInstances[0].row1.y,1);
            uint destination; InterlockedAdd(outputInstances[0].row0.y,1,destination);
            OutputRecord result; result.row0=uint4(bounds.meta.z,placementIndex,0,0); result.row1=placement.identity; result.row2=0;
            outputInstances[destination+1]=result;
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
    const float3 origin=float3(asfloat(instance.meta.z),asfloat(instance.meta.w),instance.lod.z);
    const float distance = length(origin - constants.cameraPosition.xyz);
    if(instance.lod.w>0.0 &&
        max(0.0,length(instance.sphere.xyz-constants.cameraPosition.xyz)-instance.sphere.w)>=instance.lod.w) return;
    if (constants.selection.x >= 0)
    {
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
