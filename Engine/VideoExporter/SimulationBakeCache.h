#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Engine::Scene { class Scene; }
namespace Engine::Core { class Object; }

namespace Engine::Video
{
// Stores render-visible simulation samples independently from camera and light
// presentation state. Dynamic scene topology is validated on every sample.
class SimulationBakeCache final
{
public:
    enum class Mode { Disabled, Recording, Replay };
    ~SimulationBakeCache();
    static uint64_t BuildCompatibilitySignature(Engine::Scene::Scene& scene);

    bool Initialize(Engine::Scene::Scene& scene,
        const std::filesystem::path& path, uint64_t compatibility,
        uint64_t expectedSamples, bool enabled);
    Mode GetMode() const { return m_mode; }
    bool IsReplaying() const { return m_mode == Mode::Replay; }
    bool ApplyNext(Engine::Scene::Scene& scene);
    bool CaptureCurrent(Engine::Scene::Scene& scene);
    bool Complete();

private:
    bool RefreshObjects(Engine::Scene::Scene& scene);
    bool IsPresentationObject(const Engine::Core::Object* object) const;

    std::filesystem::path m_path;
    std::filesystem::path m_temporaryPath;
    std::ifstream m_input;
    std::ofstream m_output;
    std::vector<Engine::Core::Object*> m_objects;
    Mode m_mode = Mode::Disabled;
    uint64_t m_compatibility = 0;
    uint64_t m_schema = 0;
    uint64_t m_expectedSamples = 0;
    uint64_t m_sampleCount = 0;
    bool m_hasCloth = false;
};
}
