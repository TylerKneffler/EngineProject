#include "Core/Scene/Settings/SceneSettingsSerialization.h"
#include <algorithm>
#include <cctype>
#include <utility>

namespace Engine::Scene
{
namespace
{
    Engine::Serialization::JsonValue Vec3ToJson(const glm::vec3& value)
    {
        return Engine::Serialization::JsonValue::MakeArray()
            .Push(Engine::Serialization::JsonValue(value.x))
            .Push(Engine::Serialization::JsonValue(value.y))
            .Push(Engine::Serialization::JsonValue(value.z));
    }

    glm::vec3 Vec3FromJson(const Engine::Serialization::JsonValue& value, const glm::vec3& fallback)
    {
        if (!value.IsArray() || value.ArraySize() < 3)
            return fallback;
        return {
            value.ArrayAt(0).AsFloat(),
            value.ArrayAt(1).AsFloat(),
            value.ArrayAt(2).AsFloat()
        };
    }

    const Engine::Serialization::JsonValue* FindField(
        const Engine::Serialization::JsonValue& object,
        const char* key)
    {
        if (!object.IsObject() || !key)
            return nullptr;
        if (object.Has(key))
            return &object[key];

        std::string loweredKey(key);
        std::transform(loweredKey.begin(), loweredKey.end(), loweredKey.begin(),
            [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });

        for (std::size_t index = 0; index < object.ObjectSize(); ++index)
        {
            std::string candidate = object.ObjectKey(index);
            std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                [](unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                });
            if (candidate == loweredKey)
                return &object.ObjectValue(index);
        }
        return nullptr;
    }
}

Engine::Serialization::JsonValue SerializeSceneSettings(const Engine::Model::SceneSettings& settings)
{
    Engine::Serialization::JsonValue value = Engine::Serialization::JsonValue::MakeObject();
    auto quality = Engine::Serialization::JsonValue::MakeObject();
    quality.Set("preset", Engine::Serialization::JsonValue(settings.graphicsQuality.preset));
    auto overrides = settings.graphicsQuality.overrides;
    auto channels = Engine::Model::GraphicsQualitySettings::Channels(overrides);
    for (size_t i=0;i<channels.size();++i)
        quality.Set(Engine::Model::GraphicsQualitySettings::Keys[i], Engine::Serialization::JsonValue(*channels[i]));
    value.Set("graphicsQuality", quality);
    value.Set("dimension",
        Engine::Serialization::JsonValue(static_cast<int>(settings.dimension)));
    value.Set("showGrid", Engine::Serialization::JsonValue(settings.showGrid));
    value.Set("gridHalfSize", Engine::Serialization::JsonValue(settings.gridHalfSize));
    value.Set("gridCellSize", Engine::Serialization::JsonValue(settings.gridCellSize));
    value.Set("gridOpacity", Engine::Serialization::JsonValue(settings.gridOpacity));
    value.Set("gridFadeDistance", Engine::Serialization::JsonValue(settings.gridFadeDistance));
    value.Set("gridColor", Vec3ToJson(settings.gridColor));
    value.Set("gridOriginColor", Vec3ToJson(settings.gridOriginColor));
    value.Set("ambientColor", Vec3ToJson(settings.ambientColor));
    value.Set("skyboxEnabled", Engine::Serialization::JsonValue(settings.skyboxEnabled));
    value.Set("skyboxTexture", Engine::Serialization::JsonValue(settings.skyboxTexture));
    value.Set("hdriLightingEnabled", Engine::Serialization::JsonValue(settings.hdriLightingEnabled));
    value.Set("hdriIntensity", Engine::Serialization::JsonValue(settings.hdriIntensity));
    value.Set("hdriExposure", Engine::Serialization::JsonValue(settings.hdriExposure));
    value.Set("hdriRotation", Engine::Serialization::JsonValue(settings.hdriRotation));
    value.Set("toneMapping",
        Engine::Serialization::JsonValue(static_cast<int>(settings.toneMapping)));
    value.Set("textureLodEnabled", Engine::Serialization::JsonValue(settings.textureLod.enabled));
    value.Set("textureLodNearDistance", Engine::Serialization::JsonValue(settings.textureLod.nearDistance));
    value.Set("textureLodFarDistance", Engine::Serialization::JsonValue(settings.textureLod.farDistance));
    value.Set("textureLodMipBias", Engine::Serialization::JsonValue(settings.textureLod.mipBias));
    value.Set("textureLodSimplifyShading", Engine::Serialization::JsonValue(settings.textureLod.simplifyShading));
    value.Set("textureLodDisableAlphaAtFar", Engine::Serialization::JsonValue(settings.textureLod.disableAlphaAtFar));
    value.Set("textureLodAlphaMipBias", Engine::Serialization::JsonValue(settings.textureLod.alphaMipBias));
    value.Set("outputExposure", Engine::Serialization::JsonValue(settings.outputExposure));
    {
        const auto& fog = settings.atmosphereFog;
        auto fogValue = Engine::Serialization::JsonValue::MakeObject();
        fogValue.Set("enabled", Engine::Serialization::JsonValue(fog.enabled));
        fogValue.Set("density", Engine::Serialization::JsonValue(fog.density));
        fogValue.Set("heightFalloff", Engine::Serialization::JsonValue(fog.heightFalloff));
        fogValue.Set("baseHeight", Engine::Serialization::JsonValue(fog.baseHeight));
        fogValue.Set("startDistance", Engine::Serialization::JsonValue(fog.startDistance));
        fogValue.Set("maxOpacity", Engine::Serialization::JsonValue(fog.maxOpacity));
        fogValue.Set("inscatterColor", Vec3ToJson(fog.inscatterColor));
        fogValue.Set("inscatterIntensity", Engine::Serialization::JsonValue(fog.inscatterIntensity));
        fogValue.Set("sunGlowStrength", Engine::Serialization::JsonValue(fog.sunGlowStrength));
        fogValue.Set("sunGlowAnisotropy", Engine::Serialization::JsonValue(fog.sunGlowAnisotropy));
        fogValue.Set("skyDistance", Engine::Serialization::JsonValue(fog.skyDistance));
        fogValue.Set("lightShafts", Engine::Serialization::JsonValue(fog.lightShafts));
        fogValue.Set("lightShaftSamples", Engine::Serialization::JsonValue(fog.lightShaftSamples));
        fogValue.Set("lightShaftStrength", Engine::Serialization::JsonValue(fog.lightShaftStrength));
        value.Set("atmosphereFog", fogValue);
    }
    {
        const auto& clouds = settings.volumetricClouds;
        auto cloudValue = Engine::Serialization::JsonValue::MakeObject();
        cloudValue.Set("enabled", Engine::Serialization::JsonValue(clouds.enabled));
        cloudValue.Set("coverage", Engine::Serialization::JsonValue(clouds.coverage));
        cloudValue.Set("density", Engine::Serialization::JsonValue(clouds.density));
        cloudValue.Set("baseHeight", Engine::Serialization::JsonValue(clouds.baseHeight));
        cloudValue.Set("thickness", Engine::Serialization::JsonValue(clouds.thickness));
        cloudValue.Set("shapeScale", Engine::Serialization::JsonValue(clouds.shapeScale));
        cloudValue.Set("detail", Engine::Serialization::JsonValue(clouds.detail));
        cloudValue.Set("windSpeed", Engine::Serialization::JsonValue(clouds.windSpeed));
        cloudValue.Set("windDirection", Engine::Serialization::JsonValue(clouds.windDirection));
        cloudValue.Set("ambientColor", Vec3ToJson(clouds.ambientColor));
        cloudValue.Set("ambientIntensity", Engine::Serialization::JsonValue(clouds.ambientIntensity));
        cloudValue.Set("sunIntensity", Engine::Serialization::JsonValue(clouds.sunIntensity));
        cloudValue.Set("steps", Engine::Serialization::JsonValue(clouds.steps));
        cloudValue.Set("maxDistance", Engine::Serialization::JsonValue(clouds.maxDistance));
        value.Set("volumetricClouds", cloudValue);
    }
    {
        const auto& sky = settings.physicalSky;
        auto skyValue = Engine::Serialization::JsonValue::MakeObject();
        skyValue.Set("enabled", Engine::Serialization::JsonValue(sky.enabled));
        skyValue.Set("groundHeight", Engine::Serialization::JsonValue(sky.groundHeight));
        skyValue.Set("rayleighScale", Engine::Serialization::JsonValue(sky.rayleighScale));
        skyValue.Set("mieScale", Engine::Serialization::JsonValue(sky.mieScale));
        skyValue.Set("mieScaleHeight", Engine::Serialization::JsonValue(sky.mieScaleHeight));
        skyValue.Set("mieAnisotropy", Engine::Serialization::JsonValue(sky.mieAnisotropy));
        skyValue.Set("ozoneScale", Engine::Serialization::JsonValue(sky.ozoneScale));
        skyValue.Set("groundAlbedo", Vec3ToJson(sky.groundAlbedo));
        skyValue.Set("skyIntensity", Engine::Serialization::JsonValue(sky.skyIntensity));
        skyValue.Set("attenuateSun", Engine::Serialization::JsonValue(sky.attenuateSun));
        skyValue.Set("aerialPerspective", Engine::Serialization::JsonValue(sky.aerialPerspective));
        value.Set("physicalSky", skyValue);
    }
    {
        const auto& exposure = settings.autoExposure;
        auto exposureValue = Engine::Serialization::JsonValue::MakeObject();
        exposureValue.Set("enabled", Engine::Serialization::JsonValue(exposure.enabled));
        exposureValue.Set("minEv", Engine::Serialization::JsonValue(exposure.minEv));
        exposureValue.Set("maxEv", Engine::Serialization::JsonValue(exposure.maxEv));
        exposureValue.Set("speedUp", Engine::Serialization::JsonValue(exposure.speedUp));
        exposureValue.Set("speedDown", Engine::Serialization::JsonValue(exposure.speedDown));
        exposureValue.Set("lowPercent", Engine::Serialization::JsonValue(exposure.lowPercent));
        exposureValue.Set("highPercent", Engine::Serialization::JsonValue(exposure.highPercent));
        exposureValue.Set("groundWeight", Engine::Serialization::JsonValue(exposure.groundWeight));
        value.Set("autoExposure", exposureValue);
    }
    {
        const auto& occlusion = settings.ambientOcclusion;
        auto occlusionValue = Engine::Serialization::JsonValue::MakeObject();
        occlusionValue.Set("enabled", Engine::Serialization::JsonValue(occlusion.enabled));
        occlusionValue.Set("radius", Engine::Serialization::JsonValue(occlusion.radius));
        occlusionValue.Set("intensity", Engine::Serialization::JsonValue(occlusion.intensity));
        occlusionValue.Set("temporalBlend", Engine::Serialization::JsonValue(occlusion.temporalBlend));
        value.Set("ambientOcclusion", occlusionValue);
    }
    value.Set("bloomEnabled", Engine::Serialization::JsonValue(settings.bloomEnabled));
    value.Set("bloomIntensity", Engine::Serialization::JsonValue(settings.bloomIntensity));
    value.Set("colorGradingEnabled", Engine::Serialization::JsonValue(settings.colorGradingEnabled));
    value.Set("gradingShadowTint", Vec3ToJson(settings.gradingShadowTint));
    value.Set("gradingHighlightTint", Vec3ToJson(settings.gradingHighlightTint));
    value.Set("gradingBalance", Engine::Serialization::JsonValue(settings.gradingBalance));
    value.Set("gradingContrast", Engine::Serialization::JsonValue(settings.gradingContrast));
    value.Set("gradingSaturation", Engine::Serialization::JsonValue(settings.gradingSaturation));
    value.Set("sunDiscEnabled", Engine::Serialization::JsonValue(settings.sunDiscEnabled));
    value.Set("sunDiscAngularDiameter", Engine::Serialization::JsonValue(settings.sunDiscAngularDiameter));
    value.Set("sunDiscIntensity", Engine::Serialization::JsonValue(settings.sunDiscIntensity));
    value.Set("sunGlowIntensity", Engine::Serialization::JsonValue(settings.sunGlowIntensity));
    value.Set("renderMode",
        Engine::Serialization::JsonValue(static_cast<int>(settings.renderMode)));
    value.Set("renderScaleMode",
        Engine::Serialization::JsonValue(static_cast<int>(settings.renderScaleMode)));
    value.Set("renderScale",
        Engine::Serialization::JsonValue(settings.renderScale));
    value.Set("dynamicInternalRenderScale",
        Engine::Serialization::JsonValue(settings.dynamicInternalRenderScale));
    value.Set("temporalUpscaling",
        Engine::Serialization::JsonValue(settings.temporalUpscaling));
    value.Set("renderScaleTargetMs",
        Engine::Serialization::JsonValue(settings.renderScaleTargetMs));
    value.Set("renderScaleMinimum",
        Engine::Serialization::JsonValue(settings.renderScaleMinimum));
    value.Set("renderScaleMaximum",
        Engine::Serialization::JsonValue(settings.renderScaleMaximum));
    value.Set("sceneViewUiOverlay",
        Engine::Serialization::JsonValue(settings.sceneViewUiOverlay));
    value.Set("cpuOcclusionCulling",
        Engine::Serialization::JsonValue(settings.cpuOcclusionCulling));
    value.Set("prefabBuildBudgetMs",
        Engine::Serialization::JsonValue(settings.prefabBuildBudgetMs));
    value.Set("lodTransitionSeconds",
        Engine::Serialization::JsonValue(settings.lodTransitionSeconds));
    value.Set("portalDebugVisuals",
        Engine::Serialization::JsonValue(settings.portalDebugVisuals));
    value.Set("sceneCameraWarpLookThrough",
        Engine::Serialization::JsonValue(settings.sceneCameraWarpLookThrough));
    value.Set("portalDebugWireframe",
        Engine::Serialization::JsonValue(settings.portalDebugWireframe));
    value.Set("portalDebugTintRemoteView",
        Engine::Serialization::JsonValue(settings.portalDebugTintRemoteView));
    value.Set("portalDebugOverlayAlpha",
        Engine::Serialization::JsonValue(settings.portalDebugOverlayAlpha));
    value.Set("portalRecursionDepth",
        Engine::Serialization::JsonValue(settings.portalRecursionDepth));
    value.Set("portalConnectionRepeatLimit",
        Engine::Serialization::JsonValue(settings.portalConnectionRepeatLimit));
    value.Set("portalMaxViewsPerFrame",
        Engine::Serialization::JsonValue(settings.portalMaxViewsPerFrame));
    return value;
}

void DeserializeSceneSettings(Engine::Model::SceneSettings& settings, const Engine::Serialization::JsonValue& value)
{
    // Legacy documents must not inherit quality settings from the previous scene.
    settings.textureLod = {};
    settings.graphicsQuality = {};
    if (!value.IsObject())
        return;
    if (const auto* quality = FindField(value, "graphicsQuality")) {
        if (const auto* preset = FindField(*quality, "preset"))
            settings.graphicsQuality.preset = std::clamp(preset->AsInt(), 0,
                Engine::Model::GraphicsQualitySettings::AutoPreset);
        auto channels = Engine::Model::GraphicsQualitySettings::Channels(settings.graphicsQuality.overrides);
        for (size_t i=0;i<channels.size();++i)
            if (const auto* field = FindField(*quality, Engine::Model::GraphicsQualitySettings::Keys[i]))
                *channels[i] = field->AsFloat();
    }
    if (const auto* field = FindField(value, "textureLodEnabled")) settings.textureLod.enabled = field->AsBool();
    if (const auto* field = FindField(value, "textureLodNearDistance")) settings.textureLod.nearDistance = field->AsFloat();
    if (const auto* field = FindField(value, "textureLodFarDistance")) settings.textureLod.farDistance = field->AsFloat();
    if (const auto* field = FindField(value, "textureLodMipBias")) settings.textureLod.mipBias = field->AsFloat();
    if (const auto* field = FindField(value, "textureLodSimplifyShading")) settings.textureLod.simplifyShading = field->AsBool();
    if (const auto* field = FindField(value, "textureLodAlphaMipBias")) settings.textureLod.alphaMipBias = field->AsFloat();
    if (const auto* field = FindField(value, "textureLodDisableAlphaAtFar")) settings.textureLod.disableAlphaAtFar = field->AsBool();
    settings.textureLod.Normalize();
    if (const auto* dimension = FindField(value, "dimension"))
    {
        settings.dimension = dimension->AsInt() == 1
            ? Engine::Model::SceneDimension::TwoD
            : Engine::Model::SceneDimension::ThreeD;
    }
    if (const auto* showGrid = FindField(value, "showGrid"))
        settings.showGrid = showGrid->AsBool();
    if (const auto* gridHalfSize = FindField(value, "gridHalfSize"))
        settings.gridHalfSize = gridHalfSize->AsInt();
    if (const auto* gridCellSize = FindField(value, "gridCellSize"))
        settings.gridCellSize = gridCellSize->AsFloat();
    if (const auto* gridOpacity = FindField(value, "gridOpacity"))
        settings.gridOpacity = gridOpacity->AsFloat();
    if (const auto* gridFadeDistance = FindField(value, "gridFadeDistance"))
        settings.gridFadeDistance = gridFadeDistance->AsFloat();
    if (const auto* gridColor = FindField(value, "gridColor"))
        settings.gridColor = Vec3FromJson(*gridColor, settings.gridColor);
    if (const auto* gridOriginColor = FindField(value, "gridOriginColor"))
        settings.gridOriginColor = Vec3FromJson(*gridOriginColor, settings.gridOriginColor);
    if (const auto* ambientColor = FindField(value, "ambientColor"))
        settings.ambientColor = Vec3FromJson(*ambientColor, settings.ambientColor);
    if (const auto* skyboxEnabled = FindField(value, "skyboxEnabled"))
        settings.skyboxEnabled = skyboxEnabled->AsBool();
    else
        settings.skyboxEnabled = true;
    if (const auto* skyboxTexture = FindField(value, "skyboxTexture"))
        settings.skyboxTexture = skyboxTexture->AsString();
    else
        settings.skyboxTexture.clear();
    if (const auto* enabled = FindField(value, "hdriLightingEnabled"))
        settings.hdriLightingEnabled = enabled->AsBool();
    if (const auto* intensity = FindField(value, "hdriIntensity"))
        settings.hdriIntensity = std::max(0.f, intensity->AsFloat());
    if (const auto* exposure = FindField(value, "hdriExposure"))
        settings.hdriExposure = std::clamp(exposure->AsFloat(), -16.f, 16.f);
    if (const auto* rotation = FindField(value, "hdriRotation"))
        settings.hdriRotation = rotation->AsFloat();
    if (const auto* toneMapping = FindField(value, "toneMapping"))
    {
        settings.toneMapping = static_cast<Engine::Model::ToneMappingOperator>(
            std::clamp(toneMapping->AsInt(), 0, 3));
    }
    if (const auto* outputExposure = FindField(value, "outputExposure"))
        settings.outputExposure = std::clamp(
            outputExposure->AsFloat(), -16.f, 16.f);
    settings.atmosphereFog = {};
    if (const auto* fogValue = FindField(value, "atmosphereFog"))
    {
        auto& fog = settings.atmosphereFog;
        auto readFloat = [&](const char* key, float& target)
        {
            if (const auto* field = FindField(*fogValue, key))
                target = field->AsFloat();
        };
        if (const auto* enabled = FindField(*fogValue, "enabled"))
            fog.enabled = enabled->AsBool();
        readFloat("density", fog.density);
        readFloat("heightFalloff", fog.heightFalloff);
        readFloat("baseHeight", fog.baseHeight);
        readFloat("startDistance", fog.startDistance);
        readFloat("maxOpacity", fog.maxOpacity);
        if (const auto* color = FindField(*fogValue, "inscatterColor"))
            fog.inscatterColor = Vec3FromJson(*color, fog.inscatterColor);
        readFloat("inscatterIntensity", fog.inscatterIntensity);
        readFloat("sunGlowStrength", fog.sunGlowStrength);
        readFloat("sunGlowAnisotropy", fog.sunGlowAnisotropy);
        readFloat("skyDistance", fog.skyDistance);
        if (const auto* shafts = FindField(*fogValue, "lightShafts"))
            fog.lightShafts = shafts->AsBool();
        if (const auto* samples = FindField(*fogValue, "lightShaftSamples"))
            fog.lightShaftSamples = samples->AsInt();
        readFloat("lightShaftStrength", fog.lightShaftStrength);
        fog.Normalize();
    }
    settings.volumetricClouds = {};
    if (const auto* cloudValue = FindField(value, "volumetricClouds"))
    {
        auto& clouds = settings.volumetricClouds;
        auto readFloat = [&](const char* key, float& target)
        {
            if (const auto* field = FindField(*cloudValue, key))
                target = field->AsFloat();
        };
        if (const auto* enabled = FindField(*cloudValue, "enabled"))
            clouds.enabled = enabled->AsBool();
        readFloat("coverage", clouds.coverage);
        readFloat("density", clouds.density);
        readFloat("baseHeight", clouds.baseHeight);
        readFloat("thickness", clouds.thickness);
        readFloat("shapeScale", clouds.shapeScale);
        readFloat("detail", clouds.detail);
        readFloat("windSpeed", clouds.windSpeed);
        readFloat("windDirection", clouds.windDirection);
        if (const auto* color = FindField(*cloudValue, "ambientColor"))
            clouds.ambientColor = Vec3FromJson(*color, clouds.ambientColor);
        readFloat("ambientIntensity", clouds.ambientIntensity);
        readFloat("sunIntensity", clouds.sunIntensity);
        if (const auto* steps = FindField(*cloudValue, "steps"))
            clouds.steps = steps->AsInt();
        readFloat("maxDistance", clouds.maxDistance);
        clouds.Normalize();
    }
    settings.autoExposure = {};
    if (const auto* exposureValue = FindField(value, "autoExposure"))
    {
        auto& exposure = settings.autoExposure;
        if (const auto* field = FindField(*exposureValue, "enabled"))
            exposure.enabled = field->AsBool();
        const std::pair<const char*, float*> floats[] = {
            { "minEv", &exposure.minEv }, { "maxEv", &exposure.maxEv },
            { "speedUp", &exposure.speedUp }, { "speedDown", &exposure.speedDown },
            { "lowPercent", &exposure.lowPercent }, { "highPercent", &exposure.highPercent },
            { "groundWeight", &exposure.groundWeight } };
        for (const auto& [key, target] : floats)
            if (const auto* field = FindField(*exposureValue, key))
                *target = field->AsFloat();
        exposure.Normalize();
    }
    settings.ambientOcclusion = {};
    if (const auto* occlusionValue = FindField(value, "ambientOcclusion"))
    {
        auto& occlusion = settings.ambientOcclusion;
        if (const auto* field = FindField(*occlusionValue, "enabled"))
            occlusion.enabled = field->AsBool();
        const std::pair<const char*, float*> floats[] = {
            { "radius", &occlusion.radius }, { "intensity", &occlusion.intensity },
            { "temporalBlend", &occlusion.temporalBlend } };
        for (const auto& [key, target] : floats)
            if (const auto* field = FindField(*occlusionValue, key))
                *target = field->AsFloat();
        occlusion.Normalize();
    }
    settings.physicalSky = {};
    if (const auto* skyValue = FindField(value, "physicalSky"))
    {
        auto& sky = settings.physicalSky;
        auto readFloat = [&](const char* key, float& target)
        {
            if (const auto* field = FindField(*skyValue, key))
                target = field->AsFloat();
        };
        auto readBool = [&](const char* key, bool& target)
        {
            if (const auto* field = FindField(*skyValue, key))
                target = field->AsBool();
        };
        readBool("enabled", sky.enabled);
        readFloat("groundHeight", sky.groundHeight);
        readFloat("rayleighScale", sky.rayleighScale);
        readFloat("mieScale", sky.mieScale);
        readFloat("mieScaleHeight", sky.mieScaleHeight);
        readFloat("mieAnisotropy", sky.mieAnisotropy);
        readFloat("ozoneScale", sky.ozoneScale);
        if (const auto* albedo = FindField(*skyValue, "groundAlbedo"))
            sky.groundAlbedo = Vec3FromJson(*albedo, sky.groundAlbedo);
        readFloat("skyIntensity", sky.skyIntensity);
        readBool("attenuateSun", sky.attenuateSun);
        readBool("aerialPerspective", sky.aerialPerspective);
        sky.Normalize();
    }
    {
        // Older scenes predate these fields and keep the struct defaults.
        const Engine::Model::SceneSettings defaults{};
        const auto readBool = [&](const char* key, bool& target, bool fallback)
        {
            const auto* field = FindField(value, key);
            target = field ? field->AsBool() : fallback;
        };
        const auto readFloat = [&](const char* key, float& target, float fallback,
            float minimum, float maximum)
        {
            const auto* field = FindField(value, key);
            target = std::clamp(field ? field->AsFloat() : fallback, minimum, maximum);
        };
        const auto readColor = [&](const char* key, glm::vec3& target, glm::vec3 fallback)
        {
            const auto* field = FindField(value, key);
            target = glm::clamp(field ? Vec3FromJson(*field, fallback) : fallback,
                glm::vec3(0.f), glm::vec3(4.f));
        };
        readBool("bloomEnabled", settings.bloomEnabled, defaults.bloomEnabled);
        readFloat("bloomIntensity", settings.bloomIntensity, defaults.bloomIntensity, 0.f, 1.f);
        readBool("colorGradingEnabled", settings.colorGradingEnabled, defaults.colorGradingEnabled);
        readColor("gradingShadowTint", settings.gradingShadowTint, defaults.gradingShadowTint);
        readColor("gradingHighlightTint", settings.gradingHighlightTint, defaults.gradingHighlightTint);
        readFloat("gradingBalance", settings.gradingBalance, defaults.gradingBalance, -1.f, 1.f);
        readFloat("gradingContrast", settings.gradingContrast, defaults.gradingContrast, 0.25f, 4.f);
        readFloat("gradingSaturation", settings.gradingSaturation, defaults.gradingSaturation, 0.f, 4.f);
        readBool("sunDiscEnabled", settings.sunDiscEnabled, defaults.sunDiscEnabled);
        readFloat("sunDiscAngularDiameter", settings.sunDiscAngularDiameter,
            defaults.sunDiscAngularDiameter, 0.05f, 20.f);
        readFloat("sunDiscIntensity", settings.sunDiscIntensity, defaults.sunDiscIntensity, 0.f, 100000.f);
        readFloat("sunGlowIntensity", settings.sunGlowIntensity, defaults.sunGlowIntensity, 0.f, 100.f);
    }
    if (const auto* renderMode = FindField(value, "renderMode"))
    {
        const int mode = renderMode->AsInt();
        switch (mode)
        {
        case 1:
            settings.renderMode = Engine::Model::SceneRenderMode::Unlit;
            break;
        case 2:
            settings.renderMode = Engine::Model::SceneRenderMode::Wireframe;
            break;
        case 3:
            settings.renderMode = Engine::Model::SceneRenderMode::Performance;
            break;
        case 4:
            settings.renderMode = Engine::Model::SceneRenderMode::Detail;
            break;
        case 5:
            settings.renderMode = Engine::Model::SceneRenderMode::Triangles;
            break;
        default:
            settings.renderMode = Engine::Model::SceneRenderMode::Lit;
            break;
        }
    }
    if (const auto* renderScaleMode = FindField(value, "renderScaleMode"))
    {
        settings.renderScaleMode = static_cast<Engine::Model::RenderScaleMode>(
            std::clamp(renderScaleMode->AsInt(), 0, 3));
        settings.renderScale = Engine::Model::RenderQualitySettings::ScaleForMode(
            settings.renderScaleMode);
    }
    if (const auto* renderScale = FindField(value, "renderScale"))
        settings.renderScale = std::clamp(renderScale->AsFloat(), 0.5f, 1.0f);
    if (const auto* dynamicInternalRenderScale = FindField(value, "dynamicInternalRenderScale"))
        settings.dynamicInternalRenderScale = dynamicInternalRenderScale->AsBool();
    if (const auto* temporalUpscaling = FindField(value, "temporalUpscaling"))
        settings.temporalUpscaling = temporalUpscaling->AsBool();
    if (const auto* renderScaleTargetMs = FindField(value, "renderScaleTargetMs"))
        settings.renderScaleTargetMs = std::max(8.0f, renderScaleTargetMs->AsFloat());
    if (const auto* renderScaleMinimum = FindField(value, "renderScaleMinimum"))
        settings.renderScaleMinimum = std::clamp(renderScaleMinimum->AsFloat(), 0.5f, 1.0f);
    if (const auto* renderScaleMaximum = FindField(value, "renderScaleMaximum"))
        settings.renderScaleMaximum = std::clamp(renderScaleMaximum->AsFloat(), 0.5f, 1.0f);
    if (const auto* sceneViewUiOverlay = FindField(value, "sceneViewUiOverlay"))
        settings.sceneViewUiOverlay = sceneViewUiOverlay->AsBool();
    if (const auto* cpuOcclusion = FindField(value, "cpuOcclusionCulling"))
        settings.cpuOcclusionCulling = cpuOcclusion->AsBool();
    if (const auto* prefabBudget = FindField(value, "prefabBuildBudgetMs"))
        settings.prefabBuildBudgetMs = std::clamp(prefabBudget->AsFloat(), 0.25f, 50.f);
    if (const auto* lodTransition = FindField(value, "lodTransitionSeconds"))
        settings.lodTransitionSeconds = std::clamp(lodTransition->AsFloat(), 0.f, 2.f);
    if (const auto* portalDebugVisuals = FindField(value, "portalDebugVisuals"))
        settings.portalDebugVisuals = portalDebugVisuals->AsBool();
    if (const auto* sceneCameraWarpLookThrough = FindField(value,
        "sceneCameraWarpLookThrough"))
    {
        settings.sceneCameraWarpLookThrough =
            sceneCameraWarpLookThrough->AsBool();
    }
    if (const auto* portalDebugWireframe = FindField(value, "portalDebugWireframe"))
        settings.portalDebugWireframe = portalDebugWireframe->AsBool();
    if (const auto* portalDebugTintRemoteView = FindField(value, "portalDebugTintRemoteView"))
        settings.portalDebugTintRemoteView = portalDebugTintRemoteView->AsBool();
    if (const auto* portalDebugOverlayAlpha = FindField(value, "portalDebugOverlayAlpha"))
        settings.portalDebugOverlayAlpha = std::clamp(
            portalDebugOverlayAlpha->AsFloat(), 0.f, 1.f);
    if (const auto* portalRecursionDepth = FindField(value, "portalRecursionDepth"))
        settings.portalRecursionDepth = std::clamp(
            portalRecursionDepth->AsInt(), 1, 8);
    if (const auto* portalConnectionRepeatLimit = FindField(value,
        "portalConnectionRepeatLimit"))
    {
        settings.portalConnectionRepeatLimit = std::clamp(
            portalConnectionRepeatLimit->AsInt(), 1, 8);
    }
    if (const auto* portalMaxViews = FindField(value, "portalMaxViewsPerFrame"))
        settings.portalMaxViewsPerFrame = std::clamp(
            portalMaxViews->AsInt(), 1, 64);
}
}
