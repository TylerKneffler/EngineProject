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
// centerStep > 0 (the far cascade): the light-space center moves in steps of
// about that many metres instead of one texel, and the radius grows to keep
// the receiver sphere covered anywhere within a step, so a cached far tile is
// re-rendered only when the camera crosses a step.
inline StableShadowCascade BuildStableShadowCascade(glm::vec3 cameraPosition,
    glm::vec3 directionToLight,float receiverDistance,float shadowDistance,
    unsigned tileResolution,float centerStep=0.f)
{
    StableShadowCascade result;
    directionToLight=glm::normalize(directionToLight);
    const auto up=std::abs(directionToLight.y)>.98f
        ? glm::vec3(0,0,1):glm::vec3(0,1,0);
    const auto orientation=glm::lookAtLH(glm::vec3(0),-directionToLight,up);
    // One metre of slack allows bounded staggered updates during walking.
    // Two texels additionally cover snapping and the filter footprint.
    const float resolution=float(std::max(8u,tileResolution));
    centerStep=std::max(0.f,centerStep);
    // A center snapped to the step grid lies within half a step diagonal
    // (x and y in light space) of the camera.
    result.radius=(float(std::ceil(double(receiverDistance)*16.0)/16.0)+1.f+
        centerStep*.7072f)/(1.f-4.f/resolution);
    const float texel=2.f*result.radius/resolution;
    // Whole texels, so a stepped center never shifts the texel grid.
    const float step=centerStep>texel ? std::round(centerStep/texel)*texel:texel;
    auto lightCenter=glm::vec3(orientation*glm::vec4(cameraPosition,1));
    // Depth (z) takes the same step: the depth range (2 * depthExtent)
    // spans casters far beyond half a step either way.
    for(unsigned axis=0;axis<3;++axis)
        lightCenter[axis]=std::round(lightCenter[axis]/step)*step;
    result.center=glm::vec3(glm::inverse(orientation)*glm::vec4(lightCenter,1));
    // Keep a common depth scale across cascades, including upstream casters.
    const float depthExtent=shadowDistance+2.f;
    const auto lightView=glm::lookAtLH(result.center+directionToLight*depthExtent,
        result.center,up);
    // Reverse-Z like every depth buffer (Graphics/DepthConvention.h): near
    // and far swapped, so casters nearest the light store 1.
    result.viewProjection=glm::orthoLH_ZO(-result.radius,result.radius,
        -result.radius,result.radius,2.f*depthExtent,.1f)*lightView;
    return result;
}
}
