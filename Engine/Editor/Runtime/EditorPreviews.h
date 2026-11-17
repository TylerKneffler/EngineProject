#pragma once

namespace Engine::Scene { class Scene; }

namespace Engine::Editor
{
// Advances explicit edit-mode previews (particle emitters and animations with
// a running editor preview, editor spatial scripts) without running gameplay
// callbacks. Uses the same owner-thread job phases as Game::Update.
// Returns true when anything advanced.
bool AdvanceEditorPreviews(Engine::Scene::Scene& scene, float deltaTime);
}
