#include "Scripts/GeneralizedBehaviors/MarchingCubesQuad.h"

#include "Core/Serialization/SceneSerializer.h"

MarchingCubesQuad::MarchingCubesQuad()
{
    SetTypeName(COMPONENT_TYPE_NAME(MarchingCubesQuad));
    RegisterField("quadX", quadX);
    RegisterField("quadZ", quadZ);
    RegisterField("triangleCount", triangleCount);
}

namespace
{
struct MarchingCubesQuadRegistration
{
    MarchingCubesQuadRegistration()
    {
        Engine::Serialization::RegisterComponentType<MarchingCubesQuad>(
            "MarchingCubesQuad");
    }
};
MarchingCubesQuadRegistration g_registration;
}
