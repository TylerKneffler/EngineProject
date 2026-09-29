#include "LightProbe.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <glm/gtc/matrix_inverse.hpp>

namespace Engine::Components
{
namespace
{
Engine::Serialization::JsonValue Vec3Array(const std::vector<glm::vec3>& values)
{
    auto array=Engine::Serialization::JsonValue::MakeArray();
    for(const glm::vec3& value:values)
        array.Push(Engine::Serialization::JsonValue::MakeArray()
            .Push(Engine::Serialization::JsonValue(value.x))
            .Push(Engine::Serialization::JsonValue(value.y))
            .Push(Engine::Serialization::JsonValue(value.z)));
    return array;
}

void ReadVec3Array(const Engine::Serialization::JsonValue& value,
    std::vector<glm::vec3>& output)
{
    output.clear();
    if(!value.IsArray()) return;
    output.reserve(value.ArraySize());
    for(size_t index=0;index<value.ArraySize();++index)
    {
        const auto& item=value.ArrayAt(index);
        if(item.IsArray()&&item.ArraySize()>=3)
            output.push_back({item.ArrayAt(0).AsFloat(),
                item.ArrayAt(1).AsFloat(),item.ArrayAt(2).AsFloat()});
    }
}

glm::vec3 SafeDirection(glm::vec3 direction)
{
    return glm::dot(direction,direction)>.000001f
        ?glm::normalize(direction):glm::vec3(0.f,1.f,0.f);
}
}

LightProbe::LightProbe()
{
    SetTypeName(COMPONENT_TYPE_NAME(LightProbe));
    singlecomponent=true;
    RegisterField("irradiance",irradiance);
    RegisterField("directionalIrradiance",directionalIrradiance);
    RegisterField("lightDirection",lightDirection);
    RegisterField("valid",valid);
}

bool LightProbe::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    ui.DisabledLabel(valid?"Baked probe sample.":
        "Unbaked probe. Run Lighting > Bake Lighting.");
    char value[128]{};
    std::snprintf(value,sizeof(value),"%.3f, %.3f, %.3f",
        irradiance.x,irradiance.y,irradiance.z);
    ui.ValueLabel("Irradiance",value);
    std::snprintf(value,sizeof(value),"%.3f, %.3f, %.3f",
        lightDirection.x,lightDirection.y,lightDirection.z);
    ui.ValueLabel("Dominant Direction",value);
    return false;
}

LightProbeGroup::LightProbeGroup()
{
    SetTypeName(COMPONENT_TYPE_NAME(LightProbeGroup));
    singlecomponent=true;
    RegisterField("size",size);
    RegisterField("countX",countX);
    RegisterField("countY",countY);
    RegisterField("countZ",countZ);
    RegisterField("valid",valid);
}

size_t LightProbeGroup::SampleCount() const
{
    return static_cast<size_t>(std::clamp(countX,2,32))*
        static_cast<size_t>(std::clamp(countY,2,32))*
        static_cast<size_t>(std::clamp(countZ,2,32));
}

glm::vec3 LightProbeGroup::LocalSamplePosition(int x,int y,int z) const
{
    const glm::vec3 safeSize=glm::max(glm::abs(size),glm::vec3(.01f));
    return {-safeSize.x*.5f+safeSize.x*static_cast<float>(x)/
            static_cast<float>(std::max(1,countX-1)),
        -safeSize.y*.5f+safeSize.y*static_cast<float>(y)/
            static_cast<float>(std::max(1,countY-1)),
        -safeSize.z*.5f+safeSize.z*static_cast<float>(z)/
            static_cast<float>(std::max(1,countZ-1))};
}

void LightProbeGroup::ResizeSamples()
{
    countX=std::clamp(countX,2,32);
    countY=std::clamp(countY,2,32);
    countZ=std::clamp(countZ,2,32);
    size=glm::max(glm::abs(size),glm::vec3(.01f));
    const size_t count=SampleCount();
    irradiance.assign(count,glm::vec3(0.f));
    directionalIrradiance.assign(count,glm::vec3(0.f));
    lightDirections.assign(count,glm::vec3(0.f,1.f,0.f));
    valid=false;
}

bool LightProbeGroup::SampleAt(const glm::vec3& worldPosition,
    ProbeLightingSample& result) const
{
    if(!valid||!Owner||irradiance.size()!=SampleCount()||
        directionalIrradiance.size()!=SampleCount()||
        lightDirections.size()!=SampleCount())
        return false;
    const glm::mat4 world=Owner->transform.GetWorldMatrix();
    if(std::abs(glm::determinant(world))<.000001f) return false;
    const glm::vec3 local=glm::vec3(glm::inverse(world)*
        glm::vec4(worldPosition,1.f));
    const glm::vec3 safeSize=glm::max(glm::abs(size),glm::vec3(.01f));
    const glm::vec3 normalized=local/safeSize+glm::vec3(.5f);
    if(normalized.x<0.f||normalized.y<0.f||normalized.z<0.f||
        normalized.x>1.f||normalized.y>1.f||normalized.z>1.f)
        return false;
    const glm::vec3 grid=normalized*glm::vec3(countX-1,countY-1,countZ-1);
    const glm::ivec3 low=glm::min(glm::ivec3(grid),
        glm::ivec3(countX-2,countY-2,countZ-2));
    const glm::vec3 fraction=grid-glm::vec3(low);
    glm::vec3 direction(0.f);
    result={};
    for(int z=0;z<2;++z) for(int y=0;y<2;++y) for(int x=0;x<2;++x)
    {
        const float weight=(x?fraction.x:1.f-fraction.x)*
            (y?fraction.y:1.f-fraction.y)*(z?fraction.z:1.f-fraction.z);
        const size_t index=static_cast<size_t>(low.x+x)+
            static_cast<size_t>(countX)*(static_cast<size_t>(low.y+y)+
            static_cast<size_t>(countY)*static_cast<size_t>(low.z+z));
        result.irradiance+=irradiance[index]*weight;
        result.directionalIrradiance+=directionalIrradiance[index]*weight;
        direction+=lightDirections[index]*weight;
    }
    result.direction=SafeDirection(direction);
    result.valid=true;
    return true;
}

LightProbeGroup::JsonValue LightProbeGroup::Serialize() const
{
    JsonValue value=Component::Serialize();
    value.Set("samples",Vec3Array(irradiance));
    value.Set("directionalSamples",Vec3Array(directionalIrradiance));
    value.Set("sampleDirections",Vec3Array(lightDirections));
    return value;
}

void LightProbeGroup::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    countX=std::clamp(countX,2,32);
    countY=std::clamp(countY,2,32);
    countZ=std::clamp(countZ,2,32);
    size=glm::max(glm::abs(size),glm::vec3(.01f));
    ReadVec3Array(value["samples"],irradiance);
    ReadVec3Array(value["directionalSamples"],directionalIrradiance);
    ReadVec3Array(value["sampleDirections"],lightDirections);
    if(irradiance.size()!=SampleCount()||
        directionalIrradiance.size()!=SampleCount()||
        lightDirections.size()!=SampleCount())
        valid=false;
}

bool LightProbeGroup::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed=ui.DragFloat3("Size",&size.x,.1f,.01f,10000.f);
    changed=ui.SliderInt("Probes X",&countX,2,32)||changed;
    changed=ui.SliderInt("Probes Y",&countY,2,32)||changed;
    changed=ui.SliderInt("Probes Z",&countZ,2,32)||changed;
    if(changed) ResizeSamples();
    ui.DisabledLabel(valid?"Baked probe volume.":
        "Probe layout changed or is unbaked. Run Lighting > Bake Lighting.");
    const std::string count=std::to_string(SampleCount());
    ui.ValueLabel("Probe Count",count.c_str());
    return changed;
}

ProbeLightingSample SampleLightProbes(const Engine::Scene::Scene& scene,
    const glm::vec3& worldPosition)
{
    ProbeLightingSample result{};
    int groupCount=0;
    for(const auto& object:scene.GetObjects())
    {
        if(!object->IsEnabledInHierarchy()) continue;
        const auto* group=object->GetComponent<LightProbeGroup>();
        ProbeLightingSample sample{};
        if(group&&group->SampleAt(worldPosition,sample))
        {
            result.irradiance+=sample.irradiance;
            result.directionalIrradiance+=sample.directionalIrradiance;
            result.direction+=sample.direction;
            ++groupCount;
        }
    }
    if(groupCount>0)
    {
        const float inverse=1.f/static_cast<float>(groupCount);
        result.irradiance*=inverse;
        result.directionalIrradiance*=inverse;
        result.direction=SafeDirection(result.direction);
        result.valid=true;
        return result;
    }

    struct Candidate{const LightProbe* probe;float distanceSquared;};
    std::vector<Candidate> candidates;
    for(const auto& object:scene.GetObjects())
        if(object->IsEnabledInHierarchy())
            if(const auto* probe=object->GetComponent<LightProbe>();probe&&probe->valid)
            {
                const glm::vec3 delta=
                    object->transform.GetWorldPosition()-worldPosition;
                candidates.push_back({probe,glm::dot(delta,delta)});
            }
    std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,
        const Candidate& b){return a.distanceSquared<b.distanceSquared;});
    float totalWeight=0.f;
    glm::vec3 direction(0.f);
    for(size_t index=0;index<std::min<size_t>(4,candidates.size());++index)
    {
        const float weight=1.f/std::max(candidates[index].distanceSquared,.01f);
        const LightProbe& probe=*candidates[index].probe;
        result.irradiance+=probe.irradiance*weight;
        result.directionalIrradiance+=probe.directionalIrradiance*weight;
        direction+=probe.lightDirection*weight;
        totalWeight+=weight;
    }
    if(totalWeight>0.f)
    {
        result.irradiance/=totalWeight;
        result.directionalIrradiance/=totalWeight;
        result.direction=SafeDirection(direction);
        result.valid=true;
    }
    return result;
}
}
