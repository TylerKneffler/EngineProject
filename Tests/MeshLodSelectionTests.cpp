#include "Core/Rendering/MeshLodSelection.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cstdlib>
#include <iostream>

using namespace Engine::Rendering;
static void Check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main()
{
    const glm::vec3 lo(-2,-5,-2), hi(2,5,2);
    const glm::mat4 world(1.f);
    const auto makeView = [](glm::vec3 eye, float fov, float scale) {
        return MeshLodView{glm::perspective(glm::radians(fov), 16.f/9.f, .1f, 1000.f) *
            glm::lookAt(eye, glm::vec3(0), glm::vec3(0,1,0)), {1920*scale,1080*scale}};
    };
    const float ground = ProjectedLodPixels(lo, hi, world, makeView({0,0,40},60,1));
    const float elevated = ProjectedLodPixels(lo, hi, world, makeView({0,30,40},60,1));
    Check(ground > elevated && elevated > 0, "elevated projection");
    Check(ProjectedLodPixels(lo,hi,world,makeView({0,0,40},30,1)) > ground, "zoom retains detail");
    for (float scale : {.5f,.75f,1.f,1.5f})
        Check(std::abs(ProjectedLodPixels(lo,hi,world,makeView({0,0,40},60,scale))-ground*scale)<.01f,
            "render scale uses actual pixels");
    Check(ProjectedLodPixels(lo-glm::vec3(1),hi+glm::vec3(1),world,makeView({0,0,40},60,1))>ground,
        "wind envelope retains detail");
    Check(std::isinf(ProjectedLodPixels(lo,hi,world,makeView({0,0,1},60,1))), "eye crossing");
    MeshLodView ortho{glm::ortho(-20.f,20.f,-10.f,10.f,.1f,1000.f) *
        glm::lookAt(glm::vec3(0,0,40),glm::vec3(0),glm::vec3(0,1,0)), {1920,1080}};
    Check(std::abs(ProjectedLodPixels(lo,hi,world,ortho)-540)<.01f, "orthographic bounds");
    MeshLodView zeroDepth{glm::orthoRH_ZO(-20.f,20.f,-10.f,10.f,.1f,1000.f) *
        glm::lookAt(glm::vec3(0,0,40),glm::vec3(0),glm::vec3(0,1,0)), {1920,1080}, true};
    Check(std::abs(ProjectedLodPixels(lo,hi,world,zeroDepth)-540)<.01f, "zero-to-one depth");
    zeroDepth.viewportPixels = {0,0};
    Check(std::isinf(ProjectedLodPixels(lo,hi,world,zeroDepth)), "missing viewport keeps finest");
    const std::vector<float> thresholds{300,80};
    int state = 0, changes = 0;
    for (int i=0;i<100;++i)
    {
        int next = SelectScreenMeshLod(i%2 ? 290.f : 310.f, thresholds, state);
        changes += next != state; state=next;
    }
    Check(changes==0, "camera jitter must not chatter");
    state=SelectScreenMeshLod(269,thresholds,state);
    Check(state==1 && SelectScreenMeshLod(320,thresholds,state)==1 &&
        SelectScreenMeshLod(331,thresholds,state)==0, "bidirectional hysteresis");
    Check(SelectScreenMeshLod(20,thresholds,0)==2, "camera cut skips bands");
    Check(SelectScreenMeshLod(10,{80,300})==0, "invalid authoring");
    Check(SelectScreenMeshLod(std::numeric_limits<float>::infinity(),thresholds,2)==0, "near plane keeps finest");
    std::cout << "Projected LOD comparisons passed: ground/elevated, zoom, motion, render scales, wind, ortho.\n";
}
