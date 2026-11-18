#pragma once
#include "Core/Rendering/SceneRenderer.h"

#include "Core/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

namespace Engine::Rendering
{
class SceneRendererObjectRecordUpload
{
public:
    using Scene = Engine::Scene::Scene;
    SceneRendererObjectRecordUpload(
        SceneRenderer& renderer,
        std::vector<PreparedDraw>& draws,
        bool shadowOnly);
    ~SceneRendererObjectRecordUpload();

    SceneRendererObjectRecordUpload(const SceneRendererObjectRecordUpload&) = delete;
    SceneRendererObjectRecordUpload& operator=(const SceneRendererObjectRecordUpload&) = delete;

    void EnsureUploaded();

private:
    struct BufferedRecords;

    SceneRenderer& m_renderer;
    bool m_uploaded = false;
    uint32_t m_firstDirtyObject = UINT32_MAX;
    uint32_t m_lastDirtyObject = 0;
    BufferedRecords& m_recordSnapshot;
};
}
