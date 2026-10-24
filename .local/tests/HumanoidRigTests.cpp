#include "Core/Model/HumanoidRig.h"
#include "Core/Model/AnimationRigMapping.h"
#include "Core/Model/RigAsset.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Model/AnimationPackAsset.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <chrono>

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)
namespace H = Engine::Model::HumanoidRig;
using namespace Engine;

std::vector<H::Node> Avatar(bool aPose = true)
{
    std::vector<H::Node> nodes(H::Count);
    for (auto& node : nodes) node.joint=false;
    const auto set = [&](H::Role role, int parent, glm::vec3 position) {nodes[role]={parent,position,true};};
    set(H::Root,-1,{0,0,0}); set(H::Pelvis,H::Root,{0,1,0});
    set(H::Spine,H::Pelvis,{0,1.2f,0}); set(H::Chest,H::Spine,{0,1.5f,0});
    set(H::Neck,H::Chest,{0,1.75f,0}); set(H::Head,H::Neck,{0,1.95f,0});
    for (bool left : {true,false})
    {
        const float x=left?1.f:-1.f;
        const size_t arm=left?H::LeftShoulder:H::RightShoulder;
        set(H::Role(arm),H::Chest,{x*.08f,1.5f,0});
        set(H::Role(arm+1),int(arm),{x*.2f,1.55f,0});
        set(H::Role(arm+2),int(arm+1),{x*.5f,aPose?1.3f:1.55f,0});
        set(H::Role(arm+3),int(arm+2),{x*.8f,aPose?1.05f:1.55f,0});
        const size_t leg=left?H::LeftThigh:H::RightThigh;
        set(H::Role(leg),H::Pelvis,{x*.12f,.95f,0});
        set(H::Role(leg+1),int(leg),{x*.12f,.5f,.01f});
        set(H::Role(leg+2),int(leg+1),{x*.12f,.08f,0});
        set(H::Role(leg+3),int(leg+2),{x*.12f,.03f,.15f});
    }
    return nodes;
}

std::vector<Model::AnimationClipAsset::BaseNode> Bind(const std::vector<H::Node>& input,
    const std::string& prefix, float scale=1.f)
{
    std::vector<Model::AnimationClipAsset::BaseNode> result(input.size());
    for (size_t index=0;index<input.size();++index)
    {
        const int parent=input[index].parent;
        result[index].path=parent<0?prefix+std::to_string(index):result[parent].path+"/"+prefix+std::to_string(index);
        result[index].parent=parent<0?"":result[parent].path;
        result[index].position=(input[index].position-(parent<0?glm::vec3(0):input[parent].position))*scale;
    }
    return result;
}

int main(int argc, char** argv)
{
    try
    {
        if (argc>1 && std::string(argv[1])=="--inspect")
        {
            for (const auto* path : {"Engine/Core/Assets/Roku_V1_packed/Roku_V1_packed.rig",
                    "Engine/Core/Assets/Models/mmd/Mococo/Mococo Abyssgard/Mococo Abyssgard.rig"})
                for (const auto& rig : Model::RigAsset::LoadAll(path))
                {
                    std::vector<unsigned> joints;
                    for (const auto& joint : rig.joints) joints.push_back(joint.nodeIndex);
                    const auto detected=H::Detect(H::Nodes(rig.bindNodes,joints));
                    std::cout<<path<<" skin "<<rig.skinIndex<<" joints "<<joints.size()<<'\n';
                    for (size_t role=0;role<H::Count;++role)
                        if (const int node=detected.mapping.bones[role];node>=0)
                            std::cout<<"  "<<H::Labels[role]<<" ["<<node<<"] "<<rig.bindNodes[node].path<<'\n';
                    std::cout<<"  Unresolved/validation notices: "<<detected.issues.size()<<'\n';
                }
            return 0;
        }
        auto avatar=Avatar();
        const auto detected=H::Detect(avatar);
        for (auto role : {H::Pelvis,H::Spine,H::Chest,H::Neck,H::Head,H::LeftUpperArm,H::LeftLowerArm,H::LeftHand,
                H::RightUpperArm,H::RightLowerArm,H::RightHand,H::LeftThigh,H::LeftCalf,H::LeftFoot,
                H::RightThigh,H::RightCalf,H::RightFoot})
        {
            if (detected.mapping.bones[role]!=int(role))
                throw std::runtime_error(std::string("Incorrect detection for ")+H::Labels[role]+": "+
                    std::to_string(detected.mapping.bones[role]));
        }
        CHECK(H::Validate(detected.mapping,avatar).empty());
        const auto tPose=H::Detect(Avatar(false));
        CHECK(tPose.mapping.bones[H::LeftHand]==H::LeftHand && tPose.mapping.bones[H::RightCalf]==H::RightCalf);
        auto rotated=avatar;
        for (auto& node : rotated) {std::swap(node.position.y,node.position.z);node.position*=42.f;}
        H::Mapping axes; axes.upAxis=2;axes.forwardAxis=1;
        const auto zUp=H::Detect(rotated,axes);
        CHECK(zUp.mapping.bones[H::LeftHand]==H::LeftHand && zUp.mapping.bones[H::RightFoot]==H::RightFoot);
        axes=H::Mapping();axes.leftPositive=false;
        CHECK(H::Detect(avatar,axes).mapping.bones[H::LeftHand]==H::RightHand);
        auto cyclic=avatar;cyclic[H::Root].parent=H::Head;
        CHECK(!H::Detect(cyclic).issues.empty());
        auto invalid=detected.mapping;invalid.bones[H::LeftHand]=H::RightFoot;
        CHECK(!H::Validate(invalid,avatar).empty());
        auto ambiguous=avatar;
        const int branch=int(ambiguous.size());
        for (int i=0;i<3;++i)
        {
            auto node=avatar[H::LeftUpperArm+i];
            if (i>0) node.parent=branch+i-1;
            ambiguous.push_back(node);
        }
        CHECK(H::Detect(ambiguous).mapping.bones[H::LeftHand]<0);
        auto helperParent=avatar;
        auto colocatedParent=helperParent[H::LeftUpperArm];
        helperParent[H::LeftUpperArm].parent=int(helperParent.size());
        helperParent.push_back(colocatedParent);
        CHECK(H::Detect(helperParent).mapping.bones[H::LeftUpperArm]==H::LeftUpperArm);
        auto controlHierarchy=avatar;
        controlHierarchy[H::LeftUpperArm].parent=H::Pelvis;
        controlHierarchy[H::RightUpperArm].parent=H::Pelvis;
        CHECK(H::Detect(controlHierarchy).mapping.bones[H::Chest]!=H::Pelvis);
        H::Mapping scope;scope.bones[H::Root]=H::LeftUpperArm;
        CHECK(H::Detect(avatar,scope).mapping.bones[H::RightHand]<0);

        // Neither skeleton uses recognizable names. The target's model-node
        // indices also differ from the source, so copying indices cannot pass.
        Model::AnimationClipAsset source;
        source.bindSkeleton=Bind(avatar,"opaque_");source.baseSkeleton=source.bindSkeleton;
        source.previewPrefab="source.prefab";
        Model::AnimationChannel channel;
        channel.nodeIndex=H::LeftUpperArm;channel.targetPath=source.bindSkeleton[H::LeftUpperArm].path;
        channel.path=Model::AnimationChannel::Path::Rotation;channel.valueWidth=4;
        channel.times={0,1};channel.values={0,0,0,1,0,0,.70710678f,.70710678f};
        source.clip.clipName="Unknown bone wave";source.clip.duration=1;source.clip.channels.push_back(channel);
        Scene::Scene scene;
        auto* root=scene.AddObject("Target avatar");auto* model=root->AddComponent<Components::Model>();
        std::vector<Core::Object*> objects(avatar.size());
        const auto targetBind=Bind(avatar,"unrelated_",1.3f);
        Model::RigAsset rig;rig.prefabPath="target.prefab";
        for (size_t i=0;i<avatar.size();++i)
        {
            auto* object=scene.AddObject("unrelated_"+std::to_string(i));
            object->Parent=avatar[i].parent<0?root:objects[avatar[i].parent];
            object->Parent->Children.push_back(object);object->transform.position=targetBind[i].position;
            objects[i]=object;model->BindNode(unsigned(i*2),object);
            auto* helper=scene.AddObject("inert_"+std::to_string(i));helper->Parent=root;root->Children.push_back(helper);
            model->BindNode(unsigned(i*2+1),helper);
            rig.joints.push_back({unsigned(i*2),object->name,avatar[i].parent<0?-1:avatar[i].parent*2,glm::mat4(1)});
        }
        rig.bindNodes=Model::AnimationClipAsset::CaptureBaseSkeleton(*model);
        const auto inferred=Model::AnimationRigMapping::Map(source,*model,rig);
        CHECK(inferred.unmatchedChannels==0);
        CHECK(inferred.clip.channels[0].nodeIndex==H::LeftUpperArm*2);
        CHECK(inferred.inferredChannels==1);
        source.humanoid=detected.mapping;
        rig.humanoid=detected.mapping;
        for (auto& bone : rig.humanoid.bones) if (bone>=0) bone*=2;
        // An explicit map must override a coincidentally matching display path.
        rig.humanoid.bones[H::LeftUpperArm]=H::RightUpperArm*2;
        const auto manual=Model::AnimationRigMapping::Map(source,*model,rig);
        CHECK(manual.clip.channels[0].nodeIndex==H::RightUpperArm*2);
        rig.humanoid.bones[H::LeftUpperArm]=H::LeftUpperArm*2;
        objects[H::LeftUpperArm]->transform.rotation.z=.3f;
        const auto rotatedTarget=Model::AnimationRigMapping::Map(source,*model,rig);
        const auto& values=rotatedTarget.clip.channels[0].values;
        CHECK(std::abs(values[2]-std::sin(.15f))<1e-4f);
        CHECK(std::abs(values[3]-std::cos(.15f))<1e-4f);
        objects[H::LeftUpperArm]->transform.rotation.z=0.f;
        const auto output=std::filesystem::path(".local/test-output")/
            ("humanoid-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(output);
        CHECK(rig.Save((output/"avatar.rig").string()));
        auto loaded=Model::RigAsset::Load((output/"avatar.rig").string());
        CHECK(loaded && loaded->humanoid.bones==rig.humanoid.bones);
        CHECK(source.Save((output/"wave.anim").string()));
        auto animation=Model::AnimationClipAsset::Load((output/"wave.anim").string());
        CHECK(animation && animation->humanoid.bones==source.humanoid.bones);
        auto* accessory=root->AddComponent<Components::Skeleton>();
        accessory->skinIndex=0;accessory->jointNodes={H::LeftHand*2};
        auto* body=root->AddComponent<Components::Skeleton>();
        body->skinIndex=1;
        for (const auto& joint : rig.joints) body->jointNodes.push_back(joint.nodeIndex);
        rig.skinIndex=1;
        CHECK(rig.Save((output/"avatar.rig").string()));
        body->rigPath=(output/"avatar.rig").string();
        CHECK(Model::RigAsset::ForModel(*model).skinIndex==1);
        CHECK(Model::RigAsset::ForModel(*model).humanoid.bones==rig.humanoid.bones);
        Model::AnimationPackAsset pack;
        pack.activeClip=source.clip.clipName;pack.clips={source.clip};
        pack.bindSkeleton=source.bindSkeleton;pack.humanoid=source.humanoid;pack.sourcePrefab=source.previewPrefab;
        Model::AnimationLayer layer;layer.clip=source.clip.clipName;layer.nodeMask={H::LeftUpperArm};
        pack.layers={layer};pack.nodeMaskPaths={{source.bindSkeleton[H::LeftUpperArm].path}};
        std::string error;
        CHECK(pack.Save((output/"wave.animpack").string(),error));
        auto loadedPack=Model::AnimationPackAsset::Load((output/"wave.animpack").string(),error);
        CHECK(loadedPack && loadedPack->humanoid.bones==source.humanoid.bones);
        auto* manager=root->AddComponent<Components::AnimationManager>();
        CHECK(loadedPack->Apply(*manager,error));
        CHECK(manager->clips[0].channels[0].nodeIndex==H::LeftUpperArm*2);
        CHECK(manager->layers[0].nodeMask[0]==H::LeftUpperArm*2);
        const auto captured=Model::AnimationPackAsset::Capture(*manager);
        CHECK(captured.humanoid.bones==rig.humanoid.bones);
        std::cout<<"Nameless A/T poses, axes, ambiguity, cycles, semantic retargeting and profile round trips passed\n";
        return 0;
    }
    catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
