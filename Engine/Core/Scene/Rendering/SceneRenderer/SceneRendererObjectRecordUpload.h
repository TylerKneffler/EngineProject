#pragma once

#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

namespace Engine::Scene::Renderer
{
class SceneRendererObjectRecordUpload
{
public:
    SceneRendererObjectRecordUpload(
        Scene& scene,
        std::vector<PreparedDraw>& draws,
        bool shadowOnly);
    ~SceneRendererObjectRecordUpload();

    SceneRendererObjectRecordUpload(const SceneRendererObjectRecordUpload&) = delete;
    SceneRendererObjectRecordUpload& operator=(const SceneRendererObjectRecordUpload&) = delete;

    void EnsureUploaded();

private:
    struct BufferedRecords;

    Scene& m_scene;
    bool m_uploaded = false;
    uint32_t m_firstDirtyObject = UINT32_MAX;
    uint32_t m_lastDirtyObject = 0;
    BufferedRecords& m_recordSnapshot;
};
}
