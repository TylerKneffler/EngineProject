// Scene settings round trip for SceneSettings::atmosphereFog through the
// on-disk XML transport (scenes store settings as XML attributes/elements).
#include "Core/Scene/Settings/SceneSettingsSerialization.h"
#include "Core/Serialization/SceneSerializerInternal.h"

#include <pugixml.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
void Require(bool value, const char* what)
{
    if (value) return;
    std::fprintf(stderr, "AtmosphereFogSettingsTests failed: %s\n", what);
    std::abort();
}

bool Near(float a, float b) { return std::abs(a - b) <= 1e-5f * (1.f + std::abs(b)); }

Engine::Model::SceneSettings RoundTripXml(const Engine::Model::SceneSettings& source)
{
    using Engine::Serialization::JsonValue;
    JsonValue root = JsonValue::MakeObject();
    root.Set("settings", Engine::Scene::SerializeSceneSettings(source));
    const std::string xml =
        Engine::Serialization::Detail::SceneXmlBehavior::WriteDocument(root);
    pugi::xml_document document;
    Require(static_cast<bool>(document.load_string(xml.c_str())), "XML parses");
    const JsonValue parsed =
        Engine::Serialization::Detail::SceneXmlBehavior::ReadNode(
            document.document_element());
    Engine::Model::SceneSettings result;
    Engine::Scene::DeserializeSceneSettings(result, parsed["settings"]);
    return result;
}
}

int main()
{
    Engine::Model::SceneSettings settings;
    auto& fog = settings.atmosphereFog;
    fog.enabled = true;
    fog.density = 0.00035f;
    fog.heightFalloff = 0.0021f;
    fog.baseHeight = -42.5f;
    fog.startDistance = 120.f;
    fog.maxOpacity = 0.9f;
    fog.inscatterColor = { 0.4f, 0.5f, 0.7f };
    fog.inscatterIntensity = 2.5f;
    fog.sunGlowStrength = 0.6f;
    fog.sunGlowAnisotropy = 0.8f;
    fog.skyDistance = 24000.f;

    const auto loaded = RoundTripXml(settings);
    const auto& read = loaded.atmosphereFog;
    Require(read.enabled, "enabled");
    Require(Near(read.density, fog.density), "density");
    Require(Near(read.heightFalloff, fog.heightFalloff), "heightFalloff");
    Require(Near(read.baseHeight, fog.baseHeight), "baseHeight");
    Require(Near(read.startDistance, fog.startDistance), "startDistance");
    Require(Near(read.maxOpacity, fog.maxOpacity), "maxOpacity");
    Require(Near(read.inscatterColor.x, 0.4f) && Near(read.inscatterColor.y, 0.5f) &&
        Near(read.inscatterColor.z, 0.7f), "inscatterColor");
    Require(Near(read.inscatterIntensity, fog.inscatterIntensity), "inscatterIntensity");
    Require(Near(read.sunGlowStrength, fog.sunGlowStrength), "sunGlowStrength");
    Require(Near(read.sunGlowAnisotropy, fog.sunGlowAnisotropy), "sunGlowAnisotropy");
    Require(Near(read.skyDistance, fog.skyDistance), "skyDistance");

    // A scene saved before the fog existed loads with fog off.
    Engine::Model::SceneSettings legacySource;
    auto legacyJson = Engine::Scene::SerializeSceneSettings(legacySource);
    Engine::Serialization::JsonValue withoutFog =
        Engine::Serialization::JsonValue::MakeObject();
    for (std::size_t index = 0; index < legacyJson.ObjectSize(); ++index)
        if (legacyJson.ObjectKey(index) != "atmosphereFog")
            withoutFog.Set(legacyJson.ObjectKey(index).c_str(),
                legacyJson.ObjectValue(index));
    Engine::Model::SceneSettings legacy;
    legacy.atmosphereFog.enabled = true; // stale state from a previous scene
    Engine::Scene::DeserializeSceneSettings(legacy, withoutFog);
    Require(!legacy.atmosphereFog.enabled, "legacy scene resets fog");

    // Out-of-range values are clamped on load.
    settings.atmosphereFog.sunGlowAnisotropy = 3.f;
    settings.atmosphereFog.maxOpacity = -1.f;
    const auto clamped = RoundTripXml(settings).atmosphereFog;
    Require(clamped.sunGlowAnisotropy <= 0.95f, "anisotropy clamp");
    Require(clamped.maxOpacity == 0.f, "opacity clamp");

    std::printf("AtmosphereFogSettingsTests passed\n");
    return 0;
}
