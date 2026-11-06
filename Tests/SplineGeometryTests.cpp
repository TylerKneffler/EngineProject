#include "Core/Compoonents/Path/SplineGeometry.h"
#include "Core/Compoonents/Path/SplinePath.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>
using Engine::Components::Mesh;
static void Require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
static glm::vec3 Position(const Mesh::Vertex& v) { return {v.pos[0],v.pos[1],v.pos[2]}; }
static glm::vec3 Normal(const Mesh::Vertex& v) { return {v.normal[0],v.normal[1],v.normal[2]}; }
static void Validate(const std::vector<Mesh::Vertex>& data)
{
    Require(!data.empty() && data.size()%3==0,"invalid topology");
    for (const auto& v:data) {
        Require(std::isfinite(v.pos[0]) && std::isfinite(v.pos[1]) && std::isfinite(v.pos[2]),"nonfinite position");
        Require(std::abs(glm::length(Normal(v))-1.f)<.001f,"invalid normal");
    }
    for (size_t i=0;i<data.size();i+=3) {
        const auto face=glm::cross(Position(data[i+1])-Position(data[i]),Position(data[i+2])-Position(data[i]));
        Require(glm::length(face)>1.e-7f,"degenerate triangle");
        Require(glm::dot(face,Normal(data[i])+Normal(data[i+1])+Normal(data[i+2]))>0,"winding disagrees with normals");
    }
}
int main()
{
    std::filesystem::path sourcePath;
    try {
        Engine::Scene::Scene scene;
        auto* root=scene.AddObject("Path");
        auto* path=root->AddComponent<SplinePath>();
        path->interpolation=int(SplinePath::Interpolation::Linear);
        for (float z:{0.f,10.f}) {
            auto* point=scene.AddObject("Point"); scene.SetParentLocal(point,root); point->transform.position={0,0,z};
        }
        auto* geometry=root->AddComponent<SplineGeometry>();
        std::vector<Mesh::Vertex> data; std::string error;
        Require(geometry->BuildGeometry(data,error),error.c_str()); Validate(data);
        Require(data.size()==120,"road subdivision count wrong");
        Require(std::abs(Position(data.front()).x+2.f)<.001f,"road width wrong");
        Require(std::abs(data.back().uv[1]-5.f)<.001f,"distance UVs wrong");
        geometry->mode=int(SplineGeometry::Mode::Pipe); geometry->radialSegments=8;
        Require(geometry->BuildGeometry(data,error),error.c_str()); Validate(data);
        Require(data.size()==20*8*6+8*6,"pipe cap count wrong");
        geometry->PrepareGeometry(nullptr);
        Require(path->GetControlPointCount()==2,"generated output became a control point");
        const auto children=root->Children.size();
        geometry->PrepareGeometry(nullptr);
        Require(root->Children.size()==children,"unchanged preparation duplicated output");
        Engine::Serialization::SceneSerializer::EnsureBuiltinsRegistered();
        const auto sceneText=Engine::Serialization::SceneSerializer::SaveToString(scene);
        Require(sceneText.find("Spline Geometry (generated)")==std::string::npos,"derived geometry was serialized");
        Engine::Scene::Scene loaded;
        Require(Engine::Serialization::SceneSerializer::LoadFromString(loaded,sceneText),"scene did not reload");
        SplineGeometry* loadedGeometry=nullptr;
        for (const auto& object:loaded.GetObjects())
            if (auto* candidate=object->GetComponent<SplineGeometry>()) loadedGeometry=candidate;
        Require(loadedGeometry && loadedGeometry->BuildGeometry(data,error),"registered geometry did not rebuild after load");
        const auto saved=geometry->Serialize();
        SplineGeometry restored; restored.Deserialize(saved);
        Require(restored.mode==geometry->mode && restored.radialSegments==8,"settings did not round trip");
        // Nonuniform owner transforms must preserve world-space dimensions.
        root->transform.scale={2,3,4};
        geometry->mode=int(SplineGeometry::Mode::Road);
        Require(geometry->BuildGeometry(data,error),error.c_str()); Validate(data);
        const auto world=root->transform.GetWorldMatrix();
        const auto left=glm::vec3(world*glm::vec4(Position(data[0]),1));
        const auto right=glm::vec3(world*glm::vec4(Position(data[2]),1));
        Require(std::abs(glm::length(right-left)-4.f)<.001f,"owner scale changed road width");
        root->transform.scale={1,1,1};
        // A subdivided triangular strip is enough to verify repeat and bend mapping.
        std::vector<Mesh::Vertex> source(6);
        const glm::vec3 points[]={{-.5f,0,0},{-.5f,0,2},{.5f,0,0},{.5f,0,0},{-.5f,0,2},{.5f,0,2}};
        for (int i=0;i<6;++i) for (int a=0;a<3;++a) {
            source[i].pos[a]=points[i][a]; source[i].normal[a]=a==1 ? 1.f:0.f;
            source[i].tangent[a]=a==0 ? 1.f:0.f;
        }
        sourcePath=std::filesystem::temp_directory_path()/"engine-spline-geometry-test.mesh";
        Require(Mesh::SaveNativeFile(sourcePath.string(),source),"could not save fixture mesh");
        geometry->meshAsset=sourcePath.string(); geometry->spacing=2.f;
        geometry->mode=int(SplineGeometry::Mode::RepeatMesh);
        Require(geometry->BuildGeometry(data,error),error.c_str()); Validate(data);
        Require(data.size()==30,"repeat count wrong");
        Require(std::abs(Position(data[6]).z-2.f)<.001f,"repeat spacing wrong");
        geometry->mode=int(SplineGeometry::Mode::BendMesh);
        geometry->spacing=3.f;
        Require(geometry->BuildGeometry(data,error),error.c_str()); Validate(data);
        Require(data.size()==18,"fit section count wrong");
        Require(glm::length(Position(data[1])-Position(data[6]))<.001f,"bent sections do not join");
        Require(std::abs(Position(data.back()).z-10.f)<.001f,"bent mesh did not reach path end");
        geometry->mode=int(SplineGeometry::Mode::Pipe);
        path->closed=true; path->interpolation=int(SplinePath::Interpolation::CatmullRom);
        for (auto p:{glm::vec3(10,4,10),glm::vec3(10,0,0)}) {
            auto* point=scene.AddObject("Point"); scene.SetParentLocal(point,root); point->transform.position=p;
        }
        Require(geometry->BuildGeometry(data,error),error.c_str()); Validate(data);
        const size_t last=data.size()-size_t(geometry->radialSegments)*6;
        Require(glm::length(Position(data[0])-Position(data[last+2]))<.001f,"closed pipe seam not joined");
        root->Children[1]->transform.position={0,10,0};
        Require(geometry->BuildGeometry(data,error),error.c_str()); Validate(data);
        // Rebuild once with a changed point and verify the live mesh revision.
        geometry->PrepareGeometry(nullptr);
        auto* output=root->Children.back();
        auto* outputMesh=output->GetComponent<Mesh>();
        // The generated child was inserted before the two newly authored points.
        if (!outputMesh) for (auto* child:root->Children)
            if (child->runtimeOnly && child->GetComponent<Mesh>()) { output=child; outputMesh=child->GetComponent<Mesh>(); break; }
        Require(outputMesh,"generated mesh missing");
        const auto revision=outputMesh->GetAuthoredGeometryRevision();
        root->Children[0]->transform.position={-1,0,0};
        geometry->PrepareGeometry(nullptr);
        Require(outputMesh->GetAuthoredGeometryRevision()>revision,"moving a point did not rebuild geometry");
        geometry->spacing=0;
        Require(!geometry->BuildGeometry(data,error) && data.empty(),"invalid spacing accepted");
        geometry->spacing=2; geometry->segmentLength=1.e-9f;
        Require(!geometry->BuildGeometry(data,error) && !error.empty(),"sample budget was not enforced");
        geometry->segmentLength=.5f; geometry->pathObjectName="Missing";
        Require(!geometry->BuildGeometry(data,error),"missing path accepted");
        geometry->PrepareGeometry(nullptr);
        Require(!output->enabled,"invalid build left stale geometry visible");
        // The bundled construction example must deserialize and build every mode.
        Engine::Scene::Scene demo;
        Require(Engine::Serialization::SceneSerializer::Load(demo,
            "Engine/Core/Assets/Scenes/Splines/spline_geometry_demo.scene",nullptr),"demo scene did not load");
        size_t generators=0;
        for (const auto& object:demo.GetObjects()) if (auto* g=object->GetComponent<SplineGeometry>()) {
            Require(g->BuildGeometry(data,error),error.c_str()); Validate(data); ++generators;
        }
        Require(generators==4,"demo is missing a geometry mode");
        std::filesystem::remove(sourcePath);
        std::cout<<"Spline geometry tests passed\n"; return 0;
    } catch (const std::exception& e) {
        if (!sourcePath.empty()) std::filesystem::remove(sourcePath);
        std::cerr<<e.what()<<'\n'; return 1;
    }
}
