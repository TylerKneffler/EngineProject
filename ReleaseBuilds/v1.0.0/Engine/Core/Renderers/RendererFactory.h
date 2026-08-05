#pragma once
#include "IRenderer.h"
#include "IEditorRenderer.h"
#include "IGameRenderer.h"
#include <memory>
#include <string>
#include <vector>

struct ProjectSettings;

struct RendererOption
{
    std::string name;
    bool available = false;
    std::string unavailableReason;
};

// ---------------------------------------------------------------------------
// RendererFactory — Creates renderer instances based on project settings.
//
// The application specifies the desired renderer API (e.g., "DirectX12")
// in ProjectSettings::renderingAPI. The factory instantiates the
// corresponding renderer implementation, decoupling the rest of the application
// from graphics API selection.
//
// Usage:
//   ProjectSettings settings = ...;
//   auto editorRenderer = RendererFactory::CreateEditorRenderer(settings);
//   auto gameRenderer   = RendererFactory::CreateGameRenderer(settings);
//
// Supported renderers are registered in the factory; adding a new API requires
// implementing the IEditorRenderer / IGameRenderer interfaces and registering
// a creation function in the factory.
// ---------------------------------------------------------------------------
class RendererFactory
{
public:
    // Create an editor renderer based on ProjectSettings::renderingAPI.
    // Returns nullptr if the API is unsupported or initialization fails.
    static std::unique_ptr<IEditorRenderer> CreateEditorRenderer(
        const ProjectSettings& settings);

    // Create a game renderer based on ProjectSettings::renderingAPI.
    // Returns nullptr if the API is unsupported or initialization fails.
    static std::unique_ptr<IGameRenderer> CreateGameRenderer(
        const ProjectSettings& settings);

    // Get a human-readable list of supported renderer APIs.
    // Used for error messages, logging, UI dropdowns, etc.
    static std::string GetSupportedRenderers();

    // Runtime availability includes the Windows graphics runtime, a compatible
    // hardware adapter, and a driver exposing the feature level used by us.
    static const std::vector<RendererOption>& GetRendererOptions();
    static bool IsRendererAvailable(const std::string& api, std::string* reason = nullptr);

private:
    RendererFactory() = delete;  // static factory, no instances
};
