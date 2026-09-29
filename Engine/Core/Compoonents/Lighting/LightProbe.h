#pragma once

#include "Core/component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <vector>

namespace Engine::Scene { class Scene; }

namespace Engine::Components
{
struct ProbeLightingSample
{
    glm::vec3 irradiance{0.f};
    glm::vec3 directionalIrradiance{0.f};
    glm::vec3 direction{0.f,1.f,0.f};
    bool valid=false;
};

// A manually positioned baked-light sample used by movable objects.
class LightProbe final : public Engine::Core::Component
{
public:
    LightProbe();

    glm::vec3 irradiance{0.f};
    glm::vec3 directionalIrradiance{0.f};
    glm::vec3 lightDirection{0.f,1.f,0.f};
    bool valid=false;

    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
};

// A box-shaped regular grid of baked probes. Samples are stored in X-major,
// then Y, then Z order and trilinearly interpolated at runtime.
class LightProbeGroup final : public Engine::Core::Component
{
public:
    LightProbeGroup();

    PROPERTY(Inspector,EditAnywhere,Category="Probe Volume")
    glm::vec3 size{10.f,5.f,10.f};
    PROPERTY(Inspector,EditAnywhere,Category="Probe Volume")
    int countX=4;
    PROPERTY(Inspector,EditAnywhere,Category="Probe Volume")
    int countY=3;
    PROPERTY(Inspector,EditAnywhere,Category="Probe Volume")
    int countZ=4;

    std::vector<glm::vec3> irradiance;
    std::vector<glm::vec3> directionalIrradiance;
    std::vector<glm::vec3> lightDirections;
    bool valid=false;

    size_t SampleCount() const;
    glm::vec3 LocalSamplePosition(int x,int y,int z) const;
    void ResizeSamples();
    bool SampleAt(const glm::vec3& worldPosition,
        ProbeLightingSample& result) const;

    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
};

ProbeLightingSample SampleLightProbes(const Engine::Scene::Scene& scene,
    const glm::vec3& worldPosition);
}
