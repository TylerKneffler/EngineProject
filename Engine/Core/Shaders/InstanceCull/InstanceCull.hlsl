struct InstanceRecord { float4 sphere; uint4 meta; float4 lod; };
struct PersistentInstance { float4 row0; float4 row1; float4 row2; uint4 identity; };
struct OutputRecord { uint4 row0; uint4 row1; uint4 row2; };
struct CullConstants
{
    float4x4 viewProjection;
    float4 cameraPosition;
    uint4 counts; // instance count, vertex/index count, indexed, reset phase
    int4 selection; // target LOD, mode, maximum LOD, flags (1 shadow, 2 per-placement bands,
                    // 4 occlusion, 8/16 occlusion debug, 32 placement-scaled band distances)
};
#ifdef VULKAN
[[vk::push_constant]] CullConstants constants;
[[vk::binding(0, 0)]] StructuredBuffer<InstanceRecord> inputInstances;
[[vk::binding(1, 0)]] RWStructuredBuffer<OutputRecord> outputInstances;
[[vk::binding(2, 0)]] StructuredBuffer<PersistentInstance> placementPool;
[[vk::binding(3, 0)]] StructuredBuffer<float> occlusionPyramid;
#else
cbuffer Constants : register(b0) { CullConstants constants; };
StructuredBuffer<InstanceRecord> inputInstances : register(t0);
RWStructuredBuffer<OutputRecord> outputInstances : register(u0);
StructuredBuffer<PersistentInstance> placementPool : register(t1);
StructuredBuffer<float> occlusionPyramid : register(t2);
#endif

// Occlusion test against the camera's software occlusion buffer (CPU-built
// from this frame's terrain; layout in Core/Rendering/OcclusionBuffer.h,
// SerializeForGpu). Must match OcclusionBuffer::IsBoxHidden: the box is
// hidden only when its nearest point lies behind the farthest occluder depth
// over its whole screen rectangle. Bounds crossing the near plane or wholly
// off screen stay visible. selection.w bit 4 enables it (camera passes only).
// Ground cover thinning, matching Object.hlsl VSMain (GrassWindHash and the
// share formula must stay identical): true when the vertex shader would
// shrink this placement to nothing, so it need not be drawn at all.
// thin = (start, end, minimum share, keep width) from the tile record.
uint ThinHash(int2 cell, uint seed)
{
    uint hash = asuint(cell.x) * 0x8da6b343u ^ asuint(cell.y) * 0xd8163841u ^ seed;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    hash *= 0x846ca68bu;
    return hash ^ (hash >> 16);
}

bool IsThinnedAway(float4 thin, float3 root, float3 camera)
{
    if (thin.z >= 1.0 || thin.x == thin.y) return false;
    const float share = lerp(1.0, saturate(thin.z), smoothstep(thin.x, thin.y, length(root - camera)));
    const float rank = (float)(ThinHash(int2(floor(root.xz * 16.0)), 0x1b873593u) & 4095u) / 4095.0;
    return share <= rank;
}

bool IsBoxOccluded(float3 center, float3 extent)
{
    const uint levels = (uint)occlusionPyramid[0];
    if (levels == 0u) return false;
    const float width = occlusionPyramid[1], height = occlusionPyramid[2];
    float4x4 viewProjection;
    [unroll] for (uint row = 0u; row < 4u; ++row)
        viewProjection[row] = float4(occlusionPyramid[64u + row * 4u], occlusionPyramid[65u + row * 4u],
            occlusionPyramid[66u + row * 4u], occlusionPyramid[67u + row * 4u]);
    float2 low = float2(3.0e38, 3.0e38), high = float2(-3.0e38, -3.0e38);
    float nearest = 3.0e38;
    [unroll] for (uint corner = 0u; corner < 8u; ++corner)
    {
        const float3 offset = float3((corner & 1u) ? 1.0 : -1.0, (corner & 2u) ? 1.0 : -1.0,
            (corner & 4u) ? 1.0 : -1.0);
        const float4 clip = mul(viewProjection, float4(center + extent * offset, 1.0));
        if (clip.w <= 0.05) return false;
        const float2 screen = float2((clip.x / clip.w * 0.5 + 0.5) * width,
            (0.5 - clip.y / clip.w * 0.5) * height);
        low = min(low, screen); high = max(high, screen);
        nearest = min(nearest, clip.w);
    }
    if (high.x < 0.0 || high.y < 0.0 || low.x >= width || low.y >= height) return false;
    const int2 first = clamp(int2(floor(low)), int2(0, 0), int2(width - 1.0, height - 1.0));
    const int2 last = clamp(int2(floor(high)), int2(0, 0), int2(width - 1.0, height - 1.0));
    uint level = 0u;
    [loop] while (level + 1u < levels &&
        ((last.x >> level) - (first.x >> level) > 1 || (last.y >> level) - (first.y >> level) > 1))
        ++level;
    const uint offset = (uint)occlusionPyramid[4u + level * 3u];
    const int levelWidth = (int)occlusionPyramid[5u + level * 3u];
    const int levelHeight = (int)occlusionPyramid[6u + level * 3u];
    float farthest = 0.0;
    [loop] for (int y = first.y >> level; y <= min(levelHeight - 1, last.y >> level); ++y)
        [loop] for (int x = first.x >> level; x <= min(levelWidth - 1, last.x >> level); ++x)
            farthest = max(farthest, occlusionPyramid[offset + (uint)(y * levelWidth + x)]);
    if (farthest >= 3.0e38) return false;
    return nearest > farthest * 1.002 + 0.05;
}
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
            // The placement's root (its translation) in world space.
            const float4 rootLocal=float4(placement.row0.w,placement.row1.w,placement.row2.w,1);
            const float3 root=float3(dot(world.sphere,rootLocal),dot(worldRow1,rootLocal),dot(world.lod,rootLocal));
            // A range with a shorter reach (selection.w bits 16-25, in
            // 1/1024ths; Object.hlsl draw.placementReach scales the same way)
            // thins sooner and is gone by cameraPosition.w.
            const uint reachBits=((uint)constants.selection.w>>16)&0x3ffu;
            float4 thin=asfloat(lodData.meta);
            if(reachBits!=0u)
            {
                thin.xy*=(float)reachBits/1024.0;
                if(constants.cameraPosition.w>0.0 &&
                    length(root-constants.cameraPosition.xyz)>=constants.cameraPosition.w) continue;
            }
            if(IsThinnedAway(thin,root,constants.cameraPosition.xyz)) continue;
            if(constants.selection.x>=0 && (constants.selection.w & 2)!=0)
            {
                // Conservative per-placement bands. Shader dithering resolves the
                // overlap; testing the tile center would lose trees at boundaries.
                const float3 delta=abs(worldCenter.xyz-constants.cameraPosition.xyz);
                // Screen coverage (bit 32): a placement scaled by s covers s
                // times the pixels, so its distance counts as distance / s
                // (Core/Rendering/ScreenCoverage.h). Object.hlsl divides the
                // same way for geometryFade.w 5; keep the clamp in sync.
                const float coverage=(constants.selection.w & 32)!=0
                    ? clamp(length(placement.row0.xyz),0.25,4.0) : 1.0;
                // Shadow vertices are static. Color vertices can bend in wind,
                // so retain their original conservative spherical envelope.
                // Ground cover (bit 64, Mesh::perPlacementLod): Object.hlsl
                // fades by the distance to the placement's root, so test
                // exactly that. The bounds envelope is far wider than the
                // few metres between ground cover levels and would draw
                // plants only for the pixel shader to discard them.
                const bool rootDistance=(constants.selection.w & 64)!=0;
                const float rootRange=length(root-constants.cameraPosition.xyz);
                const float nearest=rootDistance ? rootRange : (shadowPass ? length(max(delta-worldExtent,0.0))
                    : length(delta)-radius)/coverage;
                const float farthest=rootDistance ? rootRange : (shadowPass ? length(delta+worldExtent)
                    : length(delta)+radius)/coverage;
                // Object.hlsl picks each placement's band from the distance to
                // its origin and dissolves the whole placement over
                // InstanceLodFadeWidth either side of a switch; keep in sync.
                const float widthX=clamp(lodData.lod.x*.04,.5,6.0);
                const float widthY=clamp(lodData.lod.y*.04,.5,6.0);
                isTransitionPlacement = constants.selection.x>=0 &&
                    ((nearest <= lodData.lod.x+widthX && farthest >= lodData.lod.x-widthX) ||
                     (nearest <= lodData.lod.y+widthY && farthest >= lodData.lod.y-widthY));
                if ((constants.selection.x==0 && nearest>lodData.lod.x+widthX) ||
                    (constants.selection.x==1 && (farthest<lodData.lod.x-widthX || nearest>lodData.lod.y+widthY)) ||
                    (constants.selection.x==2 && farthest<lodData.lod.y-widthY)) continue;
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
            // Behind terrain (camera passes only; shadows still need it).
            // Debug (bit 8): keep it and mark it for the Detail view instead.
            uint occludedMark=0u;
            // Bit 16 (debug control): treat everything tested as hidden.
            if(!shadowPass && (constants.selection.w & 4)!=0 &&
                ((constants.selection.w & 16)!=0 || IsBoxOccluded(worldCenter.xyz, worldExtent)))
            {
                if((constants.selection.w & 8)==0) continue;
                occludedMark=1u;
            }
            if (isTransitionPlacement) InterlockedAdd(outputInstances[0].row1.y,1);
            uint destination; InterlockedAdd(outputInstances[0].row0.y,1,destination);
            OutputRecord result; result.row0=uint4(bounds.meta.z,placementIndex,occludedMark,0); result.row1=placement.identity; result.row2=0;
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
    uint occludedMark = 0u;
    if ((constants.selection.w & 5) == 4 &&
        ((constants.selection.w & 16) != 0 || IsBoxOccluded(instance.sphere.xyz, instance.sphere.www)))
    {
        if ((constants.selection.w & 8) == 0) return;
        occludedMark = 1u;
    }
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
    result.row0 = uint4(instance.meta.x,0,occludedMark,0); result.row1=0; result.row2=0;
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
