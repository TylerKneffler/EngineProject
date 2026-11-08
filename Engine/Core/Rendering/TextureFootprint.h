#pragma once
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine::Rendering
{
// Conservative projective Jacobian bound, in pixels per local unit. Includes
// FOV, zoom, actual target resolution, nonuniform scale and near-plane clipping.
inline float ProjectedTextureFootprint(glm::vec3 low, glm::vec3 high,
    const glm::mat4& localToClip, glm::vec2 viewport, float unitsPerUvRepeat)
{
    const float infinity=std::numeric_limits<float>::infinity();
    glm::vec4 clips[8];
    bool outside[6]={true,true,true,true,true,true};
    float minW=infinity,maxX=0.f,maxY=0.f;
    for(int corner=0;corner<8;++corner)
    {
        const auto p=clips[corner]=localToClip*glm::vec4(corner&1?high.x:low.x,
            corner&2?high.y:low.y,corner&4?high.z:low.z,1.f);
        if(!std::isfinite(p.w+p.x+p.y+p.z)) return infinity;
        outside[0]&=p.x < -p.w; outside[1]&=p.x > p.w;
        outside[2]&=p.y < -p.w; outside[3]&=p.y > p.w;
        outside[4]&=p.z < 0.f; outside[5]&=p.z > p.w;
        minW=std::min(minW,p.w);maxX=std::max(maxX,std::abs(p.x));maxY=std::max(maxY,std::abs(p.y));
    }
    for(bool rejected:outside) if(rejected) return 0.f;
    if(minW<=1e-5f || !(viewport.x>0.f && viewport.y>0.f)) return infinity;
    for(const auto& clip:clips) if(clip.z<=0.f) return infinity;
    const auto row=[&](int i) { return glm::vec3(localToClip[0][i],localToClip[1][i],localToClip[2][i]); };
    const float dw=glm::length(row(3))/(minW*minW);
    const float x=viewport.x*.5f*(glm::length(row(0))/minW+maxX*dw);
    const float y=viewport.y*.5f*(glm::length(row(1))/minW+maxY*dw);
    return std::hypot(x,y)*unitsPerUvRepeat;
}
}
