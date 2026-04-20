#include "ViewFactory.h"
#include "Core/Renderers/DX12/DX12EditorRenderer.h"
#include "Core/Renderers/DX12/D3D12View.h"
#include <cassert>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Static singleton type registry
// ---------------------------------------------------------------------------
const std::unordered_set<std::string> ViewFactory::kSingletonTypes =
{
    "Hierarchy", "Properties", "Assets", "Console"
};

// ---------------------------------------------------------------------------
// Constructor — populate the SRV free list (slot 0 is ImGui font atlas).
// ---------------------------------------------------------------------------
ViewFactory::ViewFactory(IEditorRenderer*    renderer,
                         Scene*              scene,
                         const ProjectSettings& settings)
    : m_renderer(renderer)
    , m_scene(scene)
    , m_settings(settings)
{
    // Slots 1 … MAX_SRV_SLOTS-1 are available for view textures.
    for (uint32_t i = MAX_SRV_SLOTS - 1; i >= 1; --i)
        m_freeSrvSlots.push_back(i);
}

// ---------------------------------------------------------------------------
// AllocSrvSlot — take the lowest-numbered free slot.
// ---------------------------------------------------------------------------
uint32_t ViewFactory::AllocSrvSlot()
{
    assert(!m_freeSrvSlots.empty() && "ViewFactory: SRV slot pool exhausted");
    uint32_t slot = m_freeSrvSlots.back();
    m_freeSrvSlots.pop_back();
    return slot;
}

// ---------------------------------------------------------------------------
// FreeSrvSlot — return a slot to the free list.
// ---------------------------------------------------------------------------
void ViewFactory::FreeSrvSlot(uint32_t slot)
{
    m_freeSrvSlots.push_back(slot);
}

// ---------------------------------------------------------------------------
// IsSingleton
// ---------------------------------------------------------------------------
bool ViewFactory::IsSingleton(const std::string& typeName)
{
    return kSingletonTypes.count(typeName) > 0;
}

// ---------------------------------------------------------------------------
// NotifyPanelRemoved — clear the singleton entry when a panel is destroyed.
// ---------------------------------------------------------------------------
void ViewFactory::NotifyPanelRemoved(IEditorPanel* panel)
{
    for (auto it = m_singletonInstances.begin(); it != m_singletonInstances.end(); ++it)
    {
        if (it->second == panel)
        {
            m_singletonInstances.erase(it);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Create — instantiate and initialise a panel by type name.
// ---------------------------------------------------------------------------
std::unique_ptr<IEditorPanel> ViewFactory::Create(const std::string& typeName)
{
    // Singleton check: if the panel type is restricted to one instance and one
    // already exists, focus its ImGui window and return nullptr.
    if (IsSingleton(typeName))
    {
        auto it = m_singletonInstances.find(typeName);
        if (it != m_singletonInstances.end() && it->second->IsOpen())
        {
            ImGui::SetWindowFocus(it->second->GetTitle().c_str());
            return nullptr;
        }
    }
    // For D3D12-specific views, cast to the concrete renderer implementation
    auto* dx12Renderer = dynamic_cast<DX12EditorRenderer*>(m_renderer);
    if (!dx12Renderer)
        throw std::runtime_error("ViewFactory requires a D3D12-based renderer");
    
    uint32_t w = 1280, h = 720; // default offscreen size; views resize on first DrawPanel

    if (typeName == "Scene")
    {
        if (!CanCreate3DView()) return nullptr;
        uint32_t slot = AllocSrvSlot();
        auto [cpu, gpu] = dx12Renderer->GetSrvSlot(slot);

        auto view = std::make_unique<SceneView>();
        view->SetTitle("Scene " + std::to_string(++m_sceneCount));
        view->Init(dx12Renderer, w, h, &cpu, &gpu, slot, m_scene, m_settings);
        return view;
    }

    if (typeName == "Game")
    {
        if (!CanCreate3DView()) return nullptr;
        uint32_t slot = AllocSrvSlot();
        auto [cpu, gpu] = dx12Renderer->GetSrvSlot(slot);

        auto view = std::make_unique<GameView>();
        view->SetTitle("Game " + std::to_string(++m_gameCount));
        view->Init(dx12Renderer, w, h, &cpu, &gpu, slot, m_scene, m_settings);
        return view;
    }

    if (typeName == "Hierarchy")
    {
        auto view = std::make_unique<HierarchyView>();
        view->SetTitle("Hierarchy " + std::to_string(++m_hierarchyCount));
        view->Init(m_scene);
        if (OnSelectionChanged)
            view->OnSelectionChanged = OnSelectionChanged;
        m_singletonInstances[typeName] = view.get();
        return view;
    }

    if (typeName == "Properties")
    {
        auto view = std::make_unique<PropertiesView>();
        view->SetTitle("Properties " + std::to_string(++m_propertiesCount));
        m_singletonInstances[typeName] = view.get();
        return view;
    }

    if (typeName == "Assets")
    {
        auto view = std::make_unique<AssetsExplorerView>();
        view->SetTitle("Assets " + std::to_string(++m_assetsCount));
        view->Init(m_settings.assetsDirectory);
        if (OnSceneRequested)
            view->OnSceneRequested = OnSceneRequested;
        m_singletonInstances[typeName] = view.get();
        return view;
    }

    if (typeName == "Console")
    {
        auto view = std::make_unique<ConsoleView>();
        view->SetTitle("Console " + std::to_string(++m_consoleCount));
        m_singletonInstances[typeName] = view.get();
        return view;
    }

    return nullptr;
}
