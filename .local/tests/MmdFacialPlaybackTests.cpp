#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Scene/Scene.h"
#include <cmath>
#include <iostream>

int main()
{
    Engine::Scene::Scene scene;
    auto* root = scene.AddObject("Character");
    auto* face = scene.AddObject("Face");
    face->Parent = root;
    root->Children.push_back(face);
    auto* first = scene.AddObject("Body_");
    auto* second = scene.AddObject("Body_ 2");
    for (auto* object : { first, second })
    {
        object->Parent = face;
        face->Children.push_back(object);
    }
    auto* firstMesh = first->AddComponent<Engine::Components::Mesh>();
    auto* secondMesh = second->AddComponent<Engine::Components::Mesh>();
    std::vector<Engine::Model::MorphTarget> firstTargets(2), secondTargets(3);
    firstTargets[0].name = "Blink";
    firstTargets[1].name = "Smile";
    secondTargets[0].name = "Blink.Blink";
    secondTargets[1].name = "Smile.Smile";
    secondTargets[2].name = "Blush.Blush";
    firstMesh->SetMorphData(0, firstTargets, { 0.f, 0.f });
    secondMesh->SetMorphData(0, secondTargets, { 0.f, 0.f, 0.f });
    Engine::Components::Mesh restored;
    const auto serializedMesh = secondMesh->Serialize();
    restored.Deserialize(serializedMesh);
    if (restored.GetMorphTargets().size() != 3 ||
        restored.GetMorphTargets()[1].name != "Smile.Smile")
    { std::cerr << "Morph target name was lost on reload\n"; return 1; }

    auto* model = root->AddComponent<Engine::Components::Model>();
    model->BindNode(0, face);
    auto* manager = root->AddComponent<Engine::Components::AnimationManager>();
    Engine::Model::AnimationClip body;
    body.clipName = "Body";
    body.duration = 1.f;
    body.looping = false;
    Engine::Model::AnimationChannel travel;
    travel.nodeIndex = 0;
    travel.targetPath = "Face";
    travel.path = Engine::Model::AnimationChannel::Path::Translation;
    travel.valueWidth = 3;
    travel.times = { 0.f, 1.f };
    travel.values = { 0.f, 0.f, 0.f, 2.f, 0.f, 0.f };
    body.channels.push_back(travel);
    Engine::Model::AnimationClip facial;
    facial.clipName = "Facial";
    facial.duration = 1.f;
    facial.looping = false;
    Engine::Model::AnimationChannel weights;
    weights.nodeIndex = 0;
    weights.targetPath = "Face";
    weights.targetMesh = "Face/Body_";
    weights.path = Engine::Model::AnimationChannel::Path::Weights;
    weights.valueWidth = 2;
    weights.times = { 0.f, 1.f };
    weights.values = { 0.f, 0.f, 1.f, .5f };
    facial.channels.push_back(weights);
    weights.targetMesh = "Face/Body_ 2";
    weights.valueWidth = 3;
    weights.values = { 0.f, 0.f, 0.f, .25f, .75f, 1.f };
    facial.channels.push_back(weights);
    Engine::Model::AnimationClip reloadedFacial;
    reloadedFacial.Deserialize(facial.Serialize());
    if (reloadedFacial.channels.size() != 2 ||
        reloadedFacial.channels[1].targetMesh != "Face/Body_ 2")
    { std::cerr << "Facial mesh path was lost on reload\n"; return 1; }
    manager->clips = { body, reloadedFacial };
    manager->clip = "Body";
    manager->looping = false;
    manager->playing = false;
    Engine::Model::AnimationLayer layer;
    layer.clip = "Facial";
    layer.looping = false;
    manager->layers.push_back(layer);
    manager->Start();
    manager->time = .5f;
    manager->layers[0].time = .5f;
    manager->Tick(0.f);
    const auto& a = firstMesh->GetMorphWeights();
    const auto& b = secondMesh->GetMorphWeights();
    if (std::abs(face->transform.position.x - 1.f) > 1e-4f ||
        a.size() != 2 || b.size() != 3 ||
        std::abs(a[0] - .5f) > 1e-4f ||
        std::abs(a[1] - .25f) > 1e-4f ||
        std::abs(b[0] - .125f) > 1e-4f ||
        std::abs(b[1] - .375f) > 1e-4f ||
        std::abs(b[2] - .5f) > 1e-4f)
    { std::cerr << "Independent body/facial mesh playback failed\n"; return 1; }
    manager->layers[0].enabled = false;
    manager->Tick(0.f);
    if (firstMesh->GetMorphWeights()[0] != 0.f ||
        secondMesh->GetMorphWeights()[2] != 0.f)
    { std::cerr << "Disabling facial layer did not restore defaults\n";
        return 1; }
    std::cout << "Independent facial meshes, reload and layer playback passed.\n";
    return 0;
}
