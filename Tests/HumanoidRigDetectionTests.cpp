#include "Core/AnimationEditorSource.h"
#include "Core/Model/AnimationRigMapping.h"
#include "Core/Compoonents/Animation/RigConstraints.h"
#include <pugixml.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace Engine;
namespace H = Model::HumanoidRig;

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::vector<H::Node> Avatar()
{
    std::vector<H::Node> nodes(H::Count);
    for (auto& node : nodes) node.joint = false;
    const auto set = [&](H::Role role, int parent, glm::vec3 position)
    { nodes[role] = {parent, position, true}; };
    set(H::Root, -1, {0, 0, 0}); set(H::Pelvis, H::Root, {0, 1, 0});
    set(H::Spine, H::Pelvis, {0, 1.2f, 0}); set(H::Chest, H::Spine, {0, 1.5f, 0});
    set(H::Neck, H::Chest, {0, 1.75f, 0}); set(H::Head, H::Neck, {0, 1.95f, 0});
    for (bool left : {true, false})
    {
        const float side = left ? 1.f : -1.f;
        const size_t arm = left ? H::LeftShoulder : H::RightShoulder;
        set(H::Role(arm), H::Chest, {side * .08f, 1.5f, 0});
        set(H::Role(arm + 1), int(arm), {side * .2f, 1.55f, 0});
        set(H::Role(arm + 2), int(arm + 1), {side * .5f, 1.3f, 0});
        set(H::Role(arm + 3), int(arm + 2), {side * .8f, 1.05f, 0});
        const size_t leg = left ? H::LeftThigh : H::RightThigh;
        set(H::Role(leg), H::Pelvis, {side * .12f, .95f, 0});
        set(H::Role(leg + 1), int(leg), {side * .12f, .5f, .01f});
        set(H::Role(leg + 2), int(leg + 1), {side * .12f, .08f, 0});
        set(H::Role(leg + 3), int(leg + 2), {side * .12f, .03f, .15f});
        for (int part = 0; part < 3; ++part)
            nodes[leg + part].name = std::string(left ? "Left " : "Right ") +
                (part == 0 ? "Leg" : part == 1 ? "Knee" : "Ankle");
    }
    return nodes;
}

int Find(const Model::RigAsset& rig, const std::string& suffix)
{
    for (size_t node = 0; node < rig.bindNodes.size(); ++node)
    {
        const auto& path = rig.bindNodes[node].path;
        if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0)
            return int(node);
    }
    throw std::runtime_error("Missing Mococo bone: " + suffix);
}

int main(int argc, char** argv)
{
    try
    {
        auto nodes = Avatar();
        const int duplicate = int(nodes.size());
        for (int part = 0; part < 3; ++part)
        {
            auto node = nodes[H::LeftThigh + part];
            if (part) node.parent = duplicate + part - 1;
            node.name += " D";
            nodes.push_back(std::move(node));
        }
        const auto detected = H::Detect(nodes);
        Check(detected.mapping.bones[H::LeftThigh] == H::LeftThigh &&
            detected.mapping.bones[H::LeftCalf] == H::LeftCalf &&
            detected.mapping.bones[H::LeftFoot] == H::LeftFoot,
            "Coincident D helpers displaced the primary leg chain");
        for (auto& node : nodes) node.name.clear();
        Check(H::Detect(nodes).mapping.bones[H::LeftThigh] < 0,
            "Indistinguishable nameless branches should remain unassigned");
        H::Mapping anchor; anchor.bones[H::LeftThigh] = H::LeftThigh;
        Check(H::Detect(nodes, anchor).mapping.bones[H::LeftFoot] == H::LeftFoot,
            "An assigned thigh did not disambiguate its own leg chain");

        const auto sourceRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
        const auto rigPath = sourceRoot /
            "Engine/Core/Assets/Models/mmd/Mococo/Mococo Abyssgard/Mococo Abyssgard.rig";
        auto bundle = Model::RigAsset::LoadAll(rigPath.string());
        Check(!bundle.empty(), "Mococo rig fixture is missing");
        auto& rig = bundle.front();
        std::vector<unsigned> joints;
        for (const auto& joint : rig.joints) joints.push_back(joint.nodeIndex);
        const auto mococoNodes = H::Nodes(rig.bindNodes, joints);
        const auto mococo = H::Detect(mococoNodes);
        Check(H::Validate(mococo.mapping, mococoNodes).empty(), "Mococo still has unassigned required body parts");
        const std::pair<H::Role, const char*> expected[] = {
            {H::Pelvis, "/Lower Body"}, {H::Spine, "/Upper Body"}, {H::Chest, "/Upper Body 2"},
            {H::LeftThigh, "/Left Leg"}, {H::LeftCalf, "/Left Knee"}, {H::LeftFoot, "/Left Ankle"},
            {H::RightThigh, "/Right Leg"}, {H::RightCalf, "/Right Knee"}, {H::RightFoot, "/Right Ankle"}
        };
        for (const auto& [role, suffix] : expected)
            Check(mococo.mapping.bones[role] == Find(rig, suffix), "Mococo mapped a helper instead of the primary body chain");

        // Retarget walking rotations from a nameless avatar to Mococo, with
        // a partially assigned target profile matching the reported failure.
        Model::AnimationClipAsset walking;
        const auto sourceNodes = Avatar();
        for (size_t node = 0; node < sourceNodes.size(); ++node)
        {
            Model::AnimationClipAsset::BaseNode bind;
            bind.path = "opaque_" + std::to_string(node);
            const int parent = sourceNodes[node].parent;
            if (parent >= 0) bind.parent = walking.bindSkeleton[parent].path;
            bind.position = sourceNodes[node].position - (parent >= 0 ? sourceNodes[parent].position : glm::vec3(0));
            walking.bindSkeleton.push_back(std::move(bind));
        }
        walking.baseSkeleton = walking.bindSkeleton;
        walking.humanoid = H::Detect(sourceNodes).mapping;
        walking.clip.clipName = "Walking regression";
        walking.clip.duration = 1.f;
        const H::Role legs[] = {H::LeftThigh, H::LeftCalf, H::LeftFoot, H::RightThigh, H::RightCalf, H::RightFoot};
        for (auto role : legs)
        {
            Model::AnimationChannel channel;
            channel.nodeIndex = unsigned(role); channel.targetPath = walking.bindSkeleton[role].path;
            channel.path = Model::AnimationChannel::Path::Rotation; channel.valueWidth = 4;
            channel.times = {0.f, .5f};
            channel.values = {0, 0, 0, 1, .258819f, 0, 0, .965926f};
            walking.clip.channels.push_back(std::move(channel));
        }
        Model::AnimationClipAsset target;
        target.clip.clipName = "Mococo bind";
        target.bindSkeleton = rig.bindNodes;
        Scene::Scene scene;
        Check(Serialization::SceneSerializer::LoadFromString(scene, Editor::AnimationEditorSource::ClipPreview(target)),
            "Could not create Mococo skeleton preview");
        auto* root = scene.GetObjects().front().get();
        auto* model = root->GetComponent<Components::Model>();
        std::string danceError;
        const auto dancePack = Model::AnimationPackAsset::Load((sourceRoot /
            "Engine/Core/Assets/Models/mmd/Mococo/Mococo Dance/dance1.animpack").string(), danceError);
        Check(dancePack && !dancePack->clips.empty(), "Could not load dance1 body fixture");
        Model::AnimationClipAsset danceSource;
        danceSource.clip = dancePack->clips.front();
        const auto danceMapped = Model::AnimationRigMapping::Map(danceSource, *model, rig);
        size_t danceBoneChannels = 0;
        for (size_t index = 0; index < danceSource.clip.channels.size(); ++index)
        {
            const auto& sourceChannel = danceSource.clip.channels[index];
            if (sourceChannel.targetPath.find("/Mococo_rig/") == std::string::npos) continue;
            ++danceBoneChannels;
            if (danceMapped.clip.channels[index].nodeIndex >= model->GetNodeCount())
                std::cerr << "Unresolved dance bone: " << sourceChannel.targetPath << '\n';
            Check(danceMapped.clip.channels[index].nodeIndex < model->GetNodeCount(),
                "An existing Mococo dance bone was left unassigned");
            Check(danceMapped.clip.channels[index].values == sourceChannel.values &&
                danceMapped.clip.channels[index].times == sourceChannel.times,
                "Automatic bone alias matching changed dance key data");
        }
        Check(danceBoneChannels >= 50, "Dance fixture did not exercise its original FBX bone rows");
        for (const auto& [sourceName, targetSuffix] : std::initializer_list<std::pair<const char*, const char*>>{
                {"c_arm_twist_offset.r", "/Right Upper Arm"}, {"forearm_stretch.l", "/Left Elbow"},
                {"leg_stretch.r", "/Right Knee"}, {"foot.l", "/Left Ankle"},
                {"c_ring2.r", "/Right Ring Finger 2"}, {"thumb1.l", "/Left Thumb 0"},
                {"c_thumb3.r", "/Right Thumb 2"}, {"spine_01.x", "/Upper Body"}})
            for (size_t index = 0; index < danceSource.clip.channels.size(); ++index)
                if (Model::AnimationRigMapping::Leaf(danceSource.clip.channels[index].targetPath) == sourceName)
                    Check(int(danceMapped.clip.channels[index].nodeIndex) == Find(rig, targetSuffix),
                        "Dance bone alias mapped to the wrong joint");
        std::cout << "Mapped all " << danceBoneChannels << " dance1 skeletal channels\n";
        size_t unassignedObjectScales = 0;
        for (size_t index = 0; index < danceSource.clip.channels.size(); ++index)
            if (danceSource.clip.channels[index].path == Model::AnimationChannel::Path::Scale)
            {
                ++unassignedObjectScales;
                Check(danceMapped.clip.channels[index].nodeIndex >= model->GetNodeCount(),
                    "FBX object scale was automatically assigned to a PMX bone");
            }
        Check(unassignedObjectScales > 30, "Dance fixture lost its FBX conversion-scale tracks");
        auto* scaleManager = root->GetComponent<Components::AnimationManager>();
        scaleManager->InstallEmbeddedClips({danceMapped.clip});
        scaleManager->clip = danceMapped.clip.clipName;
        scaleManager->playing = false;
        scaleManager->Start();
        for (const float time : {0.f, .5f, 2.f})
        {
            scaleManager->time = time;
            scaleManager->Tick(0.f);
            for (size_t node = 0; node < rig.bindNodes.size(); ++node)
                Check(glm::distance(model->ResolveNode(unsigned(node))->transform.scale, rig.bindNodes[node].scale) < 1e-4f,
                    "Unassigned FBX scale rows changed the target bone scale");
        }
        for (size_t node = 0; node < rig.bindNodes.size(); ++node)
        {
            auto& transform = model->ResolveNode(unsigned(node))->transform;
            transform.position = rig.bindNodes[node].position;
            transform.rotation = rig.bindNodes[node].rotation;
            transform.scale = rig.bindNodes[node].scale;
        }
        auto partial = rig;
        partial.humanoid = {};
        partial.humanoid.bones[H::LeftUpperArm] = mococo.mapping.bones[H::LeftUpperArm];
        partial.humanoid.bones[H::LeftThigh] = mococo.mapping.bones[H::LeftThigh];
        const auto mapped = Model::AnimationRigMapping::Map(walking, *model, partial);
        Check(mapped.unmatchedChannels == 0 && mapped.clip.channels.size() == 6,
            "A partial target profile prevented walking leg channels from mapping");
        for (size_t index = 0; index < 6; ++index)
            Check(int(mapped.clip.channels[index].nodeIndex) == mococo.mapping.bones[legs[index]],
                "Walking animation targeted a D helper or the wrong leg");
        auto* manager = root->GetComponent<Components::AnimationManager>();
        manager->InstallEmbeddedClips({mapped.clip}); manager->clip = mapped.clip.clipName;
        manager->playing = false; manager->time = .5f; manager->Start(); manager->Tick(0.f);
        for (auto role : legs)
            Check(glm::length(model->ResolveNode(unsigned(mococo.mapping.bones[role]))->transform.rotation) > .1f,
                "Retargeted walking animation did not move a leg joint");

        // Standard Mixamo "LeftLeg" means calf, whereas MMD "Left Leg"
        // means thigh. Confident semantic detection must precede that alias.
        walking.humanoid = {};
        for (size_t node = 0; node < sourceNodes.size(); ++node)
        {
            std::string name = H::Keys[node];
            for (const auto& [role, leaf] : std::initializer_list<std::pair<H::Role, const char*>>{
                    {H::LeftThigh, "LeftUpLeg"}, {H::LeftCalf, "LeftLeg"}, {H::LeftFoot, "LeftFoot"},
                    {H::RightThigh, "RightUpLeg"}, {H::RightCalf, "RightLeg"}, {H::RightFoot, "RightFoot"}})
                if (node == role) name = leaf;
            const int parent = sourceNodes[node].parent;
            walking.bindSkeleton[node].parent = parent >= 0 ? walking.bindSkeleton[parent].path : "";
            walking.bindSkeleton[node].path = walking.bindSkeleton[node].parent + "/mixamorig:" + name;
        }
        walking.baseSkeleton = walking.bindSkeleton;
        for (size_t index = 0; index < 6; ++index)
            walking.clip.channels[index].targetPath = walking.bindSkeleton[legs[index]].path;
        const auto mixamo = Model::AnimationRigMapping::Map(walking, *model, partial);
        Check(mixamo.unmatchedChannels == 0, "Mixamo leg aliases collided on Mococo's thighs");
        for (size_t index = 0; index < 6; ++index)
            Check(int(mixamo.clip.channels[index].nodeIndex) == mococo.mapping.bones[legs[index]],
                "A Mixamo calf was mapped onto an MMD thigh");

        const auto actualWalk = Model::AnimationClipAsset::Load((sourceRoot /
            "Engine/Core/Assets/Models/Mixemo/Walking/Walking_mixamo_com_1.anim").string());
        Check(actualWalk.has_value(), "Actual Mixamo walking clip is missing");
        for (size_t node = 0; node < rig.bindNodes.size(); ++node)
            if (auto* object = model->ResolveNode(unsigned(node)))
            {
                object->transform.position = rig.bindNodes[node].position;
                object->transform.rotation = rig.bindNodes[node].rotation;
                object->transform.scale = rig.bindNodes[node].scale;
            }
        const auto realWalk = Model::AnimationRigMapping::Map(*actualWalk, *model, rig);
        std::cout << "Walking clip: " << actualWalk->clip.clipName << ", " << realWalk.unmatchedChannels
            << " unmatched channels, " << realWalk.clip.ikSwitches.size() << " IK keys\n";
        pugi::xml_document prefab;
        Check(bool(prefab.load_file((rigPath.parent_path() / "Mococo Abyssgard.prefab").string().c_str())),
            "Could not inspect Mococo's native constraints");
        const auto native = prefab.find_node([](pugi::xml_node node)
        { return std::string(node.name()) == "components:RigConstraints"; });
        Check(bool(native), "Mococo has no native constraint fixture");
        auto* constraints = root->AddComponent<Components::RigConstraints>();
        constraints->Deserialize(Serialization::Detail::SceneXmlBehavior::ReadNode(native));
        manager->InstallEmbeddedClips({realWalk.clip}); manager->clip = realWalk.clip.clipName;
        manager->Start();
        float greatestIkCorrection = 0.f;
        float greatestFootLift = 0.f;
        for (float fraction : {0.f, .25f, .5f, .75f})
        {
            constraints->RestorePose();
            manager->time = realWalk.clip.duration * fraction;
            manager->Tick(0.f);
            const auto left = model->ResolveNode(unsigned(mococo.mapping.bones[H::LeftFoot]))->transform.GetWorldPosition();
            const auto right = model->ResolveNode(unsigned(mococo.mapping.bones[H::RightFoot]))->transform.GetWorldPosition();
            for (auto& chain : constraints->ikChains) chain.enabled = true;
            constraints->Apply();
            const auto lockedLeft = model->ResolveNode(unsigned(mococo.mapping.bones[H::LeftFoot]))->transform.GetWorldPosition();
            const auto lockedRight = model->ResolveNode(unsigned(mococo.mapping.bones[H::RightFoot]))->transform.GetWorldPosition();
            greatestIkCorrection = std::max({greatestIkCorrection,
                glm::distance(left, lockedLeft), glm::distance(right, lockedRight)});
            constraints->RestorePose();
            manager->Tick(0.f);
            for (auto& chain : constraints->ikChains) chain.enabled = false;
            constraints->Apply();
            const auto freeLeft = model->ResolveNode(unsigned(mococo.mapping.bones[H::LeftFoot]))->transform.GetWorldPosition();
            const auto freeRight = model->ResolveNode(unsigned(mococo.mapping.bones[H::RightFoot]))->transform.GetWorldPosition();
            Check(glm::distance(freeLeft, left) < .001f && glm::distance(freeRight, right) < .001f,
                "Manually disabling native IK still changed the walking foot pose");
            greatestFootLift = std::max({greatestFootLift, freeLeft.y - 1.60767f, freeRight.y - 1.60767f});
        }

        Check(greatestIkCorrection > .5f, "The fixture no longer reproduces native IK pinning the walking feet");
        Check(greatestFootLift > .5f, "Walking feet did not lift with imported IK manually disabled");
        Components::RigConstraints restoredConstraints;
        restoredConstraints.Deserialize(constraints->Serialize());
        Check(std::all_of(restoredConstraints.ikChains.begin(), restoredConstraints.ikChains.end(),
            [](const auto& chain) { return !chain.enabled; }), "Disabled IK settings did not survive serialization");
        const auto constraintPrefab = sourceRoot / ".local/test-output/mococo-walking-ik-off.prefab";
        std::filesystem::create_directories(constraintPrefab.parent_path());
        Check(Serialization::SceneSerializer::SavePrefab(*root, constraintPrefab.string()),
            "Could not save manually disabled rig IK");
        Scene::Scene reopened;
        auto* reopenedRoot = Serialization::SceneSerializer::InstantiatePrefab(reopened, constraintPrefab.string());
        auto* reopenedConstraints = reopenedRoot ? reopenedRoot->GetComponent<Components::RigConstraints>() : nullptr;
        Check(reopenedConstraints && std::all_of(reopenedConstraints->ikChains.begin(), reopenedConstraints->ikChains.end(),
            [](const auto& chain) { return !chain.enabled; }), "Prefab reload reenabled manually disabled rig IK");

        using IkOverride = Model::AnimationClip::RigIkOverride;
        for (auto& chain : constraints->ikChains) chain.enabled = true;
        const auto sampleOverride = [&](IkOverride mode)
        {
            constraints->RestorePose();
            manager->clips.front().rigIkOverride = mode;
            manager->time = realWalk.clip.duration * .25f;
            manager->Tick(0.f);
            const auto before = model->ResolveNode(unsigned(mococo.mapping.bones[H::LeftFoot]))->transform.GetWorldPosition();
            constraints->Apply();
            const auto after = model->ResolveNode(unsigned(mococo.mapping.bones[H::LeftFoot]))->transform.GetWorldPosition();
            return glm::distance(before, after);
        };
        Check(sampleOverride(IkOverride::UseModel) > .5f,
            "Use model settings did not retain Mococo's native IK");
        for (const auto& chain : constraints->ikChains)
            if (auto* controller = model->ResolveNode(chain.controllerNodeIndex))
                manager->clips.front().ikSwitches.push_back({0.f, controller->name, true});
        Check(sampleOverride(IkOverride::Disabled) < .001f,
            "The clip's FK override did not release its feet from native IK");
        Check(std::all_of(constraints->ikChains.begin(), constraints->ikChains.end(),
            [](const auto& chain) { return chain.enabled; }), "Clip override modified the model's saved IK settings");
        for (auto& key : manager->clips.front().ikSwitches) key.enabled = false;
        Check(sampleOverride(IkOverride::Enabled) > .5f,
            "Enable rig IK did not override an animation's disabled IK keys");
        manager->clips.front().ikSwitches.clear();
        Check(sampleOverride(IkOverride::UseModel) > .5f,
            "Removing the clip override did not restore the model's native IK");
        auto overrideLayer = manager->clips.front();
        overrideLayer.clipName = "IK override layer";
        overrideLayer.rigIkOverride = IkOverride::Enabled;
        manager->clips.push_back(std::move(overrideLayer));
        Model::AnimationLayer layer;
        layer.clip = "IK override layer";
        layer.time = realWalk.clip.duration * .25f;
        manager->layers.push_back(layer);
        Check(sampleOverride(IkOverride::Disabled) > .5f,
            "A later explicit animation layer did not take precedence");
        manager->layers.front().enabled = false;
        Check(sampleOverride(IkOverride::Disabled) < .001f,
            "A disabled layer still overrode the active animation's FK setting");
        auto authoredWalk = *actualWalk;
        authoredWalk.clip.rigIkOverride = IkOverride::Disabled;
        const auto overridePath = sourceRoot / ".local/test-output/walking-fk-override.anim";
        Check(authoredWalk.Save(overridePath.string()), "Could not save per-animation IK settings");
        auto loadedOverride = Model::AnimationClipAsset::Load(overridePath.string());
        Check(loadedOverride && loadedOverride->clip.rigIkOverride == IkOverride::Disabled,
            ".anim roundtrip lost the FK override");
        Check(Model::AnimationRigMapping::Map(*loadedOverride, *model, rig).clip.rigIkOverride == IkOverride::Disabled,
            "Retargeting lost the per-animation FK override");
        std::string packError;
        auto overridePack = Model::AnimationPackAsset::Capture(*manager);
        const auto overridePackPath = sourceRoot / ".local/test-output/walking-fk-override.animpack";
        if (!overridePack.Save(overridePackPath.string(), packError))
            throw std::runtime_error("Could not save IK overrides in a pack: " + packError);
        auto loadedOverridePack = Model::AnimationPackAsset::Load(overridePackPath.string(), packError);
        Check(loadedOverridePack && loadedOverridePack->clips.front().rigIkOverride == IkOverride::Disabled,
            "Animation pack roundtrip lost the FK override");
        auto legacyClip = realWalk.clip.Serialize();
        legacyClip.Erase("rigIkOverride");
        Model::AnimationClip restoredLegacy;
        restoredLegacy.Deserialize(legacyClip);
        Check(restoredLegacy.rigIkOverride == IkOverride::UseModel,
            "Legacy animations changed their default IK behavior");

        if (argc > 1 && std::string(argv[1]) == "--repair-mococo")
        {
            const auto backup = sourceRoot / ".local/backups" /
                ("mococo-humanoid-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".rig");
            std::filesystem::create_directories(backup.parent_path());
            std::filesystem::copy_file(rigPath, backup);
            for (size_t role = 0; role < H::Count; ++role)
                if (rig.humanoid.bones[role] < 0) rig.humanoid.bones[role] = mococo.mapping.bones[role];
            Check(H::Validate(rig.humanoid, mococoNodes).empty(), "Existing manual assignments need review before saving");
            Check(Model::RigAsset::SaveBundle(rigPath.string(), bundle), "Could not save Mococo humanoid mapping");
            std::cout << "Saved missing Mococo assignments. Backup: " << backup.string() << '\n';
        }
        std::cout << "Mococo detection, ambiguous helpers, manual anchors, and walking leg retargeting passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
