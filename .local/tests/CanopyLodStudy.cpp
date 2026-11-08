// Diagnostic measurements of the real selector using an asset's stable bounds.
#include "Core/Rendering/MeshLodSelection.h"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <iomanip>
#include <cstdlib>

int main(int argc, char** argv)
{
    if(argc!=7) { std::cerr << "Usage: CanopyLodStudy minX minY minZ maxX maxY maxZ\n"; return 1; }
    const glm::vec3 lo(std::atof(argv[1]),std::atof(argv[2]),std::atof(argv[3]));
    const glm::vec3 hi(std::atof(argv[4]),std::atof(argv[5]),std::atof(argv[6]));
    const auto center=(lo+hi)*.5f;
    auto pixels=[&](float distance,float elevation,float fov,float scale) {
        const auto angle=glm::radians(elevation);
        const auto eye=center+glm::vec3(0,std::sin(angle)*distance,std::cos(angle)*distance);
        Engine::Rendering::MeshLodView view{glm::perspective(glm::radians(fov),16.f/9.f,.1f,1000.f)*
            glm::lookAt(eye,center,glm::vec3(0,1,0)),{1920*scale,1080*scale}};
        return Engine::Rendering::ProjectedLodPixels(lo,hi,glm::mat4(1),view);
    };
    // Calibrate to this asset at reference FOV/resolution; these are experimental.
    const std::vector<float> thresholds{pixels(28,0,60,1),pixels(90,0,60,1)};
    std::cout << "elevation,fov,scale,hysteresis,phase,frame,distance,pixels,lod,threshold0,threshold1\n";
    for(float elevation : {0.f,35.f}) for(float fov : {60.f,30.f})
        for(float scale : {.5f,1.f,2.f}) for(float hysteresis : {0.f,.1f})
        {
            int state=-1;
            auto sample=[&](const char* phase,int frame,float distance) {
                const auto projected=pixels(distance,elevation,fov,scale);
                state=Engine::Rendering::SelectScreenMeshLod(projected,thresholds,state,hysteresis);
                std::cout << elevation << ',' << fov << ',' << scale << ',' << hysteresis << ',' << phase << ','
                    << frame << ',' << distance << ',' << projected << ',' << state << ',' << thresholds[0] << ',' << thresholds[1] << '\n';
            };
            for(int i=0;i<120;++i) sample("jitter",i,28.f+(i%2 ? .05f:-.05f));
            state=-1;
            for(int i=0;i<=360;++i) sample("out",i,20.f+i*.25f);
            for(int i=0;i<=360;++i) sample("in",i,110.f-i*.25f);
        }
}
