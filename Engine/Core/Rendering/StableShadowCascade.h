#pragma once
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

namespace Engine::Rendering
{
struct StableShadowCascade
{
    glm::mat4 viewProjection{1.f};
    glm::vec3 center{};
    float radius=0.f;
};

// Cover a radial receiver band in every viewing direction. Neither camera
// orientation nor lens/aspect participates in the light-space texel grid.
inline StableShadowCascade BuildStableShadowCascade(glm::vec3 cameraPosition,
    glm::vec3 directionToLight,float receiverDistance,float shadowDistance,
    unsigned tileResolution)
{
    StableShadowCascade result;
    directionToLight=glm::normalize(directionToLight);
    const auto up=std::abs(directionToLight.y)>.98f
        ? glm::vec3(0,0,1):glm::vec3(0,1,0);
    const auto orientation=glm::lookAtLH(glm::vec3(0),-directionToLight,up);
    // One metre of slack allows bounded staggered updates during walking.
    // Two texels additionally cover snapping and the filter footprint.
    const float resolution=float(std::max(8u,tileResolution));
    result.radius=(float(std::ceil(double(receiverDistance)*16.0)/16.0)+1.f)/
        (1.f-4.f/resolution);
    const float texel=2.f*result.radius/resolution;
    auto lightCenter=glm::vec3(orientation*glm::vec4(cameraPosition,1));
    for(unsigned axis=0;axis<3;++axis)
        lightCenter[axis]=std::round(lightCenter[axis]/texel)*texel;
    result.center=glm::vec3(glm::inverse(orientation)*glm::vec4(lightCenter,1));
    // Keep a common depth scale across cascades, including upstream casters.
    const float depthExtent=shadowDistance+2.f;
    const auto lightView=glm::lookAtLH(result.center+directionToLight*depthExtent,
        result.center,up);
    result.viewProjection=glm::orthoLH_ZO(-result.radius,result.radius,
        -result.radius,result.radius,.1f,2.f*depthExtent)*lightView;
    return result;
}
}
