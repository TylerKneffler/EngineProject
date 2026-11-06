#include "Core/Compoonents/Path/SplineGeometry.h"
#include "Core/Compoonents/Path/SplinePath.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

namespace
{
using Mesh = Engine::Components::Mesh;
constexpr size_t MaximumVertices = 1500000;
constexpr size_t MaximumSamples = 8192;
struct Frame { glm::vec3 position, forward, right, up; };
bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
glm::vec3 Unit(glm::vec3 v, glm::vec3 fallback)
{
    const float length = glm::length(v);
    return length > 1.e-6f ? v / length : fallback;
}
glm::vec3 Right(glm::vec3 forward, glm::vec3 up)
{
    if (std::abs(glm::dot(forward,up)) > .999f)
        up = std::abs(forward.y) < .9f ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
    return Unit(glm::cross(up,forward),{1,0,0});
}
SplinePath* Resolve(const SplineGeometry& geometry)
{
    auto* object = geometry.Owner;
    if (object && !geometry.pathObjectName.empty())
        object = object->FindObjectInSceneByName(geometry.pathObjectName);
    return object ? object->GetComponent<SplinePath>() : nullptr;
}
void Hash(uint64_t& hash, const void* bytes, size_t count)
{
    const auto* data=static_cast<const unsigned char*>(bytes);
    for (size_t i=0;i<count;++i) { hash^=data[i]; hash*=1099511628211ull; }
}
}

SplineGeometry::SplineGeometry()
{
    SetTypeName(COMPONENT_TYPE_NAME(SplineGeometry));
    singlecomponent=true;
    RegisterField("pathObjectName",pathObjectName); RegisterField("mode",mode);
    RegisterField("meshAsset",meshAsset); RegisterField("forwardAxis",forwardAxis);
    RegisterField("spacing",spacing); RegisterField("fitToLength",fitToLength);
    RegisterField("segmentLength",segmentLength); RegisterField("width",width);
    RegisterField("radius",radius); RegisterField("radialSegments",radialSegments);
    RegisterField("capEnds",capEnds); RegisterField("uvMetersPerRepeat",uvMetersPerRepeat);
    RegisterField("meshScale",meshScale); RegisterField("offset",offset); RegisterField("up",up);
}

bool SplineGeometry::BuildGeometry(std::vector<Mesh::Vertex>& vertices, std::string& error) const
{
    vertices.clear(); error.clear();
    const auto fail=[&](const char* message) { vertices.clear(); error=message; return false; };
    auto* path=Resolve(*this);
    if (!Owner || !path || path->GetControlPointCount()<2) return fail("Assign a SplinePath with at least two control points.");
    if (mode<0 || mode>3 || forwardAxis<0 || forwardAxis>2 ||
        !std::isfinite(spacing) || spacing<=0 || !std::isfinite(segmentLength) || segmentLength<=0 ||
        !std::isfinite(width) || width<=0 || !std::isfinite(radius) || radius<=0 ||
        !std::isfinite(uvMetersPerRepeat) || uvMetersPerRepeat<=0 || !Finite(up) ||
        !Finite(offset) || !Finite(meshScale) || glm::min(meshScale.x,glm::min(meshScale.y,meshScale.z))<=0)
        return fail("Dimensions, spacing, UV repeat and mesh scales must be finite and positive.");
    const float length=path->GetLength();
    if (!std::isfinite(length) || length<1.e-5f) return fail("The path has zero or invalid length.");
    const auto world=Owner->transform.GetWorldMatrix();
    if (std::abs(glm::determinant(world))<1.e-8f) return fail("The generator transform must be invertible.");
    const auto inverse=glm::inverse(world);
    const auto normalToLocal=glm::transpose(glm::mat3(world));
    const double requested=std::ceil(double(length)/segmentLength);
    if (requested>MaximumSamples) return fail("Too many curve samples; increase Segment Length.");
    const size_t segments=std::max<size_t>(1,size_t(requested));
    std::vector<Frame> frames(segments+1);
    for (size_t i=0;i<=segments;++i)
    {
        const float t=float(i)/float(segments);
        auto& f=frames[i]; f.position=path->EvaluatePosition(t); f.forward=path->EvaluateTangent(t);
        if (!Finite(f.position) || !Finite(f.forward)) return fail("Path contains invalid control points.");
        f.forward=Unit(f.forward,{0,0,1});
        if (!i) f.right=Right(f.forward,Unit(up,{0,1,0}));
        else {
            const auto& previous=frames[i-1];
            const auto axis=glm::cross(previous.forward,f.forward);
            const float sine=glm::length(axis), cosine=glm::clamp(glm::dot(previous.forward,f.forward),-1.f,1.f);
            f.right=sine>1.e-6f ? glm::angleAxis(std::atan2(sine,cosine),axis/sine)*previous.right : previous.right;
            f.right=Unit(f.right-f.forward*glm::dot(f.right,f.forward),Right(f.forward,previous.up));
        }
        f.up=Unit(glm::cross(f.forward,f.right),{0,1,0});
    }
    // Spread residual parallel-transport twist around a closed loop.
    if (path->closed)
    {
        const float angle=std::atan2(glm::dot(glm::cross(frames.back().right,frames.front().right),frames.front().forward),
            glm::dot(frames.back().right,frames.front().right));
        for (size_t i=1;i<=segments;++i) {
            auto& f=frames[i]; f.right=glm::angleAxis(angle*float(i)/float(segments),f.forward)*f.right;
            f.up=Unit(glm::cross(f.forward,f.right),{0,1,0});
        }
        frames.back()=frames.front();
    }
    const auto sample=[&](float distance) {
        const float t=glm::clamp(distance/length,0.f,1.f)*float(segments);
        const size_t a=std::min(size_t(t),segments-1); const float blend=t-float(a);
        Frame f; f.position=glm::mix(frames[a].position,frames[a+1].position,blend);
        f.forward=Unit(glm::mix(frames[a].forward,frames[a+1].forward,blend),frames[a].forward);
        f.right=Unit(glm::mix(frames[a].right,frames[a+1].right,blend),frames[a].right);
        f.right=Unit(f.right-f.forward*glm::dot(f.right,f.forward),frames[a].right);
        f.up=Unit(glm::cross(f.forward,f.right),frames[a].up); return f;
    };
    const auto position=[&](const Frame& f, glm::vec3 local) {
        local+=offset; return f.position+f.right*local.x+f.up*local.y+f.forward*local.z;
    };
    const auto vertex=[&](glm::vec3 p, glm::vec3 n, glm::vec3 tangent, glm::vec2 uv) {
        Mesh::Vertex v{};
        p=glm::vec3(inverse*glm::vec4(p,1)); n=Unit(normalToLocal*n,{0,1,0});
        tangent=Unit(glm::mat3(inverse)*tangent,{1,0,0});
        for (int a=0;a<3;++a) { v.pos[a]=p[a]; v.normal[a]=n[a]; v.tangent[a]=tangent[a]; }
        v.tangent[3]=1; v.uv[0]=uv.x; v.uv[1]=uv.y; return v;
    };
    const auto triangle=[&](const Mesh::Vertex& a,const Mesh::Vertex& b,const Mesh::Vertex& c) {
        vertices.push_back(a); vertices.push_back(b); vertices.push_back(c);
    };
    if (mode==int(Mode::Road) || mode==int(Mode::Pipe))
    {
        const bool pipe=mode==int(Mode::Pipe);
        const int sides=pipe ? std::clamp(radialSegments,3,128) : 1;
        if (segments*size_t(sides)*6+size_t(sides)*6>MaximumVertices) return fail("Geometry exceeds the vertex budget.");
        vertices.reserve(segments*size_t(sides)*6+size_t(sides)*6);
        const auto ring=[&](size_t i,int side) {
            const auto& f=frames[i];
            const float u=float(side)/float(sides);
            const float angle=u*glm::two_pi<float>();
            const glm::vec3 crossSection=pipe ? glm::vec3(std::cos(angle)*radius,std::sin(angle)*radius,0) : glm::vec3((u-.5f)*width,0,0);
            const glm::vec3 normal=pipe ? f.right*std::cos(angle)+f.up*std::sin(angle) : f.up;
            const auto tangent=pipe ? -f.right*std::sin(angle)+f.up*std::cos(angle) : f.right;
            auto v=vertex(position(f,crossSection),normal,tangent,{u,float(i)/float(segments)*length/uvMetersPerRepeat});
            v.tangent[3]=pipe ? 1.f:-1.f;
            return v;
        };
        for (size_t i=0;i<segments;++i) for (int side=0;side<sides;++side) {
            const auto a=ring(i,side),b=ring(i,side+1),c=ring(i+1,side),d=ring(i+1,side+1);
            // Road: right x forward points down. Pipe: angular x forward points out.
            if (pipe) { triangle(a,b,c); triangle(b,d,c); }
            else { triangle(a,c,b); triangle(b,c,d); }
        }
        if (pipe && capEnds && !path->closed) for (int end=0;end<2;++end) {
            const size_t i=end ? segments : 0; const auto& f=frames[i];
            const auto center=vertex(position(f,{0,0,0}),f.forward*(end ? 1.f:-1.f),f.right,{.5f,.5f});
            for (int side=0;side<sides;++side) {
                const auto capVertex=[&](int s) {
                    const float angle=float(s)/float(sides)*glm::two_pi<float>();
                    const glm::vec3 p(std::cos(angle)*radius,std::sin(angle)*radius,0);
                    return vertex(position(f,p),f.forward*(end ? 1.f:-1.f),f.right,{p.x/(radius*2)+.5f,p.y/(radius*2)+.5f});
                };
                const auto a=capVertex(side),b=capVertex(side+1);
                if (end) triangle(center,a,b); else triangle(center,b,a);
            }
        }
    }
    else
    {
        if (meshAsset.empty()) return fail("Choose a source mesh asset for Repeat Mesh or Bend Mesh.");
        Mesh source;
        try { source.LoadFromFile(meshAsset); }
        catch (const std::exception& e) { error=e.what(); return false; }
        const auto& data=source.GetVertices(); const auto& indices=source.GetIndices();
        const size_t count=indices.empty() ? data.size() : indices.size();
        if (!count || count%3) return fail("Source mesh must contain triangles.");
        if (count>MaximumVertices) return fail("Source mesh exceeds the vertex budget.");
        std::vector<Mesh::Vertex> expanded; expanded.reserve(count);
        for (size_t j=0;j<count;++j) {
            const size_t index=indices.empty() ? j : indices[j];
            if (index>=data.size()) return fail("Invalid source mesh index.");
            expanded.push_back(data[index]);
        }
        // OBJ files may omit normals and tangents. Derive a usable surface
        // basis once before copying/deforming, rather than substituting world up.
        for (size_t j=0;j<count;j+=3) {
            const auto vector=[](const float* v) { return glm::vec3(v[0],v[1],v[2]); };
            const auto e1=vector(expanded[j+1].pos)-vector(expanded[j].pos);
            const auto e2=vector(expanded[j+2].pos)-vector(expanded[j].pos);
            const auto faceNormal=Unit(glm::cross(e1,e2),{0,1,0});
            const auto uv1=glm::vec2(expanded[j+1].uv[0]-expanded[j].uv[0],expanded[j+1].uv[1]-expanded[j].uv[1]);
            const auto uv2=glm::vec2(expanded[j+2].uv[0]-expanded[j].uv[0],expanded[j+2].uv[1]-expanded[j].uv[1]);
            const float determinant=uv1.x*uv2.y-uv1.y*uv2.x;
            for (int corner=0;corner<3;++corner) {
                auto& v=expanded[j+corner];
                const auto n=Unit(vector(v.normal),faceNormal);
                auto t=vector(v.tangent);
                const bool missing=glm::length(t)<1.e-6f;
                if (missing) t=std::abs(determinant)>1.e-8f ? (e1*uv2.y-e2*uv1.y)/determinant : e1;
                t=Unit(t-n*glm::dot(t,n),Right(n,{0,1,0}));
                if (missing) {
                    const auto b=std::abs(determinant)>1.e-8f ? (e2*uv1.x-e1*uv2.x)/determinant : glm::cross(n,t);
                    v.tangent[3]=glm::dot(glm::cross(n,t),b)<0 ? -1.f:1.f;
                }
                for (int axis=0;axis<3;++axis) { v.normal[axis]=n[axis]; v.tangent[axis]=t[axis]; }
            }
        }
        const int forward=forwardAxis, right=(forward+1)%3, vertical=(forward+2)%3;
        const float low=source.GetBoundsMin()[forward], span=source.GetBoundsMax()[forward]-low;
        if (span<1.e-6f) return fail("Source mesh has no extent along its forward axis.");
        const bool bend=mode==int(Mode::BendMesh);
        const double copies=fitToLength ? std::max(1.0,std::round(double(length)/spacing)) :
            (bend ? std::floor(double(length)/spacing) : std::ceil(double(length)/spacing));
        if (copies>double(MaximumVertices/count)) return fail("Too many repeated vertices; increase Spacing or simplify the mesh.");
        const size_t repeats=size_t(copies);
        const float step=fitToLength ? length/float(std::max<size_t>(1,repeats)) : spacing;
        vertices.reserve(repeats*count);
        for (size_t copy=0;copy<repeats;++copy) for (size_t j=0;j<count;++j) {
            const auto& v=expanded[j];
            const float along=(v.pos[forward]-low)/span;
            const auto f=sample(float(copy)*step+(bend ? along*step:0.f));
            const glm::vec3 crossSection(v.pos[right]*meshScale.x,v.pos[vertical]*meshScale.y,
                bend ? 0.f:(v.pos[forward]-low)*meshScale.z);
            const float forwardScale=bend ? step/span : meshScale.z;
            const glm::vec3 n=Unit(f.right*(v.normal[right]/meshScale.x)+f.up*(v.normal[vertical]/meshScale.y)+
                f.forward*(v.normal[forward]/forwardScale),f.up);
            const auto tangent=f.right*(v.tangent[right]*meshScale.x)+f.up*(v.tangent[vertical]*meshScale.y)+
                f.forward*(v.tangent[forward]*forwardScale);
            auto out=vertex(position(f,crossSection),n,tangent,{v.uv[0],v.uv[1]});
            out.tangent[3]=v.tangent[3];
            for (int c=0;c<4;++c) out.color[c]=v.color[c];
            vertices.push_back(out);
        }
    }
    if (vertices.empty()) return fail("No complete mesh sections fit the path.");
    return true;
}

void SplineGeometry::PrepareGeometry(IGraphicsProvider* provider)
{
    if (!Owner || !Owner->GetScene()) return;
    auto* scene=Owner->GetScene();
    auto* output=scene->FindObjectById(m_outputId);
    auto* path=Resolve(*this);
    uint64_t signature=1469598103934665603ull;
    const auto settings=Engine::Serialization::JsonWrite(Serialize()); Hash(signature,settings.data(),settings.size());
    const uint64_t ownerRevision=Owner->transform.GetWorldRevision(); Hash(signature,&ownerRevision,sizeof(ownerRevision));
    if (path) {
        const auto revision=path->GetPointSignature(); Hash(signature,&revision,sizeof(revision));
    }
    const auto* material=Owner->GetComponent<Engine::Components::Material>();
    const auto materialSettings=material ? Engine::Serialization::JsonWrite(material->Serialize()) : std::string{};
    Hash(signature,materialSettings.data(),materialSettings.size());
    if (!meshAsset.empty()) {
        std::error_code error;
        const auto stamp=std::filesystem::last_write_time(Mesh::ResolveFilePath(meshAsset),error).time_since_epoch().count();
        Hash(signature,&stamp,sizeof(stamp));
    }
    if (signature==m_signature && provider==m_provider && (!m_outputId || output)) return;
    m_signature=signature; m_provider=provider;
    std::vector<Mesh::Vertex> vertices;
    if (!BuildGeometry(vertices,m_message)) { if (output) output->enabled=false; return; }
    if (!output) {
        output=scene->AddObject("Spline Geometry (generated)"); output->runtimeOnly=true;
        scene->SetParentLocal(output,Owner); m_outputId=scene->GetObjectId(output);
        output->AddComponent<Mesh>();
    }
    output->enabled=true;
    auto* mesh=output->GetComponent<Mesh>();
    if (!mesh->SetAuthoredVertices(std::move(vertices))) { output->enabled=false; m_message="Generated mesh failed validation."; return; }
    if (provider) mesh->CreateBuffer(provider->GetBufferFactory());
    auto* copy=output->GetComponent<Engine::Components::Material>();
    if (!copy) copy=output->AddComponent<Engine::Components::Material>();
    copy->Deserialize(material ? material->Serialize() : Engine::Components::Material().Serialize());
    copy->OnAfterDeserialize(provider);
    m_message=std::to_string(mesh->GetVertexCount()/3)+" triangles";
}

void SplineGeometry::OnDestroy()
{
    if (Owner && Owner->GetScene() && m_outputId)
        Owner->GetScene()->QueueRemoveObject(m_outputId);
    m_outputId=0;
}

bool SplineGeometry::DrawProperties(Engine::Editor::IEditorUi& ui)
{
    static const char* modes[]={"Road", "Pipe", "Repeat Mesh", "Bend Mesh"};
    static const char* axes[]={"X", "Y", "Z"};
    bool changed=ui.Combo("Mode",&mode,modes,4);
    char pathName[512]{};
    std::snprintf(pathName,sizeof(pathName),"%s",pathObjectName.c_str());
    if (ui.InputText("Path Object (empty = self)",pathName,sizeof(pathName))) { pathObjectName=pathName; changed=true; }
    changed=ui.DragFloat("Segment Length",&segmentLength,.05f,.01f,100.f)||changed;
    changed=ui.DragFloat3("Offset (right / up / forward)",&offset.x,.05f)||changed;
    changed=ui.DragFloat3("Initial Up",&up.x,.05f)||changed;
    if (mode==int(Mode::Road)) changed=ui.DragFloat("Width",&width,.1f,.01f,1000.f)||changed;
    if (mode==int(Mode::Pipe)) {
        changed=ui.DragFloat("Radius",&radius,.01f,.001f,1000.f)||changed;
        changed=ui.SliderInt("Radial Segments",&radialSegments,3,128)||changed;
        changed=ui.Checkbox("Cap Open Ends",&capEnds)||changed;
    }
    if (mode<=int(Mode::Pipe)) changed=ui.DragFloat("Meters per UV Repeat",&uvMetersPerRepeat,.1f,.01f,1000.f)||changed;
    else {
        changed=DrawAssetPathPicker(ui,this,"meshAsset","Source Mesh","mesh",meshAsset)||changed;
        changed=ui.Combo("Mesh Forward Axis",&forwardAxis,axes,3)||changed;
        changed=ui.DragFloat("Spacing / Section Length",&spacing,.1f,.01f,1000.f)||changed;
        changed=ui.Checkbox("Fit Whole Sections to Path",&fitToLength)||changed;
        changed=ui.DragFloat3("Mesh Scale (right / up / forward)",&meshScale.x,.05f,.001f,1000.f)||changed;
    }
    if (ui.Button("Rebuild Spline Geometry")) { m_signature=0; changed=true; }
    ui.DisabledLabel("Add SplinePath and a Material here, or assign another path object.");
    ui.DisabledLabel("Bend meshes need subdivisions along their forward axis.");
    if (!m_message.empty()) ui.ValueLabel("Geometry",m_message.c_str());
    return changed;
}
