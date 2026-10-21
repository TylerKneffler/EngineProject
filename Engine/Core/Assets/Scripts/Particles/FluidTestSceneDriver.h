#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <string>

// Scene-only choreography for the fluid basin and its moving rigid paddle.
class FluidTestSceneDriver final : public Engine::Core::Script
{
public:
    FluidTestSceneDriver();

    PROPERTY(Inspector, EditAnywhere, Category = "Fluid Demo")
    std::string fluidObjectName = "Fluid Basin | water";
    PROPERTY(Inspector, EditAnywhere, Category = "Fluid Demo")
    std::string paddleObjectName = "Fluid Basin | moving paddle";
    PROPERTY(Inspector, EditAnywhere, Category = "Fluid Demo")
    float restartSeconds = 12.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fluid Demo")
    float paddleAmplitude = 0.3f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fluid Demo")
    float paddleCyclesPerSecond = 0.22f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fluid Demo")
    glm::vec3 poolSpawnPosition{-0.35f, 0.25f, 0.f};
    PROPERTY(Inspector, EditAnywhere, Category = "Fluid Demo")
    glm::vec3 poolSpawnHalfExtents{0.63f, 0.06f, 0.28f};

    void Start() override;
    void Update() override;

private:
    float m_elapsed = 0.f;
    float m_sinceRestart = 0.f;
    glm::vec3 m_paddleOrigin{0.f};
    glm::vec3 m_streamOrigin{0.f};
    glm::vec3 m_streamHalfExtents{0.f};
    bool m_poolSpawnPending = false;
};
