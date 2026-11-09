#pragma once

#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Object.h"
#include "Core/Model/GraphicsQualitySettings.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <glm/glm.hpp>

#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif

namespace Engine::Scene::Renderer
{
class SceneRendererUtilities
{
public:
    static uint32_t ExpandedCapacity(uint32_t current, uint32_t required)
    {
        uint32_t capacity = std::max(1u, current);
        while (capacity < required)
        {
            if (capacity > std::numeric_limits<uint32_t>::max() / 2u)
                return required;
            capacity *= 2u;
        }
        return capacity;
    }

    static std::string ShaderPath(const char* fileName)
    {
        const std::filesystem::path shaderFile(fileName);
        const std::filesystem::path relativePath =
            shaderFile.stem() / shaderFile;
        const std::filesystem::path bundled =
            std::filesystem::path("Engine") / "Shaders" / relativePath;
        if (std::filesystem::is_regular_file(bundled))
            return bundled.string();
        return (std::filesystem::path(ENGINE_SHADERS_PATH) / relativePath).string();
    }

    static bool IsFastFoliageSurface(const Engine::Components::Material* material)
    {
        if (!material || material->baseColorUvSet != 0 ||
            material->GetAlphaMode() == Engine::Components::MaterialAlphaMode::Blend)
            return false;
        return material->shader == "Assets/Shaders/Foliage.hlsl" ||
            material->shader == "Assets/Shaders/GrassFootBend.hlsl" ||
            material->shader == "Assets/Shaders/Diagnostics/FoliageAlphaMask.hlsl";
    }

    static bool ReadEnvFlag(const char* name, bool fallback)
    {
#if defined(_MSC_VER)
        char* value = nullptr;
        size_t valueLength = 0;
        if (_dupenv_s(&value, &valueLength, name) != 0 || !value)
            return fallback;
        const char first = value[0];
        std::free(value);
#else
        const char* value = std::getenv(name);
        if (!value || !*value)
            return fallback;
#endif
        if (first == '\0')
            return fallback;
        if (first == '0')
            return false;
        const char lower = static_cast<char>(
            std::tolower(static_cast<unsigned char>(first)));
        return lower != 'f' && lower != 'n';
    }

    static bool IsObjectOrDescendant(const Engine::Core::Object* object, const Engine::Core::Object* root)
    {
        for (const Engine::Core::Object* current = object; current; current = current->Parent)
            if (current == root)
                return true;
        return false;
    }

    static bool IsBeyondQualityDistance(
        const Engine::Components::Mesh* mesh,
        const glm::mat4& world,
        const glm::vec3& cameraPosition,
        const Engine::Components::Camera* camera,
        const Engine::Model::GraphicsQualityValues& quality,
        bool vegetation)
    {
        if (!mesh || !mesh->HasBounds() || !camera)
            return false;
        const auto bounds = Engine::Rendering::TransformBounds(
            mesh->GetBoundsMin(), mesh->GetBoundsMax(), world);
        const auto delta = glm::clamp(cameraPosition, bounds.minimum, bounds.maximum) - cameraPosition;
        const float range = camera->farPlane * quality.viewDistance * (vegetation ? quality.vegetation : 1.f);
        return glm::dot(delta, delta) > range * range;
    }

    static bool ShadowCasterWithinBudget(
        const Engine::Components::Mesh* mesh,
        const glm::mat4& world,
        const glm::vec3& cameraPosition,
        const glm::vec2& casterFade,
        float budget)
    {
        if (!mesh || !mesh->HasBounds() || casterFade.x <= 0.f)
            return true;
        const auto bounds = Engine::Rendering::TransformBounds(
            mesh->GetBoundsMin(), mesh->GetBoundsMax(), world);
        const auto delta = glm::clamp(cameraPosition, bounds.minimum, bounds.maximum) - cameraPosition;
        const float effectiveBudget = std::max(0.f, budget);
        return glm::dot(delta, delta) <= effectiveBudget * effectiveBudget;
    }

    static void HashRevision(uint64_t& hash, uint64_t value)
    {
        constexpr uint64_t prime = 1099511628211ull;
        for (unsigned byte = 0; byte < sizeof(value); ++byte)
        {
            hash ^= (value >> (byte * 8u)) & 0xffu;
            hash *= prime;
        }
    }

    static float RadicalInverseVdc(uint32_t bits)
    {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) |
            ((bits & 0xaaaaaaaau) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) |
            ((bits & 0xccccccccu) >> 2u);
        bits = ((bits & 0x0f0f0f0fu) << 4u) |
            ((bits & 0xf0f0f0f0u) >> 4u);
        bits = ((bits & 0x00ff00ffu) << 8u) |
            ((bits & 0xff00ff00u) >> 8u);
        return static_cast<float>(bits) * 2.3283064365386963e-10f;
    }
};

class BrdfIntegration
{
public:
    static glm::vec2 Integrate(float noV, float roughness)
    {
        constexpr uint32_t sampleCount = 256u;
        constexpr float twoPi = 6.28318530717958647692f;
        const glm::vec3 normal(0.f, 0.f, 1.f);
        const glm::vec3 view(std::sqrt(std::max(0.f, 1.f - noV * noV)),
            0.f, noV);
        float scale = 0.f;
        float bias = 0.f;
        const float alpha = roughness * roughness;
        const float alphaSquared = alpha * alpha;
        const float geometryK = alpha * 0.5f;
        for (uint32_t sample = 0; sample < sampleCount; ++sample)
        {
            const float xiX = static_cast<float>(sample) /
                static_cast<float>(sampleCount);
            const float xiY = SceneRendererUtilities::RadicalInverseVdc(sample);
            const float phi = twoPi * xiX;
            const float cosTheta = std::sqrt(std::max(0.f,
                (1.f - xiY) /
                (1.f + (alphaSquared - 1.f) * xiY)));
            const float sinTheta = std::sqrt(std::max(0.f,
                1.f - cosTheta * cosTheta));
            const glm::vec3 halfVector(
                std::cos(phi) * sinTheta,
                std::sin(phi) * sinTheta,
                cosTheta);
            const glm::vec3 light = glm::normalize(
                2.f * glm::dot(view, halfVector) * halfVector - view);
            const float noL = std::max(light.z, 0.f);
            const float noH = std::max(halfVector.z, 0.f);
            const float voH = std::max(glm::dot(view, halfVector), 0.f);
            if (noL <= 0.f || noH <= 0.f || voH <= 0.f)
                continue;
            const float gv = noV /
                (noV * (1.f - geometryK) + geometryK);
            const float gl = noL /
                (noL * (1.f - geometryK) + geometryK);
            const float visibility = gv * gl * voH /
                std::max(noH * noV, 0.00001f);
            const float fresnel = std::pow(1.f - voH, 5.f);
            scale += (1.f - fresnel) * visibility;
            bias += fresnel * visibility;
        }
        return glm::vec2(scale, bias);
    }
};

inline uint32_t ExpandedCapacity(uint32_t current, uint32_t required)
{
    return SceneRendererUtilities::ExpandedCapacity(current, required);
}

inline std::string EngineShaderPath(const char* fileName)
{
    return SceneRendererUtilities::ShaderPath(fileName);
}

inline bool IsFastFoliageSurface(const Engine::Components::Material* material)
{
    return SceneRendererUtilities::IsFastFoliageSurface(material);
}

inline bool ReadEnvFlag(const char* name, bool fallback)
{
    return SceneRendererUtilities::ReadEnvFlag(name, fallback);
}

inline bool IsObjectOrDescendant(const Engine::Core::Object* object, const Engine::Core::Object* root)
{
    return SceneRendererUtilities::IsObjectOrDescendant(object, root);
}

inline bool IsBeyondQualityDistance(
    const Engine::Components::Mesh* mesh,
    const glm::mat4& world,
    const glm::vec3& cameraPosition,
    const Engine::Components::Camera* camera,
    const Engine::Model::GraphicsQualityValues& quality,
    bool vegetation)
{
    return SceneRendererUtilities::IsBeyondQualityDistance(mesh, world, cameraPosition, camera, quality, vegetation);
}

inline bool ShadowCasterWithinBudget(
    const Engine::Components::Mesh* mesh,
    const glm::mat4& world,
    const glm::vec3& cameraPosition,
    const glm::vec2& casterFade,
    float budget)
{
    return SceneRendererUtilities::ShadowCasterWithinBudget(mesh, world, cameraPosition, casterFade, budget);
}

inline void HashRevision(uint64_t& hash, uint64_t value)
{
    SceneRendererUtilities::HashRevision(hash, value);
}

inline float RadicalInverseVdc(uint32_t bits)
{
    return SceneRendererUtilities::RadicalInverseVdc(bits);
}

inline glm::vec2 IntegrateBrdf(float noV, float roughness)
{
    return BrdfIntegration::Integrate(noV, roughness);
}
}
