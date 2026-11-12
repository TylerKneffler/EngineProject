#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Terrain/Terrain.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
#include "Core/Compoonents/Terrain/TerrainManager.h"
#include "Core/Rendering/RenderPerformance.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/Serialization/HexBytes.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Rendering/SharedMeshBuffers.h"
#include <cstring>
#include <set>
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#include "Core/Renderers/Vulkan/VulkanGameRenderer.h"
#include "Core/Assets/Scripts/Grass/GrassField.h"
#include "Core/Compoonents/Lighting/Light.h"
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <numeric>
#include <limits>
#include <glm/gtc/matrix_transform.hpp>
#include <type_traits>
#include <cstdlib>
#include <d3d12sdklayers.h>

using namespace Engine::Renderers;
using namespace Engine::Components;
void Check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
void ConfigureCanopy(TerrainObjectLayer&, const char*, bool, std::ostream*)
{
    throw std::runtime_error("Canopy experiment helper is unavailable in this checkout");
}

void CheckGrassPhaseTiming()
{
    {
        Engine::Scene::Scene grassScene;
        auto* field=grassScene.AddObject("Grass without terrain")->AddComponent<GrassField>();
        const auto phases=[](const Engine::Rendering::GrassPerformance& p) {
            return p.referencesMs+p.sourcesMs+p.placementMs+p.visibilityMs+
                p.retirementMs+p.schedulingMs+p.buildMs+p.accountingMs+p.otherMs;
        };
        field->TickSpatialRegion(false);
        const auto first=field->GetLastPerformance();
        Check(first.totalMs>0 && std::abs(phases(first)-first.totalMs)<1.e-6,
            "early grass refresh failed to publish complete phase timing");
        field->TickSpatialRegion(false);
        const auto& accumulated=grassScene.GetGrassPerformance();
        const bool accumulatedCorrectly=
            std::abs(accumulated.totalMs-first.totalMs-field->GetLastPerformance().totalMs)<1.e-6 &&
            std::abs(phases(accumulated)-accumulated.totalMs)<1.e-6;
        if(!accumulatedCorrectly)
            std::cerr << std::setprecision(17) << "Grass accumulation: first=" << first.totalMs
                << " second=" << field->GetLastPerformance().totalMs << " scene=" << accumulated.totalMs
                << " scene_phases=" << phases(accumulated) << '\n';
        Check(accumulatedCorrectly,
            "grass phase accumulation lost or double-counted time");
    }
}

void CheckResolvedMaterialCopy()
{
    using namespace Engine::Serialization;
    std::filesystem::create_directories(".local/tests-generated");
    const std::string path = ".local/tests-generated/resolved-copy.material";
    { std::ofstream out(path); out << R"({"baseColor":[0.2,0.4,0.6],"roughness":0.3})"; }
    Engine::Scene::Scene scene;
    auto* source = scene.AddObject("Prototype")->AddComponent<Material>();
    auto* copy = scene.AddObject("Batch")->AddComponent<Material>();
    Check(source->LoadFromFile(path), "resolved material fixture did not load");
    auto data = source->Serialize();
    data.Set("shader", "custom.shader").Set("alphaMode", "Mask")
        .Set("edgeColor", JsonParse("[0.2,0.3,0.4,0.5]"))
        .Set("sphereTint", JsonParse("[0.4,0.5,0.6,0.7]"))
        .Set("toonTint", JsonParse("[0.6,0.7,0.8,0.9]"))
        .Set("textureLodMipBias", 1.5f).Set("baseColorUvSet", 3)
        .Set("subsurfaceEnabled", true).Set("subsurfaceWeight", 0.7f);
    source->Deserialize(data);
    const auto color = Texture::Acquire(".local/tests-generated/resolved-color.png", true);
    const auto linear = Texture::Acquire(".local/tests-generated/resolved-linear.png", false);
    source->baseColorTexture = color; source->sphereTexture = color;
    source->toonTexture = color; source->emissiveTexture = color;
    source->metallicRoughnessTexture = linear; source->normalTexture = linear;
    source->heightTexture = linear; source->occlusionTexture = linear;
    source->subsurfaceThicknessTexture = linear;
    source->reflectionEnvironmentTexture = linear->GetFilePath();
    source->reflectionEnvironmentMap = linear;
    source->Validate();
    Material legacy;
    legacy.Deserialize(source->Serialize());
    const auto expected = JsonWriteCompact(legacy.Serialize());
    auto* owner = copy->Owner;
    const auto revision = copy->GetConfigurationRevision();
    // A batch copies the state of its exact prototype, even if a file changes
    // halfway through constructing its spatial tiles. Later file loads still
    // observe the edit normally.
    { std::ofstream out(path); out << R"({"baseColor":[0.9,0.8,0.7],"roughness":0.8})"; }
    copy->shaderError = "old diagnostic";
    copy->CopyResolvedStateFrom(*source);
    Check(copy->Owner == owner && copy->GetConfigurationRevision() != revision,
        "resolved material copy lost owner or dirty notification");
    Check(copy->shaderError.empty() && JsonWriteCompact(copy->Serialize()) == expected,
        "resolved material copy changed serialized values");
    Check(copy->baseColorTexture == color && copy->sphereTexture == color &&
        copy->toonTexture == color && copy->emissiveTexture == color &&
        copy->metallicRoughnessTexture == linear && copy->normalTexture == linear &&
        copy->heightTexture == linear && copy->occlusionTexture == linear &&
        copy->subsurfaceThicknessTexture == linear && copy->reflectionEnvironmentMap == linear,
        "resolved material copy lost shared texture ownership");
    Material edited;
    Check(edited.LoadFromFile(path) && edited.roughnessFactor == 0.8f,
        "ordinary material loading stopped observing edits");
    auto modified = copy->Serialize(); modified.Set("roughnessFactor", 0.6f);
    copy->Deserialize(modified);
    Check(copy->roughnessFactor == 0.6f && source->roughnessFactor == 0.3f,
        "resolved material copy retained prototype field bindings");
    copy->CopyResolvedStateFrom(*source);
    copy->CopyResolvedStateFrom(*copy);
    scene.RemoveObject(source->Owner);
    scene.CommitPendingObjectChanges();
    Check(copy->Owner == owner && copy->baseColorTexture == color &&
        JsonWriteCompact(copy->Serialize()) == expected,
        "destroying prototype invalidated batch material");
    Material empty;
    copy->CopyResolvedStateFrom(empty);
    Check(!copy->baseColorTexture && !copy->reflectionEnvironmentMap && copy->GetFilePath().empty(),
        "resolved material replacement retained old resources");
}

void CheckSharedPrefabLayer()
{
    CheckGrassPhaseTiming();
    CheckResolvedMaterialCopy();
    {
        using namespace Engine::Serialization;
        const auto simple=JsonParse("[1,true,null,{},[]]");
        Check(JsonWriteCompact(simple)=="[1,true,null,{},[]]",
            "compact JSON retained formatting whitespace");
        Check(JsonWrite(simple)=="[\n    1,\n    true,\n    null,\n    {},\n    []\n]\n",
            "compact JSON changed existing pretty output");
        const auto source=JsonParse(R"({"text":"line\n\"quoted\"\\\t with spaces","values":[1.2345678901234567,1e-300,-10000000000000000],"empty":[]})");
        const auto compact=JsonWriteCompact(source);
        Check(JsonWrite(JsonParse(compact))==JsonWrite(source),
            "compact JSON changed nested values, precision or escaped strings");
        Check(compact.size()<JsonWrite(source).size(),"compact JSON did not reduce snapshot size");
    }
    using namespace Engine::Graphics;
    {
        using namespace Engine::Serialization;
        TerrainObjectLayer source;
        for(int i=0;i<3;++i)
        {
            TerrainObjectLayer::Placement placement;
            placement.id=i<2 ? 7 : 0; // Duplicate and missing authored IDs.
            placement.asset="fixture.prefab";
            placement.position={float(i),0.f,0.f};
            source.placements.push_back(placement);
        }
        const auto data=source.Serialize();
        const auto signature=source.GetPlacementSignature();
        TerrainObjectLayer restored;
        restored.Deserialize(data);
        Check(restored.HasLoadedPreparedData() && restored.GetPlacementSignature()==signature,
            "normalized placement signature rejected valid baked data");
        std::set<uint64_t> ids;
        for(size_t i=0;i<restored.placements.size();++i)
            Check(restored.placements[i].id!=0 && ids.insert(restored.placements[i].id).second &&
                restored.placements[i].id==source.placements[i].id,"placement ID normalization changed");
        Check(JsonWriteCompact(restored.Serialize())==JsonWriteCompact(data),
            "baked placement roundtrip changed serialized values");
        auto stale=data;
        stale["placements"].ArrayAt(0)["position"].ArrayAt(0)=JsonValue(25.f);
        restored.Deserialize(stale);
        Check(!restored.HasLoadedPreparedData(),"stale baked placement signature was accepted");
        Check(restored.GetPreparedSignature(4.f)==restored.GetPlacementSignature(),
            "stale baked placement fallback failed to rebuild");
        std::vector<uint8_t> bytes;
        Check(DecodeHex(data["bakedInstances"].AsString(),bytes),"baked fixture decode failed");
        // Duplicate a record index while retaining the valid placement signature.
        std::memcpy(bytes.data()+20+60,bytes.data()+20,sizeof(uint32_t));
        auto corrupt=data;
        corrupt.Set("bakedInstances",JsonValue(EncodeHex(bytes)));
        restored.Deserialize(corrupt);
        Check(!restored.HasLoadedPreparedData(),"duplicate baked record index was accepted");
        auto legacy=data;
        legacy.Erase("bakedInstances");
        legacy["placements"].ArrayAt(0).Set("id",JsonValue("0"));
        legacy["placements"].ArrayAt(1).Set("id",JsonValue("0"));
        restored.Deserialize(legacy);
        Check(!restored.HasLoadedPreparedData() && restored.placements[0].id!=0 &&
            restored.placements[1].id!=0 && restored.placements[0].id!=restored.placements[1].id,
            "legacy placement load skipped missing ID normalization");
    }
    struct Buffer : IGraphicsBuffer
    {
        Usage usage; AccessMode access; uint32_t stride; std::vector<uint8_t> bytes;
        Usage GetUsage() const override { return usage; }
        AccessMode GetAccessMode() const override { return access; }
        uint64_t GetSize() const override { return bytes.size(); }
        uint32_t GetElementStride() const override { return stride; }
        void* Map() override { return bytes.data(); }
        void Unmap() override {}
        void* GetNativeHandle() const override { return const_cast<Buffer*>(this); }
    };
    struct Provider : IGraphicsProvider, IGraphicsBufferFactory
    {
        unsigned allocations=0;
        IShaderCompiler* GetShaderCompiler() override { return nullptr; }
        IGraphicsBufferFactory* GetBufferFactory() override { return this; }
        IPipelineStateFactory* GetPipelineStateFactory() override { return nullptr; }
        IGraphicsContextFactory* GetContextFactory() override { return nullptr; }
        std::unique_ptr<IGraphicsBuffer> CreateBuffer(IGraphicsBuffer::Usage usage,
            IGraphicsBuffer::AccessMode access,uint64_t bytes,const void* data,uint32_t stride) override
        {
            auto result=std::make_unique<Buffer>(); result->usage=usage; result->access=access;
            result->stride=stride; result->bytes.resize(bytes);
            if (data) std::memcpy(result->bytes.data(),data,bytes);
            ++allocations; return result;
        }
    } provider;
    // Prefab source discovery and prototype construction are asynchronous.
    // Drive frame preparation until pending work and incremental probes settle.
    const auto preparePrefab = [&](TerrainObjectLayer* layer, Engine::Scene::Scene& scene)
    {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        unsigned settled=0;
        do
        {
            layer->PreparePrefabInstances(&provider);
            scene.CommitPendingObjectChanges();
            settled=layer->IsPrefabPreparationPending() ? 0 : settled+1;
            if (settled>=32) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now()<deadline);
        const auto& timings=layer->GetLastPrefabPrepareTimings();
        std::cerr << "Prefab wait expired: pending=" << layer->IsPrefabPreparationPending()
            << " validation_ms=" << timings.validationMs << " build_ms=" << timings.buildMs
            << " source_lookup_ms=" << timings.sourceLookupMs << '\n';
        throw std::runtime_error("prefab preparation did not settle");
    };
    {
        Provider bufferProvider;
        std::vector<unsigned char> input(65, 0x5a);
        for (size_t size=1; size<=63; ++size)
        {
            // Exercise every word-tail length with an unaligned input.
            auto first=Engine::Rendering::AcquireMeshBuffer(&bufferProvider,
                IGraphicsBuffer::Usage::VertexBuffer,input.data()+1,size,1);
            auto same=Engine::Rendering::AcquireMeshBuffer(&bufferProvider,
                IGraphicsBuffer::Usage::VertexBuffer,input.data()+1,size,1);
            Check(first && first==same,"identical buffer content was not shared");
            for (size_t byte=1; byte<=size; ++byte)
            {
                input[byte]^=1;
                auto changed=Engine::Rendering::AcquireMeshBuffer(&bufferProvider,
                    IGraphicsBuffer::Usage::VertexBuffer,input.data()+1,size,1);
                Check(changed && changed!=first,"different buffer content was shared");
                Check(std::memcmp(static_cast<Buffer*>(changed.get())->bytes.data(),
                    input.data()+1,size)==0,"shared buffer lost source bytes");
                input[byte]^=1;
            }
        }
    }
    std::filesystem::create_directories(".local/tests-generated");
    {
        const std::string payload(1024*1024,'x');
        const std::string path=".local/tests-generated/xml-borrowed-text.prefab";
        {
            std::ofstream out(path,std::ios::binary);
            out << "<Prefab><object enabled=\"true\" disabled=\"false\" number=\"-1.25e2\" "
                "overflow=\"1e9999\" partial=\"12tail\" empty=\"\" payload=\"" << payload
                << "\"><numbers _arrayEncoding=\"spaceSeparatedNumbers\"> 1, -2.5 3e2 </numbers>"
                "<literal _valueType=\"string\">true</literal>"
                "<entities _valueType=\"string\">Gr&#x61;ss &amp; &lt;trees&gt; &#xE9;</entities></object></Prefab>";
        }
        const auto document=Engine::Serialization::SceneSerializer::ReadPrefabFile(path);
        const auto& object=document["object"];
        Check(object["enabled"].IsBool() && object["enabled"].AsBool() &&
            object["disabled"].IsBool() && !object["disabled"].AsBool() &&
            object["number"].IsNumber() && object["number"].AsDouble()==-125 &&
            object["overflow"].AsString()=="1e9999" && object["partial"].AsString()=="12tail" &&
            object["empty"].IsString() && object["empty"].AsString().empty() &&
            object["payload"].AsString()==payload && object["literal"].AsString()=="true" &&
            object["entities"].AsString()=="Grass & <trees> \xc3\xa9",
            "borrowed XML scalar parsing changed values or lost owned string storage");
        const auto& numbers=object["numbers"];
        Check(numbers.ArraySize()==3 && numbers.ArrayAt(0).AsDouble()==1 &&
            numbers.ArrayAt(1).AsDouble()==-2.5 && numbers.ArrayAt(2).AsDouble()==300,
            "borrowed XML number-array parsing changed values");
    }
    {
        const std::string xml="<Prefab version=\"1\" type=\"prefab\"><object name=\"Read once\"><components/><children/></object></Prefab>";
        const std::string json="{\"version\":1,\"type\":\"prefab\",\"object\":{\"name\":\"Read once\",\"components\":[],\"children\":[]}}";
        std::string utf16="\xff\xfe";
        for (char c:xml) { utf16.push_back(c); utf16.push_back('\0'); }
        std::string utf16be="\xfe\xff",utf32("\xff\xfe\0\0",4);
        for (char c:xml)
        {
            utf16be.push_back('\0'); utf16be.push_back(c);
            utf32.push_back(c); utf32.append(3,'\0');
        }
        for (const auto& [format,contents]:std::vector<std::pair<std::string,std::string>>{
            {"xml", " \r\n"+xml}, {"json", " \r\n"+json},
            {"bom", std::string("\xef\xbb\xbf")+xml}, {"utf16",utf16},
            {"utf16be",utf16be}, {"utf32",utf32},
            {"empty", ""}, {"invalid", "<Prefab"}})
        {
            const auto path=".local/tests-generated/read-once-"+format+".prefab";
            { std::ofstream out(path,std::ios::binary); out.write(contents.data(),contents.size()); }
            bool loaded=false;
            try
            {
                const auto document=Engine::Serialization::SceneSerializer::ReadPrefabFile(path);
                loaded=document["object"]["name"].AsString()=="Read once";
            }
            catch (const std::exception&) {}
            Check(loaded==(format!="empty" && format!="invalid"),
                "single-read prefab loader changed format or error handling");
            std::ifstream input(path,std::ios::binary);
            Check(std::string(std::istreambuf_iterator<char>(input),{})==contents,
                "in-place prefab parsing changed source file bytes");
        }
    }
    const std::string meshPath=".local/tests-generated/shared-layer.mesh";
    const std::string prefabPath=".local/tests-generated/shared-layer.prefab";
    std::vector<Mesh::Vertex> vertices(3);
    vertices[1].pos[0]=1; vertices[2].pos[1]=1;
    for (auto& vertex:vertices) { vertex.normal[2]=1; for (auto& color:vertex.color) color=1; }
    if (!std::filesystem::exists(meshPath))
        Check(Mesh::SaveNativeFile(meshPath,vertices),"fixture mesh save failed");
    auto writePrefab=[&](float radius,const std::string& destination,float wind=0.f) {
        std::ofstream out(destination);
        out << "<Prefab xmlns:components=\"urn:engine-components\" version=\"1\" type=\"prefab\">"
            "<object name=\"Shared test tree\"><components><components:Mesh Mesh.file=\"" << meshPath
            << "\" Mesh.foliageWindStrength=\"" << wind << "\"/>";
        if (radius>0) out << "<components:PrimitiveObjectCollider PrimitiveObjectCollider.shape=\"Capsule\" "
            "PrimitiveObjectCollider.radius=\"" << radius << "\" PrimitiveObjectCollider.height=\"3\"/>";
        out << "</components><children/></object></Prefab>";
    };
    writePrefab(0,prefabPath);
    {
        Engine::Scene::Scene baselineScene;
        Provider baselineProvider;
        auto* root=Engine::Serialization::SceneSerializer::InstantiatePrefab(
            baselineScene,prefabPath,&baselineProvider);
        Check(root && !root->PrefabSourceSnapshot.empty(),"prefab baseline missing");
        Check(!Engine::Serialization::SceneSerializer::HasPrefabOverrides(*root),
            "compact baseline introduced initial overrides");
        root->name="Edited baseline name";
        root->InvalidatePrefabOverrideCache();
        Check(Engine::Serialization::SceneSerializer::HasPrefabOverrides(*root,false),
            "compact baseline failed to detect an override");
        Check(Engine::Serialization::SceneSerializer::RevertPrefabOverrides(*root,&baselineProvider) &&
            root->name=="Shared test tree" &&
            !Engine::Serialization::SceneSerializer::HasPrefabOverrides(*root),
            "compact baseline failed to revert an override");
    }
    std::cerr << "Shared prefab fixture written\n";
    Engine::Scene::Scene scene;
    auto* owner=scene.AddObject("Painted objects"); owner->transform.position={-180,0,-180};
    auto* layer=owner->AddComponent<TerrainObjectLayer>();
    layer->shadowLod=2;
    layer->shadowFadeWidth=4.f;
    layer->screenLodSelection=true;
    layer->screenLod1Pixels=444;
    for (int i=0;i<2;++i)
    {
        TerrainObjectLayer::Placement p; p.asset=prefabPath; p.position={180.f+2*i,0,180};
        p.scale=i+1.f; layer->placements.push_back(p);
    }
    layer->MarkConfigurationDirty(); preparePrefab(layer,scene);
    std::cerr << "Shared prefab first preparation complete\n";
    const auto meshes=scene.GetComponents<Mesh>();
    Check(meshes.size()==1 && meshes.front()->IsInstanced(),"prefab geometry was not shared");
    Check(meshes.front()->fixedShadowLodBand==0,"single-level prefab lost its fixed shadow fallback");
    Check(meshes.front()->runtimeShadowFadeWidth==4.f,"layer fade width did not reach shared geometry");
    Check(meshes.front()->screenLodSelection && meshes.front()->screenLod1Pixels==444,
        "layer screen policy did not reach shared geometry");
    Check(meshes.front()->GetInstanceData()->transforms.size()==2,"placement instances missing");
    Check(provider.allocations==1,"source vertex buffer duplicated per placement");
    Check(glm::length(meshes.front()->Owner->transform.GetWorldPosition()-glm::vec3(-180,0,-180))<.001f,
        "shared batch did not inherit the layer transform");
    Check(scene.GetComponents<PrimitiveObjectCollider>().empty() && scene.GetComponents<RigidBody>().empty(),
        "colliders synthesized for collider-free prefab");
    Check(Engine::Serialization::SceneSerializer::SaveToString(scene).find("Shared prefab part:")==std::string::npos,
        "derived instances leaked into authored scene serialization");
    const auto stableObjectId=scene.GetObjectId(meshes.front()->Owner);
    const auto stableAllocations=provider.allocations;
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    preparePrefab(layer,scene);
    Check(scene.GetObjectId(scene.GetComponents<Mesh>().front()->Owner)==stableObjectId &&
        provider.allocations==stableAllocations,"unchanged prefab poll rebuilt shared geometry");
    // Scene restore can deserialize the same content into a new placements
    // allocation and increment the component revision. Keep the existing
    // generated batches when the restored content and policy are identical.
    layer->Deserialize(layer->Serialize());
    const auto identicalRestoreStarted=std::chrono::steady_clock::now();
    preparePrefab(layer,scene);
    const double identicalRestoreMs=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-identicalRestoreStarted).count();
    Check(scene.GetObjectId(scene.GetComponents<Mesh>().front()->Owner)==stableObjectId &&
        provider.allocations==stableAllocations,
        "identical layer restore unnecessarily rebuilt shared prefab geometry");
    std::cerr << "Identical restore prefab preparation: " << identicalRestoreMs
        << " ms; generated object and buffer allocations reused\n";
    // Public placement records historically detect in-place edits on polling,
    // even without MarkConfigurationDirty(). Retain that compatibility.
    layer->placements.front().position.x+=1.f;
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    preparePrefab(layer,scene);
    Check(scene.GetComponents<Mesh>().front()->GetInstanceData()->transforms.front().rows[0].w==181.f,
        "polling lost an in-place placement edit");
    owner->transform.rotation={.2f,.4f,-.1f};
    owner->transform.scale={2.f,.75f,1.25f};
    layer->MarkConfigurationDirty(); preparePrefab(layer,scene);
    const auto layerWorld=owner->transform.GetWorldMatrix();
    for(const auto* mesh:scene.GetComponents<Mesh>())
    {
        Check(mesh->Owner->Parent==owner && mesh->Owner->runtimeOnly,
            "generated prefab batch lost layer ownership");
        const auto local=mesh->Owner->transform.GetLocalMatrix();
        const auto world=mesh->Owner->transform.GetWorldMatrix();
        for(int column=0;column<4;++column) for(int row=0;row<4;++row)
        {
            Check(std::abs(local[column][row]-(column==row ? 1.f:0.f))<.00001f,
                "generated prefab batch did not retain an identity local transform");
            Check(std::abs(world[column][row]-layerWorld[column][row])<.00001f,
                "generated prefab batch did not inherit the rotated/scaled layer");
        }
    }
    const std::string lodPrefabPath=".local/tests-generated/shared-layer-lod.prefab";
    writePrefab(0,lodPrefabPath);
    TerrainObjectLayer::Placement lodPlacement;
    lodPlacement.asset=std::filesystem::absolute(lodPrefabPath).generic_string();
    lodPlacement.position={184,0,180}; layer->placements.push_back(lodPlacement);
    layer->objectLods[prefabPath]={{prefabPath,lodPrefabPath},{},{28}};
    layer->shadowLod=2; // A two-band chain falls back to its last available proxy.
    TerrainObjectLayer roundTrip; roundTrip.Deserialize(layer->Serialize());
    Check(roundTrip.shadowLod==2,"fixed shadow LOD was not serialized");
    Check(roundTrip.shadowFadeWidth==4.f,"shadow fade width was not serialized");
    auto legacyData=layer->Serialize(); legacyData.Erase("shadowFadeWidth");
    TerrainObjectLayer legacy; legacy.Deserialize(legacyData);
    Check(legacy.shadowFadeWidth==1.f,"legacy layer changed its one-metre fade default");
    auto invalidFade=layer->Serialize(); invalidFade.Set("shadowFadeWidth",-4.f);
    legacy.Deserialize(invalidFade);
    Check(legacy.shadowFadeWidth==0.f,"negative shadow fade was not bounded");
    invalidFade.Set("shadowFadeWidth",1000.f); legacy.Deserialize(invalidFade);
    Check(legacy.shadowFadeWidth==legacy.shadowDistance,"shadow fade exceeded caster range");
    TerrainObjectLayer streamed;
    streamed.InstallPrepared(std::move(roundTrip));
    Check(streamed.shadowFadeWidth==4.f,"streaming lost shadow fade width");
    layer->shadowFadeWidth=2.f;
    layer->MarkConfigurationDirty();
    for (float radius : {.8f,1.25f,0.f})
    {
        std::cerr << "Checking prefab collider radius " << radius << '\n';
        writePrefab(radius,prefabPath);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        // Stable prefab source probes are spread across successive frame
        // preparations. Drive the fixture as a frame loop until the changed
        // source has been reached (or fail after a bounded number of frames).
        for(int frame=0;frame<64;++frame)
        {
            preparePrefab(layer,scene);
            const auto current=scene.GetComponents<PrimitiveObjectCollider>();
            if(current.size()==(radius>0 ? 3u:0u) &&
                std::all_of(current.begin(),current.end(),[&](const auto* collider) {
                    return std::abs(collider->radius-radius)<.0001f;
                })) break;
        }
        std::cerr << "Prefab edit prepared\n";
        for(auto* mesh:scene.GetComponents<Mesh>())
        {
            if (mesh->fixedShadowLodBand!=1)
                std::cerr << "Unexpected shadow band " << mesh->fixedShadowLodBand
                    << " on " << mesh->Owner->name << " visible band " << mesh->instanceLodBand
                    << " placements " << (mesh->GetInstanceData() ? mesh->GetInstanceData()->transforms.size() : 0) << '\n';
            Check(mesh->fixedShadowLodBand==1,"fixed shadow LOD did not clamp to the available chain");
            Check(mesh->runtimeShadowFadeWidth==2.f,"regenerated prefab retained stale shadow fade width");
        }
        const auto colliders=scene.GetComponents<PrimitiveObjectCollider>();
        Check(colliders.size()==(radius>0 ? 3u:0u),"prefab collider edit did not affect all placements and LODs");
        Check(scene.GetComponents<RigidBody>().size()==colliders.size(),"static prefab bodies missing");
        for (auto* collider:colliders) Check(std::abs(collider->radius-radius)<.0001f,"shared collider radius stale");
    }
    writePrefab(0.f,lodPrefabPath,.5f);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    bool editedLodFound=false;
    for(int frame=0;frame<64 && !editedLodFound;++frame)
    {
        preparePrefab(layer,scene);
        for(const auto* mesh:scene.GetComponents<Mesh>()) if(mesh->instanceLodBand==1)
        {
            editedLodFound=true;
            Check(mesh->foliageWindStrength==.5f,"LOD-only prefab edit was not polled");
        }
    }
    Check(editedLodFound,"edited prefab LOD disappeared");
    const auto savedLods=layer->objectLods[prefabPath];
    layer->objectLods[prefabPath].meshes.resize(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    preparePrefab(layer,scene);
    for(const auto* mesh:scene.GetComponents<Mesh>())
        Check(mesh->instanceLodBand==-1 && mesh->fixedShadowLodBand==0,
            "in-place LOD definition edit did not return to single-level geometry");
    layer->objectLods[prefabPath]=savedLods;
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    preparePrefab(layer,scene);
    editedLodFound=false;
    for(const auto* mesh:scene.GetComponents<Mesh>()) editedLodFound|=mesh->instanceLodBand==1;
    Check(editedLodFound,"in-place LOD definition restoration was not polled");
    {
        const std::string shadowProxyPath=".local/tests-generated/shared-layer-shadow.prefab";
        writePrefab(0.f,shadowProxyPath,.25f);
        Engine::Scene::Scene dedicatedShadowScene;
        auto* dedicatedLayer=dedicatedShadowScene.AddObject("Dedicated shadow objects")
            ->AddComponent<TerrainObjectLayer>();
        TerrainObjectLayer::Placement placement;
        placement.asset=prefabPath;
        placement.position={180.f,0.f,180.f};
        dedicatedLayer->placements.push_back(placement);
        TerrainObjectLayer::ObjectLods lods;
        lods.meshes={prefabPath,lodPrefabPath};
        lods.shadowMeshes={shadowProxyPath,lodPrefabPath};
        lods.distances={28.f};
        dedicatedLayer->objectLods[prefabPath]=std::move(lods);
        dedicatedLayer->shadowLod=1;
        dedicatedLayer->MarkConfigurationDirty();
        preparePrefab(dedicatedLayer,dedicatedShadowScene);
        size_t visibleMeshes=0,shadowProxyMeshes=0;
        for (const auto* mesh:dedicatedShadowScene.GetComponents<Mesh>())
        {
            Check(mesh->dedicatedShadowProxy,
                "dedicated shadow proxy path was not tagged");
            if (mesh->shadowProxyOnly)
            {
                ++shadowProxyMeshes;
                Check(mesh->fixedShadowLodBand==1,
                    "dedicated shadow proxy did not keep the fixed shadow LOD");
            }
            else
            {
                ++visibleMeshes;
                Check(mesh->fixedShadowLodBand==-1,
                    "visible mesh retained shadow-only proxy selection");
            }
        }
        Check(visibleMeshes && shadowProxyMeshes,
            "dedicated shadow proxies did not emit both visible and shadow-only meshes");
    }
    // A newly appearing original path can override the bundled fallback even
    // when it deliberately preserves the same file timestamp.
    {
        const std::string name="refresh-relocation-"+std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count())+".prefab";
        const auto fallback=std::filesystem::path("Engine/Core/Assets/.local/tests-generated")/name;
        const auto original=std::filesystem::absolute(std::filesystem::path(".local/tests-generated/relocation")/
            "Engine/Core/Assets/.local/tests-generated"/name);
        Check(!std::filesystem::exists(fallback) && !std::filesystem::exists(original),"relocation fixture already exists");
        struct Cleanup
        {
            std::filesystem::path fallback,original;
            ~Cleanup() { std::error_code error; std::filesystem::remove(fallback,error); std::filesystem::remove(original,error); }
        } cleanup{fallback,original};
        std::filesystem::create_directories(fallback.parent_path());
        std::filesystem::create_directories(original.parent_path());
        writePrefab(.3f,fallback.generic_string());
        Engine::Scene::Scene relocated;
        auto* objects=relocated.AddObject("Relocated prefab")->AddComponent<TerrainObjectLayer>();
        TerrainObjectLayer::Placement placement; placement.asset=original.generic_string();
        objects->placements.push_back(placement); objects->MarkConfigurationDirty();
        const auto prepare=[&] { preparePrefab(objects,relocated); };
        const auto checkRadius=[&](float expected) {
            const auto colliders=relocated.GetComponents<PrimitiveObjectCollider>();
            Check(colliders.size()==1 && std::abs(colliders.front()->radius-expected)<.0001f,
                "prefab path override/fallback did not refresh");
        };
        prepare(); checkRadius(.3f);
        writePrefab(.6f,original.generic_string());
        std::filesystem::last_write_time(original,std::filesystem::last_write_time(fallback));
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        prepare(); checkRadius(.6f);
        std::filesystem::remove(original);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        prepare(); checkRadius(.3f);
        std::filesystem::remove(fallback);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        prepare();
        Check(relocated.GetComponents<Mesh>().empty(),"deleted prefab retained generated geometry");
        writePrefab(.9f,fallback.generic_string());
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        prepare(); checkRadius(.9f);
    }
    // A streamed cell destroys its runtime layer and restores it from authored
    // data on re-entry. Its prefab profile cache must be rebuilt for the new
    // component and retain both the placement and authored LOD chain.
    {
        Engine::Scene::Scene streamed;
        auto* manager=streamed.AddObject("Streamed terrain")->AddComponent<TerrainManager>();
        manager->cellsX=manager->cellsY=4;
        manager->cellWidth=manager->cellDepth=4.f;
        manager->loadedRadius=manager->farRadius=0;
        manager->asyncStreaming=false;
        manager->generateColliders=false;
        Check(manager->AddCell(0,0) && manager->AddCell(1,0),"streaming cells could not be created");
        manager->SyncLoadedCells(0,0);
        streamed.CommitPendingObjectChanges();
        auto* cell=manager->GetLoadedCell(0,0);
        Check(cell && cell->Owner,"first streaming cell did not load");
        auto* layerObject=streamed.AddObject("Streamed prefab layer");
        Check(streamed.SetParentLocal(layerObject,cell->Owner),"could not attach streaming layer");
        auto* streamedLayer=layerObject->AddComponent<TerrainObjectLayer>();
        streamedLayer->layerName="Streamed prefab layer";
        const std::string streamedBase=".local/tests-generated/streamed-retention-base.prefab";
        const std::string streamedLod=".local/tests-generated/streamed-retention-lod.prefab";
        writePrefab(0.f,streamedBase);
        writePrefab(0.f,streamedLod);
        TerrainObjectLayer::Placement placement;
        placement.asset=streamedBase;
        placement.position={2.f,0.f,2.f};
        streamedLayer->placements.push_back(placement);
        streamedLayer->objectLods[streamedBase]={{streamedBase,streamedLod},{},{28}};
        streamedLayer->shadowLod=1;
        streamedLayer->MarkConfigurationDirty();
        preparePrefab(streamedLayer,streamed);
        const auto hasBothBands=[&](TerrainObjectLayer* target) {
            bool found[2]={false,false};
            for (const auto* mesh:streamed.GetComponents<Mesh>())
                if (mesh->Owner && mesh->Owner->Parent==target->Owner &&
                    mesh->instanceLodBand>=0 && mesh->instanceLodBand<2)
                    found[mesh->instanceLodBand]=true;
            return found[0] && found[1];
        };
        Check(hasBothBands(streamedLayer),"streaming layer did not prepare its LOD chain");
        std::vector<std::shared_ptr<void>> retainedPrototypes;
        streamedLayer->AppendPrefabPrototypeRetention(retainedPrototypes);
        Check(retainedPrototypes.size()>=2,"streaming did not retain both prefab prototypes");
        std::weak_ptr<void> retainedBase=retainedPrototypes.front();
        manager->SyncLoadedCells(1,0);
        streamed.CommitPendingObjectChanges();
        Check(!manager->GetLoadedCell(0,0) && manager->GetLoadedCell(1,0),
            "streaming did not unload the first cell");
        manager->SyncLoadedCells(0,0);
        streamed.CommitPendingObjectChanges();
        cell=manager->GetLoadedCell(0,0);
        Check(cell && cell->Owner,"streaming did not reload the first cell");
        streamedLayer=nullptr;
        for (auto* child:cell->Owner->Children)
            if (child)
                if (auto* candidate=child->GetComponent<TerrainObjectLayer>();
                    candidate && candidate->layerName=="Streamed prefab layer")
                    streamedLayer=candidate;
        Check(streamedLayer && streamedLayer->placements.size()==1,
            "streaming did not restore the prefab placement");
        preparePrefab(streamedLayer,streamed);
        Check(hasBothBands(streamedLayer),"streaming lost the authored LOD chain");
        retainedPrototypes.clear();
        Check(!retainedBase.expired(),"restored layer did not reuse its retained prefab prototype");
    }
    Engine::Scene::Scene forest;
    Check(Engine::Serialization::SceneSerializer::Load(forest,
        "Engine/Core/Assets/Scenes/terrain/oak_meadow.scene",nullptr),"headless oak load failed");
    const auto forestLayers=forest.GetComponents<TerrainObjectLayer>();
    size_t placements=0;
    for (auto* objects:forestLayers)
    {
        if (objects->layerName=="Oak forest") placements=objects->placements.size();
        preparePrefab(objects,forest);
    }
    forest.CommitPendingObjectChanges();
    Check(placements==2048,"headless forest lost authored placements");
    std::set<const Mesh*> sources;
    size_t instanceParts=0,batches=0;
    std::set<const Mesh::InstanceData*> placementArrays;
    for (auto* mesh:forest.GetComponents<Mesh>()) if (mesh->IsInstanced())
    {
        ++batches; sources.insert(mesh->GetInstanceSource());
        if (placementArrays.insert(mesh->GetInstanceData().get()).second)
            instanceParts+=mesh->GetInstanceData()->transforms.size();
        for (const auto& placement:mesh->GetInstanceData()->transforms)
        {
            const auto point=mesh->Owner->transform.GetWorldMatrix()*glm::vec4(
                placement.rows[0].w,placement.rows[1].w,placement.rows[2].w,1);
            Check(std::abs(point.x)<180 && std::abs(point.z)<180,"forest placement shifted outside terrain");
        }
    }
    Check(instanceParts==2048 && sources.size()==85 && batches>sources.size(),
        "forest did not share bark/leaf sources across spatial tiles");
    Check(batches<400,"forest expanded shared geometry into too many CPU mesh components");
    for (auto* collider:forest.GetComponents<PrimitiveObjectCollider>())
        Check(collider->Owner->name=="Meadow Explorer","unexpected tree collider in forest");
    for (auto* body:forest.GetComponents<RigidBody>())
    {
        auto* objects=body->Owner->GetComponentInParent<TerrainObjectLayer>();
        Check(!objects || objects->layerName!="Oak forest","forest still has per-tree rigid bodies");
    }
    std::cout << "Forest: " << placements << " placements; " << sources.size()
        << " shared source meshes; " << batches << " spatial batches; no tree colliders.\n";
    std::cout << "Shared geometry, transformed placements, serialization, collider addition/edit/removal passed.\n";
}

template<class Renderer> void CaptureOak(const std::string& backend)
{
    const char* requestedScale=std::getenv("ENGINE_OAK_CAPTURE_SCALE");
    const std::string scaleText=requestedScale ? requestedScale:"1";
    Check(scaleText=="0.5" || scaleText=="1" || scaleText=="2","oak capture scale must be 0.5, 1 or 2");
    const float scale=std::stof(scaleText);
    const unsigned width=unsigned(960*scale),height=unsigned(540*scale);
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Actual oak meadow validation",width,height);
    ShowWindow(window.GetHWND(),SW_HIDE);
    Renderer renderer;
    Check(renderer.Init(window.GetHWND(),width,height),"oak renderer init failed");
    if constexpr (std::is_same_v<Renderer,DX11GameRenderer>)
    {
        std::vector<uint8_t> pixels;
        Check(!renderer.ReadSceneCaptureRGBA(pixels),"DX11 capture was ready before a request");
        for(unsigned channel=0;channel<3;++channel)
        {
            renderer.BeginFrame();
            renderer.Clear(channel==0 ? 1.f:0.f,channel==1 ? 1.f:0.f,channel==2 ? 1.f:0.f,1);
            renderer.RequestSceneCapture();
            Check(!renderer.ReadSceneCaptureRGBA(pixels),"DX11 returned an old requested frame");
            renderer.EndFrame();
            Check(renderer.ReadSceneCaptureRGBA(pixels),"DX11 current-frame capture failed");
            const size_t center=(width*(height/2)+width/2)*4;
            Check(pixels.size()==width*height*4 && pixels[center+channel]>pixels[center+(channel+1)%3]+20,
                "DX11 screenshot did not contain the just-composed color");
        }
    }
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> validation;
    if constexpr (std::is_same_v<Renderer,DX12GameRenderer>)
        if(std::getenv("ENGINE_OAK_DX12_VALIDATION"))
            Check(SUCCEEDED(renderer.GetDevice()->QueryInterface(IID_PPV_ARGS(&validation))),
                "DX12 validation queue unavailable");
    auto checkValidation=[&] {
        if(!validation) return;
        bool failed=false;
        for(UINT64 i=0;i<validation->GetNumStoredMessages();++i)
        {
            SIZE_T bytes=0; validation->GetMessage(i,nullptr,&bytes);
            std::vector<uint8_t> storage(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if(SUCCEEDED(validation->GetMessage(i,message,&bytes)) &&
                message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)
            { std::cerr << "DX12 validation " << message->ID << ": " << message->pDescription << '\n'; failed=true; }
        }
        validation->ClearStoredMessages();
        Check(!failed,"DX12 validation reported an invalid submission");
    };
    Engine::Scene::Scene scene; scene.Init(renderer.GetGraphicsProvider());
    scene.SetBeforeGpuResourceDestruction([&] { renderer.WaitIdle(); });
    Check(scene.Load("Engine/Core/Assets/Scenes/terrain/oak_meadow.scene"),"oak scene load failed");
    auto* viewer=scene.FindObjectByName("Meadow Explorer");
    Check(viewer!=nullptr,"authored oak viewer missing");
    auto* camera=viewer->GetComponent<Camera>();
    Check(camera!=nullptr,"authored oak camera missing");
    const auto entrancePosition=viewer->transform.position;
    const auto entranceTarget=camera->target;
    const auto fields=scene.GetComponents<GrassField>();
    Check(!fields.empty(),"oak grass scripts missing");
    for (auto* field:fields) { field->useEditorCamera=false; field->Start(); }
    const auto terrains=scene.GetComponents<Terrain>();
    Check(!terrains.empty(),"oak terrain missing");
    auto* terrain=terrains.front();
    const char* requestedWalkView=std::getenv("ENGINE_OAK_WALK_VIEW");
    const std::string walkView=requestedWalkView ? requestedWalkView:"overlook";
    Check(walkView=="entrance" || walkView=="overlook","unknown oak walking view");
    const bool screenLodDiagnostic=std::getenv("ENGINE_OAK_SCREEN_LOD")!=nullptr;
    const char* canopyRoot=std::getenv("ENGINE_OAK_CANOPY_ROOT");
    const char* requestedOutputRoot=std::getenv("ENGINE_OAK_OUTPUT_ROOT");
    bool screenPolicyEnabled=false, screenDistanceOverride=false;
    const std::string outputRoot=requestedOutputRoot ? std::string(requestedOutputRoot)+"/"+backend+"/"+scaleText : canopyRoot ? std::string(canopyRoot)+"/renderer/"+backend+"/"+scaleText : screenLodDiagnostic ?
        ".local/screen-lod-validation/"+backend+"/"+scaleText : std::string(".local/tree-lod-oak-validation")+
        (walkView=="entrance" ? "/entrance":"")+
        (scale==1 ? "":"/"+std::to_string(width)+"x"+std::to_string(height));
    std::filesystem::create_directories(outputRoot);
    std::ofstream report(outputRoot+"/"+backend+".log");
    report << "Source: Engine/Core/Assets/Scenes/terrain/oak_meadow.scene\n"
        << "Actual scene; authored entrance camera and established overlook camera; "
        << width << 'x' << height << "\n";
    bool refreshGrass=true;
    float renderDelta=0.f;
    bool terrainOnlyColor=false;
    bool diagnosticOakWind=false, oakOnlyColor=false;
    unsigned diagnosticWindMeshes=0;
    bool forceShadowRefresh=false;
    bool partialDepthClearSupported=false;
    const bool groundCapture=std::getenv("ENGINE_OAK_GROUND_CAPTURE")!=nullptr;
    glm::vec3 shadowLookOffset(0);
    std::vector<std::pair<std::string,std::vector<uint8_t>>> capturedImages;
    auto render=[&] {
        try {
        // This direct-context screenshot harness does not retain recorded
        // packets or rotate Scene's native buffers like the Editor pipeline.
        // Complete the previous submission before mutating shared resources.
        renderer.WaitIdle();
        checkValidation();
        scene.AdvanceManualFrame(renderDelta);
        scene.ResetGrassPerformance();
        if (refreshGrass) for (auto* field:fields) field->RefreshEditorPreview();
        diagnosticWindMeshes=0;
        if(screenLodDiagnostic)
            for(auto* mesh:scene.GetComponents<Mesh>()) if(mesh->instanceLodBand>=0)
            {
                mesh->screenLodSelection=screenPolicyEnabled;
                mesh->distanceLodOverride=screenDistanceOverride;
                mesh->screenLod1Pixels=300; mesh->screenLod2Pixels=80;
            }
        if(diagnosticOakWind)
            for(auto* mesh:scene.GetComponents<Mesh>()) if(mesh->instanceLodBand>=0)
            { mesh->foliageWindStrength=.5f; ++diagnosticWindMeshes; }
        const auto receiverTarget=camera->target;
        if(glm::length(shadowLookOffset)>0.f)
            camera->target=viewer->transform.position+glm::dvec3(shadowLookOffset);
        renderer.BeginFrame(); scene.PrepareRenderFrame();
        auto context=renderer.CreateFrameGraphicsContext();
        partialDepthClearSupported=context->SupportsDepthRectangleClear();
        context->SetViewport({0,0,float(width),float(height),0,1});
        context->SetScissorRect({0,0,int(width),int(height)});
        if(forceShadowRefresh) scene.InvalidateDirectionalShadowCache();
        scene.Render(context.get(),float(width)/height,camera,false,width,height,true);
        camera->target=receiverTarget;
        if (terrainOnlyColor || oakOnlyColor || std::is_same_v<Renderer,DX12GameRenderer>)
        {
            renderer.EndFrame(); checkValidation(); renderer.WaitIdle();
            renderer.BeginFrame(); context=renderer.CreateFrameGraphicsContext();
        }
        std::vector<Mesh*> hiddenMeshes;
        if(terrainOnlyColor || oakOnlyColor)
        {
            for(auto* mesh:scene.GetComponents<Mesh>())
                if(mesh->IsEditorVisible() && !mesh->Owner->GetComponent<Terrain>() &&
                    !(oakOnlyColor && mesh->instanceLodBand>=0))
                { mesh->SetEditorVisible(false); hiddenMeshes.push_back(mesh); }
            // Shadow commands have completed before replacing the prepared
            // draw list or its shared buffers. The atlas remains unchanged.
            scene.PrepareRenderFrame();
        }
        context->SetViewport({0,0,float(width),float(height),0,1});
        context->SetScissorRect({0,0,int(width),int(height)});
        renderer.Clear(.1f,.1f,.1f,1);
        const bool includeEditorVisuals=
            std::getenv("ENGINE_OAK_SELECTION_RENDER_CHECK")!=nullptr;
        scene.Render(context.get(),float(width)/height,camera,
            includeEditorVisuals,width,height);
        renderer.RequestSceneCapture();
        renderer.EndFrame();
        for(auto* mesh:hiddenMeshes) mesh->SetEditorVisible(true);
        checkValidation();
        } catch(...) {
            if constexpr (std::is_same_v<Renderer,DX12GameRenderer>)
            {
                std::cerr << "DX12 removal reason: " << std::hex <<
                    unsigned(renderer.GetDevice()->GetDeviceRemovedReason()) << std::dec << '\n';
                checkValidation();
            }
            throw;
        }
    };
    auto capture=[&](const std::string& name,int frames) {
        for (int i=0;i<frames;++i) render();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);
        auto texturesReady=[&] {
            bool ready=true;
            for (auto* material:scene.GetComponents<Material>())
                for (const auto& texture : {material->baseColorTexture,material->normalTexture,
                    material->metallicRoughnessTexture,material->occlusionTexture,
                    material->emissiveTexture,material->heightTexture})
                    if (texture && !texture->Prepare(renderer.GetGraphicsProvider())) ready=false;
            return ready;
        };
        while (!texturesReady())
        {
            Check(std::chrono::steady_clock::now()<deadline,"oak material textures did not become GPU-ready");
            render();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Refill flip-model buffers and refresh material bindings after uploads.
        for (int i=0;i<4;++i) render();
        std::vector<uint8_t> pixels;
        Check(renderer.ReadSceneCaptureRGBA(pixels),"oak capture failed");
        Check(pixels.size()==width*height*4,"oak capture size wrong");
        // Artifact writes can trigger the project's shader watcher. Publish
        // screenshots only after all GPU validation has completed.
        capturedImages.emplace_back(name,pixels);
        for (auto* material:scene.GetComponents<Material>())
            if (!material->shaderError.empty())
                throw std::runtime_error("oak material shader error: " + material->shaderError);
        Check(scene.GetLastRenderCpuTimings().instanceTileBuilt<32,
            "shadow/color LOD metadata cache rebuilt static forest tiles");
        const auto& shadow=scene.GetShadowDebugSnapshot();
        size_t fixedProxies=0;
        for(auto* mesh:scene.GetComponents<Mesh>()) if(mesh->instanceLodBand>=0)
        {
            Check(mesh->fixedShadowLodBand==1,"authored forest does not pin its shadow proxy");
            Check(mesh->instanceLodDistances==glm::vec2(20,60),"fixed shadows changed visible tree LODs");
            fixedProxies+=mesh->instanceLodBand==1;
        }
        Check(shadow.casterCount<=fixedProxies,"non-proxy forest bands entered shadow preparation");
        report << name << ": selectedShadow=" << shadow.hasSelectedLight << " casters="
            << shadow.submittedCasterDrawCount << " grassFields=" << fields.size() << std::endl;
        return pixels;
    };
    if(std::getenv("ENGINE_OAK_SELECTION_RENDER_CHECK"))
    {
        scene.SetSelectedObject(nullptr);
        scene.SetSelectedPlacementId(0);
        const auto baseline=capture("selection-none",8);
        TerrainObjectLayer* selectedLayer=nullptr;
        Mesh* selectedMesh=nullptr;
        uint64_t selectedId=0;
        float nearest=std::numeric_limits<float>::max();
        const glm::vec3 cameraPosition(viewer->transform.GetWorldPosition());
        const glm::vec3 cameraForward=glm::normalize(camera->target-cameraPosition);
        for(auto* layer:scene.GetComponents<TerrainObjectLayer>())
        {
            if(layer->layerName!="Oak forest") continue;
            for(auto* mesh:scene.GetComponents<Mesh>())
            {
                if(mesh->Owner->GetComponentInParent<TerrainObjectLayer>()!=layer)
                    continue;
                if(!mesh->IsInstanced()) continue;
                const auto& data=mesh->GetInstanceData();
                if(!data || data->placementIds.empty()) continue;
                for(size_t i=0;i<data->placementIds.size() && i<data->transforms.size();++i)
                {
                    if(data->placementIds[i]==0) continue;
                    const auto& transform=data->transforms[i];
                    const glm::vec3 local(transform.rows[0].w,
                        transform.rows[1].w,transform.rows[2].w);
                    const glm::vec3 world(mesh->Owner->transform.GetWorldMatrix()*
                        glm::vec4(local,1.f));
                    const glm::vec3 offset=world-cameraPosition;
                    const float depth=glm::dot(offset,cameraForward);
                    if(depth<=0.f) continue;
                    const float distance=glm::dot(offset,offset);
                    if(distance<nearest)
                    { nearest=distance; selectedLayer=layer; selectedMesh=mesh;
                      selectedId=data->placementIds[i]; }
                }
            }
        }
        Check(selectedLayer && selectedMesh && selectedId,
            "oak selection render check found no forward foliage placement");
        scene.SetSelectedObject(selectedMesh->Owner);
        scene.SetSelectedPlacementId(selectedId,selectedLayer->Owner);
        const auto selected=capture("selection-one-placement",8);
        size_t changedPixels=0, outlinePixels=0;
        for(size_t i=0;i<baseline.size();i+=4)
        {
            if(std::abs(int(baseline[i])-int(selected[i]))>12 ||
                std::abs(int(baseline[i+1])-int(selected[i+1]))>12 ||
                std::abs(int(baseline[i+2])-int(selected[i+2]))>12)
                ++changedPixels;
            if(selected[i]>170 && selected[i+1]>100 && selected[i+2]<100 &&
                !(baseline[i]>170 && baseline[i+1]>100 && baseline[i+2]<100))
                ++outlinePixels;
        }
        report << "one-placement editor outline changed pixels=" << changedPixels
            << " yellow outline pixels=" << outlinePixels << '\n';
        Check(outlinePixels>100 && outlinePixels<size_t(width)*height/20,
            "selected oak placement outline is missing or spans too much of the forest");
        scene.SetSelectedObject(nullptr);
        scene.SetSelectedPlacementId(0);
    }
    if(canopyRoot)
    {
        // Isolated experiment: change resident middle geometry in memory only.
        // Authored 20/60 m, materials, placements and shadow policy are retained.
        forceShadowRefresh=true;
        auto sequence=[&](const std::string& variant) {
            for(const std::string view : {"entrance","overlook"}) for(float fov : {60.f,30.f})
            {
                const glm::dvec3 position=view=="entrance" ? entrancePosition:glm::dvec3(-5,27,-39);
                const glm::dvec3 target=view=="entrance" ? glm::dvec3(entranceTarget):glm::dvec3(2,1,5);
                camera->fov=fov;
                for(int frame=0;frame<12;++frame)
                {
                    const glm::dvec3 shift(frame*.35,0,0);
                    viewer->transform.position=position+shift; camera->target=target+shift;
                    const auto prefix=view+"-fov"+std::to_string(int(fov))+"-"+std::to_string(frame);
                    capture(prefix+"-"+variant,frame==0 ? 12:1);
                    if(frame==0) capture(prefix+"-"+variant+"-repeat",2);
                }
            }
        };
        for(auto* layer:scene.GetComponents<TerrainObjectLayer>())
            if(layer->layerName=="Oak forest") ConfigureCanopy(*layer,canopyRoot,false,&report);
        capture("warmup",90); refreshGrass=false;
        sequence("baseline");
        unsigned replaced=0;
        for(auto* layer:scene.GetComponents<TerrainObjectLayer>())
        {
            if(layer->layerName!="Oak forest") continue;
            for(auto& [asset,chain]:layer->objectLods)
                for(int variety=1;variety<=4;++variety)
                {
                    const std::string name="Large_oak_tree_00"+std::to_string(variety);
                    if(asset.find(name+"_LOD0.prefab")==std::string::npos) continue;
                    Check(chain.meshes.size()==3,"canopy experiment requires authored three-band chain");
                    const auto path=std::filesystem::path(canopyRoot)/name/(name+"_CanopyCandidate.prefab");
                    if(!std::filesystem::exists(path)) continue; // permit a one-variety pilot
                    Engine::Scene::Scene candidateValidation;
                    auto* candidateRoot=Engine::Serialization::SceneSerializer::InstantiatePrefab(
                        candidateValidation,path.generic_string(),nullptr);
                    Check(candidateRoot!=nullptr,"canopy candidate prefab failed to instantiate");
                    const auto candidateMeshes=candidateRoot->GetComponentsInChildren<Mesh>();
                    Check(candidateMeshes.size()==2,"canopy candidate must load both bark and leaf components");
                    size_t candidateTriangles=0;
                    for(auto* mesh:candidateMeshes)
                    {
                        Check(!mesh->GetVertices().empty(),"canopy candidate loaded empty geometry");
                        candidateTriangles+=(mesh->GetIndices().empty() ? mesh->GetVertices().size():mesh->GetIndices().size())/3;
                    }
                    report << name << " validatedCandidateTriangles=" << candidateTriangles << '\n';
                    chain.meshes[1]=path.generic_string(); ++replaced;
                }
            layer->MarkConfigurationDirty();
        }
        Check(replaced>0,"canopy experiment did not replace any large oaks");
        report << "Canopy candidate varieties replaced=" << replaced << '\n';
        sequence("candidate");
        renderer.WaitIdle(); checkValidation();
        for(const auto& [name,pixels]:capturedImages)
        {
            std::ofstream image(outputRoot+"/"+name+".ppm",std::ios::binary);
            image << "P6\n" << width << ' ' << height << "\n255\n";
            for(size_t i=0;i<pixels.size();i+=4) image.write(reinterpret_cast<const char*>(pixels.data()+i),3);
        }
        report << "Canopy experiment captured; no visual acceptance or performance claim.\n";
        scene.ClearObjects(); renderer.WaitIdle(); return;
    }
    if(screenLodDiagnostic)
    {
        // Serialized authoring and prepared-layer publication must retain policy.
        TerrainObjectLayer authored;
        authored.screenLodSelection=true; authored.distanceLodOverride=true;
        authored.screenLod1Pixels=450; authored.screenLod2Pixels=110; authored.screenLodHysteresis=.15f;
        TerrainObjectLayer restored; restored.Deserialize(authored.Serialize());
        Check(restored.screenLodSelection && restored.distanceLodOverride && restored.screenLod1Pixels==450 &&
            restored.screenLod2Pixels==110 && restored.screenLodHysteresis==.15f,"screen policy serialization");
        TerrainObjectLayer published; published.InstallPrepared(std::move(restored));
        Check(published.screenLodSelection && published.screenLod1Pixels==450,"prepared screen policy publication");
        Mesh serializedMesh; serializedMesh.screenLodSelection=true; serializedMesh.distanceLodOverride=true;
        serializedMesh.screenLod1Pixels=444; Mesh restoredMesh; restoredMesh.Deserialize(serializedMesh.Serialize());
        Check(restoredMesh.screenLodSelection && restoredMesh.distanceLodOverride && restoredMesh.screenLod1Pixels==444,
            "mesh screen policy serialization");
        forceShadowRefresh=true;
        capture("warmup",90);
        refreshGrass=false;
        for(const std::string viewName : {"entrance","overlook"})
        {
            viewer->transform.position=viewName=="entrance" ? entrancePosition:glm::dvec3(-5,27,-39);
            camera->target=viewName=="entrance" ? entranceTarget:glm::dvec3(2,1,5);
            for(float fov : {60.f,30.f})
            {
                camera->fov=fov;
                const std::string prefix=viewName+"-fov"+std::to_string(int(fov));
                screenPolicyEnabled=false;
                const auto baseline=capture(prefix+"-distance",viewName=="entrance" ? 20:2);
                const auto baselineRepeat=capture(prefix+"-distance-repeat",2);
                screenPolicyEnabled=true; screenDistanceOverride=true;
                const auto overridden=capture(prefix+"-override",2);
                const auto changedPixels=[&](const auto& first,const auto& second) {
                    size_t changed=0;
                    for(size_t pixel=0;pixel<first.size();pixel+=4)
                        if(std::abs(int(first[pixel])-int(second[pixel]))>3 ||
                            std::abs(int(first[pixel+1])-int(second[pixel+1]))>3 ||
                            std::abs(int(first[pixel+2])-int(second[pixel+2]))>3) ++changed;
                    return changed;
                };
                const size_t repeatNoise=changedPixels(baseline,baselineRepeat);
                const size_t overrideDelta=changedPixels(baselineRepeat,overridden);
                report << prefix << " unchangedRepeatPixels=" << repeatNoise << " overridePixels=" << overrideDelta << std::endl;
                Check(overrideDelta<=repeatNoise+width*height/1000,"distance override changed the authored forest image beyond unchanged-repeat noise");
                screenDistanceOverride=false;
                const auto projected=capture(prefix+"-screen",2);
                const auto counts=scene.GetScreenLodCounts();
                Check(counts[0]+counts[1]+counts[2]>512,"screen renderer lost distinct placement sets");
                report << prefix << " screenLevels=" << counts[0] << ',' << counts[1] << ',' << counts[2] << '\n';
                scene.gpuInstanceCulling=false;
                const auto cpu=capture(prefix+"-screen-cpu",2);
                Check(scene.GetScreenLodCounts()==counts,"CPU fallback changed projected LOD choices");
                size_t changed=0;
                for(size_t i=0;i<cpu.size();i+=4)
                    if(std::abs(int(cpu[i])-int(projected[i]))>3 || std::abs(int(cpu[i+1])-int(projected[i+1]))>3 ||
                        std::abs(int(cpu[i+2])-int(projected[i+2]))>3) ++changed;
                Check(changed<width*height/100,"CPU/GPU screen LOD images differ by more than one percent");
                scene.gpuInstanceCulling=true;
                terrainOnlyColor=true;
                screenPolicyEnabled=false; const auto shadowReference=capture(prefix+"-ground-distance",2);
                screenPolicyEnabled=true; const auto shadowScreen=capture(prefix+"-ground-screen",2);
                const auto shadowRepeat=capture(prefix+"-ground-repeat",2);
                const size_t shadowNoise=changedPixels(shadowScreen,shadowRepeat);
                const size_t shadowDelta=changedPixels(shadowReference,shadowScreen);
                report << prefix << " unchangedGroundPixels=" << shadowNoise << " shadowPixels=" << shadowDelta << std::endl;
                Check(shadowDelta<=shadowNoise+width*height/1000,"visible screen LOD changed ground shadows beyond unchanged-repeat noise");
                terrainOnlyColor=false;
            }
        }
        camera->fov=60;
        const auto motionStart=viewer->transform.position;
        const auto motionTarget=camera->target;
        for(int frame=0;frame<12;++frame)
        {
            viewer->transform.position=motionStart+glm::dvec3(frame*.35,0,0);
            camera->target=motionTarget+glm::vec3(frame*.35f,0,0);
            screenPolicyEnabled=false;
            capture("motion-"+std::to_string(frame)+"-distance",1);
            screenPolicyEnabled=true;
            capture("motion-"+std::to_string(frame)+"-screen",1);
        }
        for(auto* field:fields)
            if(auto* grassLayer=field->Owner->GetComponent<TerrainObjectLayer>())
            { grassLayer->screenLodSelection=true; grassLayer->screenLod1Pixels=222; grassLayer->MarkConfigurationDirty(); }
        refreshGrass=true;
        capture("grass-layer-screen-policy",8);
        size_t screenGrassTiles=0;
        for(auto* generated:scene.GetComponents<Mesh>())
            if(generated->usesGrassShadowBudget && generated->screenLodSelection && generated->screenLod1Pixels==222)
                ++screenGrassTiles;
        Check(screenGrassTiles>0,"scripted grass tiles did not inherit the layer screen policy");
        renderer.WaitIdle(); checkValidation();
        for(const auto& [name,pixels]:capturedImages)
        {
            std::ofstream image(outputRoot+"/"+name+".ppm",std::ios::binary);
            image << "P6\n" << width << ' ' << height << "\n255\n";
            for(size_t i=0;i<pixels.size();i+=4) image.write(reinterpret_cast<const char*>(pixels.data()+i),3);
        }
        report << "Screen LOD forest comparison passed; authored 20/60 m and fixed shadow LOD remain unchanged.\n";
        scene.ClearObjects(); renderer.WaitIdle(); return;
    }
    for (const std::string view : {"entrance","overlook"})
    {
        if (view=="overlook") { viewer->transform.position={-5,27,-39}; camera->target={2,1,5}; }
        capture(view,view=="entrance" ? 45:12);
        Check(scene.GetShadowDebugSnapshot().hasSelectedLight,"authored oak sun shadows are disabled");
        Check(scene.GetShadowDebugSnapshot().submittedCasterDrawCount>0,"authored oak shadow casters missing");
        struct SavedLight { Light* light; bool casts; float resolution; };
        std::vector<SavedLight> savedLights;
        for (auto* light:scene.GetComponents<Light>())
        { savedLights.push_back({light,light->castsShadows,light->shadowResolutionScale}); light->castsShadows=false; }
        const auto noShadows=capture(view+"-diagnostic-shadows-off",4);
        for (const auto& saved:savedLights)
            if (saved.light->GetLightType()==Light::Type::Directional)
            { saved.light->castsShadows=saved.casts; saved.light->shadowResolutionScale=saved.resolution; }
        const auto normal=capture(view+"-diagnostic-shadows-on",8);
        Check(scene.GetShadowDebugSnapshot().hasSelectedLight,"oak diagnostic directional light was not shadowed");
        Check(scene.GetShadowDebugSnapshot().submittedCasterDrawCount>0,"oak diagnostic shadow casters missing");
        size_t darker=0;
        for (size_t i=0;i<normal.size();i+=4)
            darker+=int(noShadows[i])+int(noShadows[i+1])+int(noShadows[i+2])>
                int(normal[i])+int(normal[i+1])+int(normal[i+2])+15;
        report << view << ": directional shadow darkened pixels=" << darker << std::endl;
        Check(darker>100,"oak directional shadows had no visible effect");
        for (const auto& saved:savedLights)
        { saved.light->castsShadows=saved.casts; saved.light->shadowResolutionScale=saved.resolution; }
        // Override the authored material sidedness to prove terrain still culls
        // its underside, using the actual scene with other objects hidden.
        std::vector<Engine::Core::Object*> hidden;
        refreshGrass=false;
        for (const auto& object:scene.GetObjects())
            if (object.get()!=terrain->Owner && object.get()!=viewer && object->enabled)
            { hidden.push_back(object.get()); object->enabled=false; }
        const auto savedPosition=viewer->transform.position;
        const auto savedTarget=camera->target;
        viewer->transform.position={0,-25,0}; camera->target={0,0,.1f};
        auto* terrainMaterial=terrain->Owner->GetComponent<Material>();
        const bool savedSidedness=terrainMaterial && terrainMaterial->doubleSided;
        if (terrainMaterial) terrainMaterial->doubleSided=true;
        terrain->Owner->enabled=false;
        const auto background=capture(view+"-diagnostic-background",4);
        terrain->Owner->enabled=true;
        const auto underside=capture(view+"-diagnostic-terrain-underside",4);
        size_t undersideDifferences=0;
        for (size_t i=0;i<underside.size();++i)
            undersideDifferences+=std::abs(int(underside[i])-int(background[i]))>2;
        report << view << ": isolated underside differing channels=" << undersideDifferences << std::endl;
        Check(undersideDifferences==0,"actual oak terrain underside rendered");
        if (terrainMaterial) terrainMaterial->doubleSided=savedSidedness;
        for (auto* object:hidden) object->enabled=true;
        viewer->transform.position=savedPosition; camera->target=savedTarget;
        refreshGrass=true;
    }
    const auto reference=capture("stable-look-reference",8);
    const auto referenceShadow=scene.GetShadowDebugSnapshot();
    for(const auto look: {glm::vec3(1,0,0),glm::vec3(0,0,-1)})
    {
        shadowLookOffset=look;
        const auto rotated=capture(look.x>0 ? "stable-look-side":"stable-look-back",8);
        const auto rotatedShadow=scene.GetShadowDebugSnapshot();
        for(uint32_t cascade=0;cascade<referenceShadow.cascadeCount;++cascade)
        {
            Check(glm::length(rotatedShadow.cascadeCenters[cascade]-
                referenceShadow.cascadeCenters[cascade])<.0001f,
                "looking around moved a world-space shadow cascade");
            Check(std::abs(rotatedShadow.cascadeRadii[cascade]-
                referenceShadow.cascadeRadii[cascade])<.0001f,
                "looking around resized a world-space shadow cascade");
        }
        size_t changedPixels=0;
        for(size_t i=0;i<reference.size();i+=4)
            changedPixels+=std::abs(int(reference[i])-int(rotated[i]))>5 ||
                std::abs(int(reference[i+1])-int(rotated[i+1]))>5 ||
                std::abs(int(reference[i+2])-int(rotated[i+2]))>5;
        report << "fixed receiver after shadow-camera rotation: changed pixels="
            << changedPixels << std::endl;
        Check(changedPixels<width*height/500,
            "turning the shadow camera visibly changed a fixed receiver view");
    }
    shadowLookOffset=glm::vec3(0);
    for(int warm=0;warm<4;++warm) render();
    auto previous=scene.GetShadowDebugSnapshot();
    uint32_t refreshed=0;
    for(int frame=0;frame<6;++frame)
    {
        const glm::vec3 step(.2f,0,0);
        viewer->transform.position+=step; camera->target+=step+glm::vec3(.1f,0,0);
        render();
        const auto current=scene.GetShadowDebugSnapshot();
        uint32_t changed=0;
        for(uint32_t cascade=0;cascade<current.cascadeCount;++cascade)
            if(glm::length(current.cascadeCenters[cascade]-previous.cascadeCenters[cascade])>.01f)
            { ++changed; refreshed|=1u<<cascade; }
        Check(changed>0 && (partialDepthClearSupported ? changed<=2:changed==current.cascadeCount),
            "small camera movement did not use the supported cascade refresh policy");
        previous=current;
    }
    Check(refreshed==((1u<<previous.cascadeCount)-1u),"far shadow cascade refresh starved");
    viewer->transform.position+=glm::vec3(5,0,0); camera->target+=glm::vec3(5,0,0);
    render();
    const auto cut=scene.GetShadowDebugSnapshot();
    uint32_t changed=0;
    for(uint32_t cascade=0;cascade<cut.cascadeCount;++cascade)
        changed+=glm::length(cut.cascadeCenters[cascade]-previous.cascadeCenters[cascade])>.01f;
    Check(changed==cut.cascadeCount,"camera cut did not refresh the entire shadow atlas");
    camera->fov*=.75f; render();
    const auto zoom=scene.GetShadowDebugSnapshot();
    for(uint32_t cascade=0;cascade<zoom.cascadeCount;++cascade)
        Check(std::abs(zoom.cascadeRadii[cascade]-cut.cascadeRadii[cascade])<.0001f,
            "zoom changed world-space radial shadow coverage");
    renderer.WaitIdle();
    checkValidation();
    Light* sun=nullptr;
    for(auto* light:scene.GetComponents<Light>())
        if(light->GetLightType()==Light::Type::Directional && light->castsShadows)
        { sun=light; break; }
    Check(sun!=nullptr,"shadow invalidation test has no sun");
    const auto savedSunRotation=sun->Owner->transform.rotation;
    for(int frame=0;frame<3;++frame)
    {
        render();
        const auto stationary=scene.GetShadowDebugSnapshot();
        Check(!partialDepthClearSupported || stationary.refreshedCascadeMask!=((1u<<stationary.cascadeCount)-1u),
            "unchanged sun unnecessarily invalidated every cascade");
    }
    for(int step=0;step<3;++step)
    {
        const auto oldDirection=scene.GetShadowDebugSnapshot().directionToLight;
        // Small changes used to pass the cosine-based cache-reuse threshold,
        // leaving retained cascades with a different light-space orientation.
        sun->Owner->transform.rotation.x+=glm::radians(.05f);
        render();
        const auto changedSun=scene.GetShadowDebugSnapshot();
        Check(glm::length(changedSun.directionToLight-oldDirection)>1.e-5f,
            "sun regression did not change the selected light direction");
        report << "small sun step " << step << ": refreshed mask="
            << changedSun.refreshedCascadeMask << std::endl;
        Check(changedSun.refreshedCascadeMask==((1u<<changedSun.cascadeCount)-1u),
            "small sun direction change retained stale shadow cascade tiles");
    }
    sun->Owner->transform.rotation=savedSunRotation;
    render();
    const auto restoredSun=scene.GetShadowDebugSnapshot();
    Check(restoredSun.refreshedCascadeMask==((1u<<restoredSun.cascadeCount)-1u),
        "restoring the sun retained stale shadow cascade tiles");
    // Repeat identical camera positions with a frozen and an advancing wind
    // clock. Read each frame immediately; capture() would render extra frames
    // and hide the retained-tile state we intend to validate.
    if(walkView=="entrance")
    { viewer->transform.position=entrancePosition; camera->target=entranceTarget; }
    report << "Walking view: " << walkView << std::endl;
    const auto walkStart=viewer->transform.position;
    const auto walkTarget=camera->target;
    const auto walkDirection=glm::normalize(glm::vec3(.8f,0,.6f));
    terrainOnlyColor=groundCapture;
    const std::string walkPrefix=groundCapture ? "walk-ground-":"walk-";
    std::ostringstream walkTrace;
    walkTrace << std::setprecision(9);
    // Preparation derives generated mesh ranges from their layer. Override
    // that source policy, not transient meshes which may be rebuilt.
    std::vector<std::pair<TerrainObjectLayer*,float>> savedCasterRanges;
    for(auto* layer:scene.GetComponents<TerrainObjectLayer>())
        if(layer->layerName=="Oak forest") savedCasterRanges.emplace_back(layer,layer->shadowDistance);
    Check(!savedCasterRanges.empty(),"forest caster-range diagnostic has no layer");
    constexpr float unrestrictedRange=100000.f;
    std::vector<float> savedFadeWidths;
    for(const auto& saved:savedCasterRanges) savedFadeWidths.push_back(saved.first->shadowFadeWidth);
    const std::vector<std::string> walkPhases=groundCapture
        ? std::vector<std::string>{"frozen","unrestricted","fully-refreshed","unrestricted-full","frozen-repeat",
            "fade4","fade4-full","fade8","fade8-full","animated"}
        : std::vector<std::string>{"frozen","animated"};
    for(const auto& phase:walkPhases)
    {
        const bool animated=phase=="animated";
        const bool unrestricted=phase=="unrestricted" || phase=="unrestricted-full";
        forceShadowRefresh=phase=="fully-refreshed" || phase=="unrestricted-full" ||
            phase=="fade4-full" || phase=="fade8-full";
        size_t layerIndex=0;
        for(const auto& saved:savedCasterRanges)
        {
            const float range=unrestricted ? unrestrictedRange:saved.second;
            const float fade=phase.rfind("fade4",0)==0 ? 4.f:
                phase.rfind("fade8",0)==0 ? 8.f:savedFadeWidths[layerIndex];
            ++layerIndex;
            if(saved.first->shadowDistance!=range || saved.first->shadowFadeWidth!=fade)
            { saved.first->shadowDistance=range; saved.first->shadowFadeWidth=fade; saved.first->MarkConfigurationDirty(); }
        }
        viewer->transform.position=walkStart;
        camera->target=walkTarget;
        renderDelta=0.f;
        for(int warm=0;warm<8;++warm) render();
        std::array<unsigned,4> age{};
        unsigned retainedFrames=0;
        const auto initial=scene.GetShadowDebugSnapshot();
        std::array<glm::vec3,4> anchors{};
        for(unsigned cascade=0;cascade<initial.cascadeCount;++cascade)
            anchors[cascade]=glm::vec3(walkStart)+walkDirection*(initial.cascadeSplits[cascade]+1.f);
        std::array<bool,4> crossed{};
        const double initialTime=scene.GetElapsedTime();
        renderDelta=animated ? 1.f/60.f:0.f;
        for(unsigned frame=0;frame<24;++frame)
        {
            const glm::vec3 offset=walkDirection*(.2f*float(frame+1));
            viewer->transform.position=walkStart+glm::dvec3(offset);
            camera->target=walkTarget+offset;
            render();
            const auto current=scene.GetShadowDebugSnapshot();
            unsigned oakWindMeshes=0;
            for(const auto* mesh:scene.GetComponents<Mesh>()) if(mesh->instanceLodBand>=0)
            {
                oakWindMeshes+=mesh->foliageWindStrength>0.f;
                Check(mesh->runtimeShadowDistance==(unrestricted ? unrestrictedRange:savedCasterRanges.front().second),
                    "caster range override was not retained by preparation");
                Check(mesh->runtimeShadowFadeWidth==savedCasterRanges.front().first->GetShadowFadeWidth(),
                    "caster fade override was not retained by preparation");
            }
            Check(current.cascadeCount==4,"walk test lost authored cascade count");
            const unsigned all=(1u<<current.cascadeCount)-1u;
            Check(current.refreshedCascadeMask && !(current.refreshedCascadeMask & ~all),
                "walk test received an invalid cascade refresh mask");
            if(forceShadowRefresh)
                Check(current.refreshedCascadeMask==all,"reference did not refresh every cascade");
            retainedFrames+=current.refreshedCascadeMask!=all;
            const auto position=glm::vec3(viewer->transform.position);
            walkTrace << "{\"width\":" << width << ",\"height\":" << height
                << ",\"phase\":\"" << phase << "\",\"frame\":" << frame
                << ",\"walk_view\":\"" << walkView << "\""
                << ",\"unrestricted_casters\":" << (unrestricted ? "true":"false")
                << ",\"forest_caster_range\":" << (unrestricted ? unrestrictedRange:savedCasterRanges.front().second)
                << ",\"forest_caster_fade_width\":" << savedCasterRanges.front().first->GetShadowFadeWidth()
                << ",\"diagnostic_oak_wind\":false"
                << ",\"oak_wind_meshes\":" << oakWindMeshes
                << ",\"forced_full_refresh\":" << (forceShadowRefresh ? "true":"false")
                << ",\"partial_depth_clear_supported\":" << (partialDepthClearSupported ? "true":"false")
                << ",\"time\":" << scene.GetElapsedTime() << ",\"refreshed_mask\":"
                << current.refreshedCascadeMask << ",\"camera\":["
                << position.x << ',' << position.y << ',' << position.z << "]";
            const auto vp=camera->GetProjectionMatrix(float(width)/height)*camera->GetViewMatrix();
            walkTrace << ",\"view_projection_column_major\":[";
            for(unsigned i=0;i<16;++i) { if(i) walkTrace << ','; walkTrace << vp[i/4][i%4]; }
            walkTrace << "],\"cascades\":[";
            for(unsigned cascade=0;cascade<current.cascadeCount;++cascade)
            {
                const bool refreshed=(current.refreshedCascadeMask & (1u<<cascade))!=0;
                age[cascade]=refreshed ? 0:age[cascade]+1;
                Check(age[cascade]<=6,"walking starved a retained shadow cascade");
                const float clearance=current.cascadeRadii[cascade]-
                    glm::length(position-current.cascadeCenters[cascade])-current.cascadeSplits[cascade];
                Check(clearance>=.009f,"walking retained a tile outside current receiver coverage");
                const float anchorDistance=glm::length(anchors[cascade]-position);
                crossed[cascade]=crossed[cascade] || anchorDistance<current.cascadeSplits[cascade];
                if(cascade) walkTrace << ',';
                walkTrace << "{\"split\":" << current.cascadeSplits[cascade]
                    << ",\"coverage_clearance\":" << clearance << ",\"age\":" << age[cascade]
                    << ",\"anchor\":[" << anchors[cascade].x << ',' << anchors[cascade].y
                    << ',' << anchors[cascade].z << "],\"anchor_distance\":" << anchorDistance << '}';
            }
            walkTrace << "]}\n";
            if(frame%4==0)
            {
                std::vector<uint8_t> pixels;
                Check(renderer.ReadSceneCaptureRGBA(pixels),"walking capture failed");
                Check(pixels.size()==width*height*4,"walking capture size wrong");
                capturedImages.emplace_back(walkPrefix+phase+"-"+std::to_string(frame),std::move(pixels));
            }
        }
        if(!forceShadowRefresh && partialDepthClearSupported)
            Check(retainedFrames>0,"walking never exercised retained shadow tiles");
        for(unsigned cascade=0;cascade<initial.cascadeCount;++cascade)
            Check(crossed[cascade],"walking did not cross a fixed anchor's radial band boundary");
        const double elapsed=scene.GetElapsedTime()-initialTime;
        Check(animated ? elapsed>.39 && elapsed<.41:elapsed==0.,
            "walking wind clock did not match the requested phase");
        report << "walk " << phase << ": 24 frames; retained frames=" << retainedFrames
            << "; elapsed=" << elapsed << std::endl;
    }
    renderDelta=0.f;
    forceShadowRefresh=false;
    if(groundCapture)
    {
        // A matched unshadowed control separates shadow changes from terrain
        // texture resampling and view-dependent material shading.
        std::vector<std::pair<Light*,bool>> savedCasts;
        for(auto* light:scene.GetComponents<Light>())
        { savedCasts.emplace_back(light,light->castsShadows); light->castsShadows=false; }
        for(unsigned frame=0;frame<24;frame+=4)
        {
            const glm::vec3 offset=walkDirection*(.2f*float(frame+1));
            viewer->transform.position=walkStart+glm::dvec3(offset);
            camera->target=walkTarget+offset;
            render();
            std::vector<uint8_t> pixels;
            Check(renderer.ReadSceneCaptureRGBA(pixels),"ground control capture failed");
            Check(pixels.size()==width*height*4,"ground control capture size wrong");
            capturedImages.emplace_back("walk-ground-control-"+std::to_string(frame),std::move(pixels));
        }
        for(const auto& saved:savedCasts) saved.first->castsShadows=saved.second;
        report << "Ground color isolation; complete forest shadow pass; six matched unshadowed controls.\n";

        // Authored oaks have zero wind strength, and the shader deliberately
        // excludes wind from the shadow pass. Prove visible deformation and
        // the static-caster policy independently of the clock-only walk.
        diagnosticOakWind=true;
        forceShadowRefresh=true;
        viewer->transform.position=walkStart;
        camera->target=walkTarget;
        auto windCapture=[&](const std::string& name,bool ground) {
            terrainOnlyColor=ground; oakOnlyColor=!ground;
            for(int warm=0;warm<4;++warm) render();
            Check(diagnosticWindMeshes>0,"wind diagnostic has no enabled oak meshes");
            const auto shadows=scene.GetShadowDebugSnapshot();
            Check(shadows.refreshedCascadeMask==((1u<<shadows.cascadeCount)-1u),
                "wind diagnostic did not fully refresh shadow tiles");
            std::vector<uint8_t> pixels;
            Check(renderer.ReadSceneCaptureRGBA(pixels),"wind-policy capture failed");
            capturedImages.emplace_back(name,pixels);
            return pixels;
        };
        const auto colorBefore=windCapture("wind-policy-color-before",false);
        const auto groundBefore=windCapture("wind-policy-ground-before",true);
        const double windStart=scene.GetElapsedTime();
        renderDelta=1.f/60.f;
        for(unsigned frame=0;frame<60;++frame) render();
        renderDelta=0.f;
        const auto groundAfter=windCapture("wind-policy-ground-after",true);
        const auto colorAfter=windCapture("wind-policy-color-after",false);
        Check(scene.GetElapsedTime()-windStart>.99 && scene.GetElapsedTime()-windStart<1.01,
            "wind-policy diagnostic did not advance one second");
        size_t movingPixels=0; unsigned groundMaximum=0;
        for(size_t pixel=0;pixel<colorBefore.size();pixel+=4)
        {
            unsigned colorMaximum=0;
            for(unsigned channel=0;channel<3;++channel)
            {
                colorMaximum=std::max(colorMaximum,unsigned(std::abs(int(colorAfter[pixel+channel])-int(colorBefore[pixel+channel]))));
                groundMaximum=std::max(groundMaximum,unsigned(std::abs(int(groundAfter[pixel+channel])-int(groundBefore[pixel+channel]))));
            }
            movingPixels+=colorMaximum>8;
        }
        report << "Wind policy: enabled oak meshes=" << diagnosticWindMeshes
            << "; moving color pixels (>8 RGB)=" << movingPixels
            << "; fixed-camera ground maximum RGB change=" << groundMaximum << std::endl;
        Check(movingPixels>100,"diagnostic wind did not visibly deform the oak color pass");
        Check(groundMaximum<=2,"shader wind changed static-caster ground shadows");
    }
    renderer.WaitIdle();
    checkValidation();
    std::ofstream trace(outputRoot+"/"+backend+"-"+walkPrefix.substr(0,walkPrefix.size()-1)+".jsonl");
    trace << walkTrace.str();
    if(groundCapture)
    {
        std::ofstream geometry(outputRoot+"/"+backend+"-ground-geometry.json");
        geometry << std::setprecision(9) << '[';
        bool firstMesh=true;
        for(auto* ground:scene.GetComponents<Terrain>())
        {
            const auto* mesh=ground->Owner->GetComponent<Mesh>();
            if(!mesh || !ground->Owner->IsEnabledInHierarchy() || mesh->GetIndices().empty()) continue;
            if(!firstMesh) geometry << ',';
            firstMesh=false;
            geometry << "{\"vertices\":[";
            const auto world=ground->Owner->transform.GetWorldMatrix();
            bool firstVertex=true;
            const auto writeVertex=[&](const auto& vertex) {
                const auto p=glm::vec3(world*glm::vec4(vertex.pos[0],vertex.pos[1],vertex.pos[2],1));
                if(!firstVertex) geometry << ',';
                firstVertex=false;
                geometry << '[' << p.x << ',' << p.y << ',' << p.z << ']';
            };
            if(!mesh->GetTerrainVertices().empty())
                for(const auto& vertex:mesh->GetTerrainVertices()) writeVertex(vertex);
            else for(const auto& vertex:mesh->GetVertices()) writeVertex(vertex);
            geometry << "],\"indices\":[";
            for(size_t i=0;i<mesh->GetIndices().size();++i)
            { if(i) geometry << ','; geometry << mesh->GetIndices()[i]; }
            geometry << "]}";
        }
        geometry << "]\n";
    }
    for (const auto& [name,pixels]:capturedImages)
    {
        std::ofstream image(outputRoot+"/"+backend+"-"+name+".ppm",std::ios::binary);
        image << "P6\n" << width << ' ' << height << "\n255\n";
        for (size_t i=0;i<pixels.size();i+=4) image.write(reinterpret_cast<const char*>(pixels.data()+i),3);
    }
    std::cout << backend << ": actual oak captures, shadow comparisons, staggered refresh and camera cuts passed.\n";
}

template<class Renderer> void BenchmarkOakClusters(const std::string& backend)
{
    const char* outputEnv=std::getenv("ENGINE_OAK_CLUSTER_OUTPUT");
    const std::filesystem::path output=outputEnv ? outputEnv : ".local/oak-cluster-benchmark";
    float lod1Distance=20.f, lod2Distance=60.f;
    if(const char* distances=std::getenv("ENGINE_OAK_LOD_DISTANCES"))
    {
        std::istringstream input(distances);
        char comma=0;
        Check(bool(input>>lod1Distance>>comma>>lod2Distance) && comma==',' && input.eof() &&
            std::isfinite(lod1Distance) && std::isfinite(lod2Distance) &&
            lod1Distance>0.f && lod2Distance>lod1Distance,
            "oak LOD distances must be two increasing positive values, such as 20,60");
    }
    const int displayLod=std::getenv("ENGINE_OAK_DISPLAY_LOD")
        ? std::atoi(std::getenv("ENGINE_OAK_DISPLAY_LOD")) : -1;
    Check(displayLod>=-1 && displayLod<=2,"oak display LOD must be -1, 0, 1 or 2");
    std::filesystem::create_directories(output);
    constexpr unsigned width=960,height=540;
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Oak cluster benchmark",width,height);
    ShowWindow(window.GetHWND(),SW_HIDE);
    Renderer renderer;
    Check(renderer.Init(window.GetHWND(),width,height),"oak cluster renderer init failed");
    Engine::Scene::Scene scene; scene.Init(renderer.GetGraphicsProvider());
    scene.SetBeforeGpuResourceDestruction([&] { renderer.WaitIdle(); });
    Check(scene.Load("Engine/Core/Assets/Scenes/terrain/oak_meadow.scene"),"oak scene load failed");
    auto* viewer=scene.FindObjectByName("Meadow Explorer");
    Check(viewer!=nullptr,"oak benchmark viewer missing");
    auto* camera=viewer->GetComponent<Camera>();
    Check(camera!=nullptr,"oak benchmark camera missing");
    const glm::vec3 start(viewer->transform.GetWorldPosition());
    const glm::vec3 target(camera->target);
    std::vector<TerrainObjectLayer*> forests;
    for(auto* layer:scene.GetComponents<TerrainObjectLayer>())
        if(layer->layerName=="Oak forest")
        {
            forests.push_back(layer);
            for(auto& [asset,lods]:layer->objectLods)
                if(lods.meshes.size()>=3)
                    lods.distances={lod1Distance,lod2Distance};
            layer->MarkConfigurationDirty();
        }
    Check(!forests.empty(),"oak benchmark forest layer missing");
    for(auto* field:scene.GetComponents<GrassField>()) { field->useEditorCamera=false; field->Start(); }

    std::ofstream csv(output/(backend+".csv"));
    csv << "span_m,phase,frame,cpu_cull_ms,cpu_tile_prepare_ms,cpu_submit_ms,gpu_batches,draws,submitted_instances,submitted_triangles,visible_placements,gpu_timing_valid,gpu_workload_valid,tree_bark_ms,tree_leaves_ms,tree_bark_vs,tree_bark_ps,tree_leaves_vs,tree_leaves_ps\n";
    const float spans[]={256.f,128.f,64.f,32.f};
    std::vector<float> selectedSpans;
    if(const char* span=std::getenv("ENGINE_OAK_CLUSTER_SPAN"))
    {
        const float value=std::strtof(span,nullptr);
        Check(std::isfinite(value) && value>=8.f && value<=256.f,
            "oak benchmark span must be in [8,256]");
        selectedSpans.push_back(value);
    }
    else for(float span:spans) selectedSpans.push_back(span);
    const int sweepFrames=24;
    const bool saveSweepCapture=std::getenv("ENGINE_OAK_CLUSTER_CAPTURE")!=nullptr;
    for(float span:selectedSpans)
    {
        TerrainObjectLayer::SetPrefabInstanceGroupSpanForBenchmark(span);
        for(auto* layer:forests) layer->RebuildPrefabInstancesForBenchmark();
        auto render=[&](const char* phase,int frame,float cameraOffset) {
            renderer.WaitIdle();
            scene.AdvanceManualFrame(0.f);
            for(auto* field:scene.GetComponents<GrassField>()) field->RefreshEditorPreview();
            viewer->transform.position=start+glm::vec3(cameraOffset,0,0);
            camera->target=target+glm::vec3(cameraOffset,0,0);
            renderer.BeginFrame();
            auto context=renderer.CreateFrameGraphicsContext();
            context->SetViewport({0,0,float(width),float(height),0,1});
            context->SetScissorRect({0,0,int(width),int(height)});
            renderer.Clear(.1f,.1f,.1f,1);
            scene.PrepareRenderFrame();
            scene.Render(context.get(),float(width)/height,camera,displayLod>=0,width,height,
                false,nullptr,displayLod);
            const bool captureThisFrame=saveSweepCapture && std::strcmp(phase,"camera_sweep")==0 &&
                frame==sweepFrames-1;
            if(captureThisFrame) renderer.RequestSceneCapture();
            renderer.EndFrame();
            renderer.WaitIdle();
            if(captureThisFrame)
            {
                std::vector<uint8_t> pixels;
                Check(renderer.ReadSceneCaptureRGBA(pixels) && pixels.size()==size_t(width)*height*4,
                    "oak LOD sweep screenshot capture failed");
                std::ofstream image(output/(backend+"-sweep.ppm"),std::ios::binary);
                image << "P6\n" << width << ' ' << height << "\n255\n";
                for(size_t i=0;i<pixels.size();i+=4)
                    image.write(reinterpret_cast<const char*>(pixels.data()+i),3);
            }
            const auto cpu=scene.GetLastRenderCpuTimings();
            const auto gpu=renderer.GetFrameTimingTelemetry();
            uint64_t visible=0;
            const glm::mat4 vp=camera->GetProjectionMatrix(float(width)/height)*camera->GetViewMatrix();
            const glm::vec3 eye(viewer->transform.GetWorldPosition());
            const glm::vec3 forward=glm::normalize(glm::vec3(camera->target)-eye);
            const float tanHalf=std::tan(glm::radians(camera->fov)*.5f);
            for(auto* layer:forests) for(const auto& placement:layer->placements)
            {
                const glm::vec3 p=glm::vec3(layer->Owner->transform.GetWorldMatrix()*glm::vec4(placement.position,1));
                const glm::vec4 clip=vp*glm::vec4(p,1);
                const float depth=glm::dot(p-eye,forward);
                const float radius=std::max(1.f,std::abs(placement.scale)*8.f);
                if(depth+radius<camera->nearPlane || depth-radius>camera->farPlane) continue;
                if(clip.w<=0.f || std::abs(clip.x)>clip.w+radius || std::abs(clip.y)>clip.w+radius) continue;
                ++visible;
            }
            const auto bark=static_cast<size_t>(Engine::Graphics::GpuTimingStage::TreeBark);
            const auto leaves=static_cast<size_t>(Engine::Graphics::GpuTimingStage::TreeLeaves);
            csv << span << ',' << phase << ',' << frame << ',' << cpu.cullingMs << ','
                << cpu.instanceTilePrepareMs << ',' << cpu.submissionMs << ',' << cpu.gpuBatches << ','
                << cpu.physicalDraws << ',' << cpu.submittedInstances << ',' << cpu.submittedTriangles << ','
                << visible << ',' << gpu.gpuTimingsValid << ',' << gpu.gpuWorkloadValid << ','
                << gpu.gpuMilliseconds[bark] << ',' << gpu.gpuMilliseconds[leaves] << ','
                << gpu.gpuWorkload[bark].vertices << ',' << gpu.gpuWorkload[bark].fragments << ','
                << gpu.gpuWorkload[leaves].vertices << ',' << gpu.gpuWorkload[leaves].fragments << '\n';
        };
        for(int i=0;i<8;++i) render("warmup",i,0.f);
        for(int i=0;i<32;++i) render("steady",i,0.f);
        for(int i=0;i<sweepFrames;++i)
            render("camera_sweep",i,(float(i)-float(sweepFrames-1)*.5f)*.75f);
        std::cout << backend << " span " << span << " m, LOD distances "
            << lod1Distance << '/' << lod2Distance << " m, display LOD "
            << displayLod << " captured\n";
    }
}

int main(int argc,char** argv)
{
    try
    {
        Check(argc==2,"expected renderer backend");
        const std::string backend=argv[1];
        if (backend=="cluster-benchmark-dx11") { BenchmarkOakClusters<DX11GameRenderer>(backend); return 0; }
        else if (backend=="cluster-benchmark-dx12") { BenchmarkOakClusters<DX12GameRenderer>(backend); return 0; }
        if(backend=="dx12" && std::getenv("ENGINE_OAK_DX12_VALIDATION"))
        {
            Microsoft::WRL::ComPtr<ID3D12Debug> debug;
            if(FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            { std::cerr << "Skipping DX12 validation: debug layer unavailable\n"; return 77; }
            debug->EnableDebugLayer();
            if(std::string(std::getenv("ENGINE_OAK_DX12_VALIDATION"))=="gpu")
            {
                Microsoft::WRL::ComPtr<ID3D12Debug1> gpuDebug;
                if(FAILED(debug.As(&gpuDebug)))
                { std::cerr << "Skipping DX12 validation: GPU validation unavailable\n"; return 77; }
                gpuDebug->SetEnableGPUBasedValidation(TRUE);
            }
        }
        if (backend=="grass-phase-timing")
            for(int i=0;i<1000;++i) CheckGrassPhaseTiming();
        else if (backend=="resolved-material-copy") CheckResolvedMaterialCopy();
        else if (backend=="shared-prefabs") CheckSharedPrefabLayer();
        else if (backend=="dx11") CaptureOak<DX11GameRenderer>(backend);
        else if (backend=="dx12") CaptureOak<DX12GameRenderer>(backend);
#if defined(ENGINE_VULKAN_ENABLED)
        else if (backend=="vulkan") CaptureOak<VulkanGameRenderer>(backend);
#endif
        else throw std::runtime_error("backend unavailable");
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
