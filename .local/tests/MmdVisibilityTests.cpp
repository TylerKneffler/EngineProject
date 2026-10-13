#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Scene/Scene.h"
#include <iostream>

int main()
{
    Engine::Scene::Scene scene;
    auto* root = scene.AddObject("Character");
    auto* surface = scene.AddObject("Surface");
    surface->Parent = root;
    root->Children.push_back(surface);
    auto* mesh = surface->AddComponent<Engine::Components::Mesh>();
    auto* model = root->AddComponent<Engine::Components::Model>();
    model->BindNode(0, root);
    auto* manager = root->AddComponent<Engine::Components::AnimationManager>();
    Engine::Model::AnimationClip clip;
    clip.clipName = "Dance";
    clip.duration = 2.f;
    clip.looping = false;
    clip.visibility = { { 0.f, true }, { .5f, false }, { 1.f, true } };
    Engine::Model::AnimationClip restored;
    restored.Deserialize(clip.Serialize());
    if (restored.visibility.size() != 3 || restored.visibility[1].visible)
    { std::cerr << "Visibility keys did not serialize\n"; return 1; }
    manager->clips.push_back(restored);
    Engine::Model::AnimationClip unkeyed;
    unkeyed.clipName = "Idle";
    unkeyed.duration = 2.f;
    manager->clips.push_back(unkeyed);
    manager->clip = "Dance";
    manager->playing = false;
    manager->looping = false;
    manager->Start();
    for (const auto [sample, expected] : {
        std::pair{ 0.f, true }, { .49f, true }, { .5f, false },
        { .75f, false }, { 1.f, true } })
    {
        manager->time = sample;
        manager->Tick(0.f);
        if (mesh->IsAnimationVisible() != expected || !root->enabled)
        { std::cerr << "Visibility playback failed at " << sample << '\n';
            return 1; }
    }
    manager->time = .75f;
    manager->Tick(0.f);
    manager->clip = "Idle";
    manager->Tick(0.f);
    if (!mesh->IsAnimationVisible())
    { std::cerr << "Unkeyed clip did not restore visibility\n"; return 1; }
    std::cout << "Character mesh visibility keys play and restore.\n";
    return 0;
}
