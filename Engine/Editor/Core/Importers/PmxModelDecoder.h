#pragma once

#include "Core/Model/ImportedModel.h"
#include <string>
#include <vector>

namespace Engine::Editor
{
struct PmxImportOptions
{
    bool morphs = true;
    bool rigConstraints = true;
    bool followBoneColliders = true;
    bool dynamicPhysics = false;
    bool weightFittedColliders = false;
};

struct PmxImportReport
{
    std::vector<std::string> warnings;
    int importedSupportedGroupMorphs = 0;
    int importedKinematicRigidBodies = 0;
    int importedDynamicRigidBodies = 0;
    int importedPhysicsJoints = 0;
    int importedSoftBodies = 0;
    int importedAppendRules = 0;
    int importedIkChains = 0;
    int importedBoneMorphs = 0;
    int importedMaterialMorphs = 0;
};

class PmxModelDecoder
{
public:
    static bool Decode(const std::string& sourcePath,
        Engine::Model::ImportedModel& model, std::string& error,
        PmxImportReport* report = nullptr,
        PmxImportOptions options = {});
};
}
