#include "Core/Model/SceneSettings.h"
#include "Core/Scene/Settings/SceneSettingsSerialization.h"
#include <limits>
#include <stdexcept>
#include <iostream>
void Check(bool condition) { if (!condition) throw std::runtime_error("Graphics quality regression"); }
int main()
{
    using namespace Engine::Model;
    SceneSettings scene;
    Check(scene.graphicsQuality.Resolve().geometry == 1.f);
    auto authored=scene.textureLod;
    Check(scene.graphicsQuality.ResolveTexture(authored).GpuParameters()==authored.GpuParameters());
    scene.graphicsQuality.preset=3;
    scene.graphicsQuality.overrides.texture=1.f;
    scene.graphicsQuality.overrides.material=1.f;
    Check(!scene.graphicsQuality.ResolveTexture(authored).enabled);
    scene.graphicsQuality.overrides.material=.5f;
    auto materialOnly=scene.graphicsQuality.ResolveTexture(authored);
    Check(materialOnly.enabled && materialOnly.simplifyShading && materialOnly.mipBias==0.f);
    scene.graphicsQuality.overrides.material=1.f;
    scene.graphicsQuality.overrides.texture=.5f;
    auto textureOnly=scene.graphicsQuality.ResolveTexture(authored);
    Check(textureOnly.enabled && !textureOnly.simplifyShading && textureOnly.mipBias==4.f);
    auto saved=Engine::Scene::SerializeSceneSettings(scene);
    SceneSettings loaded;
    Engine::Scene::DeserializeSceneSettings(loaded,saved);
    Check(loaded.graphicsQuality.preset==3 && loaded.graphicsQuality.overrides.texture==.5f);
    loaded.graphicsQuality.overrides.geometry=std::numeric_limits<float>::quiet_NaN();
    Check(loaded.graphicsQuality.Resolve().geometry==.45f);
    Engine::Scene::DeserializeSceneSettings(loaded,Engine::Serialization::JsonValue::MakeObject());
    Check(loaded.graphicsQuality.preset==0 && loaded.graphicsQuality.Resolve().renderScale==1.f);
    std::cout << "Graphics quality policy and serialization passed\n";
}
