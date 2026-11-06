#include "Core/Rendering/GrassInteraction.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Renderers/HlslFileInclude.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
using namespace Engine::Rendering;
void Check(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
ComPtr<ID3DBlob> Compile(const wchar_t* path, const char* entry, const char* profile)
{
    ComPtr<ID3DBlob> code, errors;
    Engine::Renderers::HlslFileInclude includes(std::filesystem::path(path).string());
    const auto result = D3DCompileFromFile(path, nullptr, &includes,
        entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(result)) throw std::runtime_error(errors
        ? static_cast<const char*>(errors->GetBufferPointer()) : "Shader compilation failed");
    return code;
}
void CheckMaterialMigration()
{
    using Engine::Serialization::JsonParse;
    Engine::Components::Material material;
    material.Deserialize(JsonParse(R"({"shaders":[{"path":"off.hlsl","enabled":false},{"path":"surface.hlsl"},{"path":"overlay.hlsl"}]})"));
    Check(material.shader == "surface.hlsl", "legacy surface selection failed");
    const auto saved = material.Serialize();
    Check(saved["shader"].AsString() == "surface.hlsl" && !saved.Has("shaders"), "legacy list serialized again");
    material.Deserialize(JsonParse(R"({"shader":"chosen.hlsl","shaders":["legacy.hlsl"]})"));
    Check(material.shader == "chosen.hlsl", "new shader field did not take precedence");
    material.Deserialize(JsonParse(R"({"shaders":["", "overlay.hlsl"]})"));
    Check(material.shader.empty(), "built-in surface was replaced by an overlay");
}
void CheckCapsules()
{
    const auto world = glm::translate(glm::mat4(1), glm::vec3(10, 0, 0)) *
        glm::scale(glm::mat4(1), glm::vec3(2, 2, 3));
    const auto capsule = MakeGrassInteractionCapsule(world, {0, .5f, 0}, .5f, 2);
    Check(glm::length(glm::vec3(capsule.startRadius) - glm::vec3(10, .5f, 0)) < .001f &&
        std::abs(capsule.startRadius.w - 1.5f) < .001f, "capsule world geometry mismatch");
    std::vector<GrassInteractionCapsule> nearby;
    for (int i = 6; i > 0; --i)
        nearby.push_back(MakeGrassInteractionCapsule(glm::translate(glm::mat4(1), glm::vec3(i, 0, 0)), {}, .5f, 2));
    nearby.push_back(MakeGrassInteractionCapsule(glm::translate(glm::mat4(1), glm::vec3(60, 0, 0)), {}, .5f, 2));
    const auto selection = SelectGrassInteractionCapsules(nearby, {});
    Check(selection.count == 4 && selection.data[0].x == 1 && selection.data[6].x == 4,
        "nearest-capsule cap failed");
    Engine::Scene::Scene scene;
    auto* player = scene.AddObject("Player");
    player->AddComponent<Engine::Components::RigidBody>()->bodyType = "Kinematic";
    auto* collider = player->AddComponent<Engine::Components::PrimitiveObjectCollider>();
    collider->shape = "Capsule";
    Check(CaptureGrassInteractionCapsules(scene).size() == 1, "player capsule missing");
    collider->bendGrass = false;
    Check(CaptureGrassInteractionCapsules(scene).empty(), "capsule opt-out ignored");
    collider->bendGrass = true;
    player->GetComponent<Engine::Components::RigidBody>()->bodyType = "Static";
    Check(CaptureGrassInteractionCapsules(scene).empty(), "static capsule included");
    player->GetComponent<Engine::Components::RigidBody>()->bodyType = "Kinematic";
    player->AddComponent<Engine::Components::AnimationBone>();
    Check(CaptureGrassInteractionCapsules(scene).empty(), "bone capsule included");
}
void CheckGpuBending()
{
    const auto code = Compile(L"Tests/Shaders/GrassFootBendTests.hlsl", "CSMain", "cs_5_0");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)), "WARP device failed");
    ComPtr<ID3D11ComputeShader> shader;
    Check(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader)), "compute shader failed");
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = 16 * sizeof(glm::vec4);
    description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    description.StructureByteStride = sizeof(glm::vec4);
    ComPtr<ID3D11Buffer> output, readback;
    Check(SUCCEEDED(device->CreateBuffer(&description, nullptr, &output)), "output buffer failed");
    ComPtr<ID3D11UnorderedAccessView> view;
    Check(SUCCEEDED(device->CreateUnorderedAccessView(output.Get(), nullptr, &view)), "output view failed");
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.MiscFlags = 0;
    description.StructureByteStride = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(SUCCEEDED(device->CreateBuffer(&description, nullptr, &readback)), "readback buffer failed");
    auto* rawView = view.Get();
    context->CSSetShader(shader.Get(), nullptr, 0);
    context->CSSetUnorderedAccessViews(0, 1, &rawView, nullptr);
    context->Dispatch(1, 1, 1);
    rawView = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &rawView, nullptr);
    context->CopyResource(readback.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "readback map failed");
    std::array<glm::vec4, 16> values;
    std::memcpy(values.data(), mapped.pData, sizeof(values));
    context->Unmap(readback.Get(), 0);
    for (const auto& value : values)
        for (int i = 0; i < 4; ++i) Check(std::isfinite(value[i]), "non-finite bend output");
    Check(glm::length(glm::vec3(values[0]) - glm::vec3(.2f, 0, 0)) < .0001f, "grass root moved");
    Check(values[2].x > .5f && values[2].y > 0 && values[2].y < 1, "near tip did not bend away");
    Check(glm::length(glm::vec3(values[4]) - glm::vec3(4, 1, 0)) < .0001f, "distant grass bent");
    Check(glm::length(glm::vec3(values[6]) - glm::vec3(.2f, 1, 0)) < .0001f, "airborne capsule bent ground grass");
    Check(glm::length(glm::vec3(values[8]) - glm::vec3(.2f, 1, 0)) < .0001f, "zero-capsule grass changed");
    Check(values[10].x > 0 && values[10].y >= 0, "center-of-foot fallback failed");
    Check(values[12].x - .8f < values[2].x - .2f, "interaction falloff failed");
    Check(glm::length(glm::vec3(values[14]) - glm::vec3(0, 1, 0)) < .0001f, "overlapping capsules amplified the bend");
}
int main()
{
    try
    {
        CheckMaterialMigration(); CheckCapsules(); CheckGpuBending();
        Compile(L"Engine/Core/Assets/Shaders/GrassFootBend.hlsl", "VSMain", "vs_5_0");
        Compile(L"Engine/Core/Assets/Shaders/GrassFootBend.hlsl", "PSMain", "ps_5_0");
        std::cout << "Single material shader migration, capsule filtering, and GPU grass bending passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
