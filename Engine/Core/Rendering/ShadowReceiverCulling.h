#pragma once
#include <array>
#include <glm/glm.hpp>
#include <limits>

namespace Engine::Rendering
{
// Project receiver frusta perpendicular to the light rays. A caster outside
// this silhouette cannot shadow that cascade, including off-screen casters.
struct ShadowReceiverCulling
{
    struct Interval { glm::vec3 axis; float minimum, maximum; };
    std::array<Interval,28> intervals{};
    size_t count=0;
    void Build(const std::array<glm::vec3,8>& corners,const glm::mat4& lightOrientation,float padding)
    {
        count=0;
        const glm::vec3 x(lightOrientation[0][0],lightOrientation[1][0],lightOrientation[2][0]);
        const glm::vec3 y(lightOrientation[0][1],lightOrientation[1][1],lightOrientation[2][1]);
        for(size_t a=0;a<8;++a) for(size_t b=a+1;b<8;++b)
        {
            const auto edge=corners[b]-corners[a];
            auto axis=x*(-glm::dot(y,edge))+y*glm::dot(x,edge);
            const float length=glm::length(axis);
            if(length<1.e-5f) continue;
            axis/=length;
            float low=std::numeric_limits<float>::max(),high=-low;
            for(const auto& corner:corners)
            { const float value=glm::dot(axis,corner); low=glm::min(low,value); high=glm::max(high,value); }
            intervals[count++]={axis,low-padding,high+padding};
        }
    }
    bool Outside(glm::vec3 minimum,glm::vec3 maximum,const glm::mat4& world) const
    {
        const auto center=glm::vec3(world*glm::vec4((minimum+maximum)*.5f,1));
        const auto extent=(maximum-minimum)*.5f;
        for(size_t i=0;i<count;++i)
        {
            const auto& plane=intervals[i];
            const auto localAxis=glm::vec3(glm::dot(plane.axis,glm::vec3(world[0])),
                glm::dot(plane.axis,glm::vec3(world[1])),glm::dot(plane.axis,glm::vec3(world[2])));
            const float projected=glm::dot(plane.axis,center), radius=glm::dot(glm::abs(localAxis),extent);
            if(projected+radius<plane.minimum || projected-radius>plane.maximum) return true;
        }
        return false;
    }
};
}
