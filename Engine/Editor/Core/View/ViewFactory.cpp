#include "ViewFactory.h"
#include <stdexcept>

// ---------------------------------------------------------------------------
// Static singleton type registry
// ---------------------------------------------------------------------------
const std::unordered_set<std::string> ViewFactory::kSingletonTypes =
{
    "Hierarchy", "Properties", "Assets", "Console"
};

// ---------------------------------------------------------------------------
// Constructor.
// ---------------------------------------------------------------------------
ViewFactory::ViewFactory(IEditorRenderer*    renderer,
                         Scene*              scene,
                         const ProjectSettings& settings)
    : m_renderer(renderer)
    , m_scene(scene)
    , m_settings(settings)
{
    OutputDebugStringA("[ViewFactory] Constructor called\n");
    OutputDebugStringA("[ViewFactory] Constructor completed\n");
}

// ---------------------------------------------------------------------------
// FreeSrvSlot — return a slot to the free list.
// ---------------------------------------------------------------------------
void ViewFactory::FreeSrvSlot(uint32_t slot)
{
    if (m_renderer)
        m_renderer->FreeSrvSlot(slot);
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
    // already exists, keep that singleton and return nullptr.
    if (IsSingleton(typeName))
    {
        auto it = m_singletonInstances.find(typeName);
        if (it != m_singletonInstances.end() && it->second->IsOpen())
        {
            return nullptr;
        }
    }
    if (!m_renderer)
        throw std::runtime_error("ViewFactory requires a valid renderer");

    void* deviceHandle = m_renderer->GetNativeDeviceHandle();
    if (!deviceHandle)
        throw std::runtime_error("ViewFactory failed to get native device handle from renderer");
    
    uint32_t w = 1280, h = 720; // default offscreen size; views resize on first DrawPanel

    if (typeName == "Scene")
    {
        if (!CanCreate3DView()) return nullptr;
        auto [handles, slot] = m_renderer->AllocateSrvSlot();
        void* cpu = handles.first;
        void* gpu = handles.second;

        auto view = std::make_unique<SceneView>();
        view->SetViewBackend(m_renderer->CreateViewBackend());
        view->SetTitle("Scene " + std::to_string(++m_sceneCount));
        view->Init(deviceHandle, w, h, cpu, gpu, slot, m_scene, m_settings);
        return view;
    }

    if (typeName == "Game")
    {
        if (!CanCreate3DView()) return nullptr;
        auto [handles, slot] = m_renderer->AllocateSrvSlot();
        void* cpu = handles.first;
        void* gpu = handles.second;

        auto view = std::make_unique<GameView>();
        view->SetViewBackend(m_renderer->CreateViewBackend());
        view->SetTitle("Game " + std::to_string(++m_gameCount));
        view->Init(deviceHandle, w, h, cpu, gpu, slot, m_scene, m_settings);
        return view;
    }

    if (typeName == "Hierarchy")
    {
        auto view = std::make_unique<HierarchyView>();
        view->SetTitle("Hierarchy " + std::to_string(++m_hierarchyCount));
        view->Init(m_scene);
        if (OnSelectionChanged)
            view->OnSelectionChanged = OnSelectionChanged;
        if (OnFocusObject)
            view->OnFocusObject = OnFocusObject;
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
