#pragma once
#include "Core/component.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include <string>
#include <vector>

// Generates derived geometry from a SplinePath on this object or another object.
// Authored settings are saved; the runtime-only mesh child is rebuilt on load.
class SplineGeometry final : public Engine::Core::Component
{
public:
    enum class Mode { Road, Pipe, RepeatMesh, BendMesh };
    SplineGeometry();
    bool RequiresUpdate() const override { return false; }
    std::string pathObjectName;
    int mode = int(Mode::Road);
    std::string meshAsset;
    int forwardAxis = 2; // X=0, Y=1, Z=2; remaining axes map to right/up cyclically.
    float spacing = 2.f;
    bool fitToLength = true;
    float segmentLength = .5f;
    float width = 4.f;
    float radius = .25f;
    int radialSegments = 12;
    bool capEnds = true;
    float uvMetersPerRepeat = 2.f;
    glm::vec3 meshScale{1.f};
    glm::vec3 offset{0.f}; // right, up, forward in the moving spline frame
    glm::vec3 up{0.f,1.f,0.f};

    // CPU-only builder, also usable by tools/exporters and headless tests.
    bool BuildGeometry(std::vector<Engine::Components::Mesh::Vertex>& vertices,
        std::string& error) const;
    void PrepareGeometry(IGraphicsProvider* provider);
    void OnDestroy() override;
    bool DrawProperties(Engine::Editor::IEditorUi& ui) override;
    const std::string& GetBuildMessage() const { return m_message; }
private:
    uint64_t m_signature = 0;
    uint64_t m_outputId = 0;
    IGraphicsProvider* m_provider = nullptr;
    std::string m_message;
};
