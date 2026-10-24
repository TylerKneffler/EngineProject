#include "Core/AnimationEditorSource.h"
#include "Core/Serialization/SceneSerializer.h"
#include <iostream>
#include <stdexcept>

using namespace Engine;
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
int main()
{
    try
    {
        Model::AnimationClipAsset asset;
        asset.clip.clipName = "Instance motion";
        asset.clip.duration = 1.f;
        Model::AnimationChannel channel;
        channel.nodeIndex = 1; channel.targetPath = "Hips/Hand";
        channel.path = Model::AnimationChannel::Path::Translation;
        channel.valueWidth = 3; channel.times = {0.f, 1.f};
        channel.values = {0.f, 1.f, 0.f, 1.f, 2.f, 3.f};
        asset.clip.channels.push_back(channel);
        Scene::Scene preview;
        Check(Serialization::SceneSerializer::LoadFromString(preview,
            Editor::AnimationEditorSource::ClipPreview(asset)), "Metadata-free clip did not open");
        auto* root = preview.GetObjects().front().get();
        auto* model = root->GetComponent<Components::Model>();
        Check(model && model->ResolveNode(1)->name == "Hand", "Preview lost channel node identity");
        Check(model->ResolveNode(1)->Parent->name == "Hips", "Preview did not construct parent paths");
        auto* manager = root->GetComponent<Components::AnimationManager>();
        Check(manager && manager->clips.size() == 1, "Preview lost animation data");
        manager->clips[0].channels[0].values[0] = 42.f;
        root->name = "Scene instance";
        root->transform.position = {2.f, 3.f, 4.f};
        root->SetPrefab("Original.prefab");
        Scene::Scene clone;
        Check(Serialization::SceneSerializer::LoadFromString(clone,
            Editor::AnimationEditorSource::Capture(*root)), "Live object did not open");
        auto* copied = clone.GetObjects().front().get();
        Check(!copied->Prefab && copied->name == root->name, "Object source reopened prefab data");
        Check(copied->transform.position == root->transform.position, "Object source lost instance placement");
        auto* copiedManager = copied->GetComponent<Components::AnimationManager>();
        Check(copiedManager->clips[0].channels[0].values[0] == 42.f, "Object source lost instance motion");
        copiedManager->clips[0].channels[0].values[0] = 13.f;
        Check(manager->clips[0].channels[0].values[0] == 42.f, "Preview modified the live object before save");
        Check(Editor::AnimationEditorSource::Rigs(copied).size() == 1, "Object source lost its skeleton");
        // A clip's saved skeleton defines the standalone preview even when
        // legacy metadata points at a prefab and a different rig ordinal.
        asset.previewPrefab = "Original.prefab";
        asset.previewRig = 7;
        asset.bindSkeleton.resize(2);
        asset.bindSkeleton[0].path = "Hips";
        asset.bindSkeleton[0].position = {0.f, 3.f, 0.f};
        asset.bindSkeleton[1].path = "Hips/Hand";
        asset.bindSkeleton[1].parent = "Hips";
        asset.bindSkeleton[1].position = {2.f, 1.f, 0.f};
        asset.baseSkeleton = asset.bindSkeleton;
        asset.baseSkeleton[1].position = {9.f, 9.f, 9.f};
        Scene::Scene standalone;
        Check(Serialization::SceneSerializer::LoadFromString(standalone,
            Editor::AnimationEditorSource::ClipPreview(asset)), "Clip with prefab metadata did not open");
        auto* standaloneRoot = standalone.GetObjects().front().get();
        auto* standaloneModel = standaloneRoot->GetComponent<Components::Model>();
        Check(!standaloneRoot->Prefab, "Standalone clip retained a prefab binding");
        Check(Editor::AnimationEditorSource::Rigs(standaloneRoot).size() == 1,
            "Preview rig hint changed standalone rig identity");
        Check(standaloneModel->ResolveNode(1)->Parent == standaloneModel->ResolveNode(0),
            "Standalone preview lost saved skeleton hierarchy");
        Check(standaloneModel->ResolveNode(1)->transform.position == asset.bindSkeleton[1].position,
            "Standalone preview replaced bind transforms with the animated base pose");
        Check(standaloneRoot->GetComponent<Components::AnimationManager>()->clips[0].channels[0].values ==
            asset.clip.channels[0].values, "Standalone preview changed the original animation channels");
        asset.bindSkeleton.resize(2);
        asset.bindSkeleton[0].path = "A"; asset.bindSkeleton[0].parent = "B";
        asset.bindSkeleton[1].path = "B"; asset.bindSkeleton[1].parent = "A";
        bool rejected = false;
        try { Editor::AnimationEditorSource::ClipPreview(asset); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "Cyclic clip hierarchy was accepted");
        Model::AnimationClipAsset empty;
        empty.clip.clipName = "New animation";
        Scene::Scene emptyPreview;
        Check(Serialization::SceneSerializer::LoadFromString(emptyPreview,
            Editor::AnimationEditorSource::ClipPreview(empty)), "Empty animation did not open");
        std::cout << "Animation editor source regressions passed\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
