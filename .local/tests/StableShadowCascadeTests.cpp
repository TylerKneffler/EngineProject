#include "Core/Rendering/StableShadowCascade.h"
#include <iostream>
#include <stdexcept>

void Check(bool ok,const char* message)
{ if(!ok) throw std::runtime_error(message); }

int main()
{
    try
    {
        for(const auto light: {glm::vec3(.3f,1,.7f),glm::vec3(0,1,0),glm::vec3(1,.01f,0)})
        for(const auto camera: {glm::vec3(0),glm::vec3(-107.3f,26.7f,88.1f)})
        for(const float band: {2.f,10.f,35.f,90.f})
        {
            const auto cascade=Engine::Rendering::BuildStableShadowCascade(camera,light,band,90,1024);
            // All directions of the receiver sphere must stay inside the map,
            // including behind the viewer and near a texel-grid boundary.
            for(int latitude=-8;latitude<=8;++latitude)
            for(int longitude=0;longitude<64;++longitude)
            {
                const float y=float(latitude)/8.f;
                const float horizontal=std::sqrt(std::max(0.f,1.f-y*y));
                const float angle=6.2831853f*longitude/64;
                const auto point=camera+band*glm::vec3(horizontal*std::cos(angle),y,horizontal*std::sin(angle));
                const auto clip=cascade.viewProjection*glm::vec4(point,1);
                Check(std::abs(clip.x)<1 && std::abs(clip.y)<1 && clip.z>0 && clip.z<1,
                    "radial receiver escaped directional shadow coverage");
            }
            const auto moved=Engine::Rendering::BuildStableShadowCascade(
                camera+glm::vec3(.29f,.03f,-.17f),light,band,90,1024);
            const auto first=cascade.viewProjection*glm::vec4(camera,1);
            const auto second=moved.viewProjection*glm::vec4(camera,1);
            for(unsigned axis=0;axis<2;++axis)
            {
                const float shift=(second[axis]-first[axis])*512.f;
                Check(std::abs(shift-std::round(shift))<.003f,
                    "translation changed the world-space shadow texel phase");
            }
            // A caster above a receiver toward the sun shares its texel and
            // must be nearer in depth, including lights almost on the horizon.
            const auto upstream=cascade.viewProjection*glm::vec4(camera+glm::normalize(light)*40.f,1);
            Check(glm::length(glm::vec2(upstream)-glm::vec2(first))<.0001f &&
                upstream.z>0 && upstream.z<first.z,"off-screen upstream caster was clipped or misplaced");
        }
        std::cout << "Radial coverage, upstream casters and world texel phase passed.\n";
        return 0;
    }
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
