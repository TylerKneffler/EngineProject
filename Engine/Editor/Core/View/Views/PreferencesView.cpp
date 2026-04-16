#include "PreferencesView.h"
#include <pugixml.hpp>
#include <fstream>

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void PreferencesView::Init(const ProjectSettings& settings, const std::string& projFilePath)
{
    m_settings = settings;
    m_projFilePath = projFilePath;

    // Initialize string buffers
    strncpy_s(m_projectNameBuf, m_settings.name.c_str(), sizeof(m_projectNameBuf) - 1);
    strncpy_s(m_assetsPathBuf, m_settings.assetsDirectory.c_str(), sizeof(m_assetsPathBuf) - 1);
    strncpy_s(m_defaultSceneBuf, m_settings.defaultScene.c_str(), sizeof(m_defaultSceneBuf) - 1);
}

// ---------------------------------------------------------------------------
// DrawWindow
// ---------------------------------------------------------------------------
void PreferencesView::DrawWindow(bool& isOpen)
{
    if (!isOpen) return;

    ImGui::SetNextWindowSize(ImVec2(600, 700), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("Project Preferences", &isOpen))
    {
        if (ImGui::BeginTabBar("PreferencesTabs"))
        {
            if (ImGui::BeginTabItem("General"))
            {
                DrawMetadataSection();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Paths"))
            {
                DrawPathsSection();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Rendering"))
            {
                DrawRenderingSection();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Game"))
            {
                DrawAspectRatioSection();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// DrawMetadataSection
// ---------------------------------------------------------------------------
void PreferencesView::DrawMetadataSection()
{
    ImGui::Text("Project Information");
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputText("##projectName", m_projectNameBuf, sizeof(m_projectNameBuf)))
    {
        m_settings.name = m_projectNameBuf;
        NotifyChanged();
    }
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled("Project Name");

    ImGui::LabelText("Version", "%s", m_settings.version.c_str());
    ImGui::LabelText("Description", "%s", m_settings.description.c_str());
}

// ---------------------------------------------------------------------------
// DrawPathsSection
// ---------------------------------------------------------------------------
void PreferencesView::DrawPathsSection()
{
    ImGui::Text("Project Paths");
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputText("##assetsPath", m_assetsPathBuf, sizeof(m_assetsPathBuf)))
    {
        m_settings.assetsDirectory = m_assetsPathBuf;
        NotifyChanged();
    }
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled("Assets Directory");

    ImGui::LabelText("Scene Directory",   "%s", m_settings.sceneDirectory.c_str());
    ImGui::LabelText("Scripts Directory", "%s", m_settings.scriptsDirectory.c_str());
    ImGui::LabelText("Shaders Directory", "%s", m_settings.shadersDirectory.c_str());

    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputText("##defaultScene", m_defaultSceneBuf, sizeof(m_defaultSceneBuf)))
    {
        m_settings.defaultScene = m_defaultSceneBuf;
        NotifyChanged();
    }
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled("Default Scene");
}

// ---------------------------------------------------------------------------
// DrawRenderingSection
// ---------------------------------------------------------------------------
void PreferencesView::DrawRenderingSection()
{
    ImGui::Text("Rendering Settings");
    ImGui::Separator();

    ImGui::TextDisabled("Rendering API: %s", m_settings.renderingAPI.c_str());

    float clearColor[4] = { m_settings.clearColor.r, m_settings.clearColor.g, m_settings.clearColor.b, m_settings.clearColor.a };
    if (ImGui::ColorEdit4("Clear Color", clearColor))
    {
        m_settings.clearColor = glm::vec4(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        NotifyChanged();
    }

    if (ImGui::SliderInt("Target Framerate", (int*)&m_settings.targetFramerate, 30, 240))
        NotifyChanged();

    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputScalar("##editorViewportWidth", ImGuiDataType_U32, &m_settings.viewportWidth))
        NotifyChanged();
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled("Editor Viewport Width");

    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputScalar("##editorViewportHeight", ImGuiDataType_U32, &m_settings.viewportHeight))
        NotifyChanged();
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled("Editor Viewport Height");
}

// ---------------------------------------------------------------------------
// DrawAspectRatioSection
// ---------------------------------------------------------------------------
void PreferencesView::DrawAspectRatioSection()
{
    ImGui::Text("Game Resolution & Aspect Ratio");
    ImGui::Separator();

    // Aspect ratio mode
    const char* modes[] = { "Free", "Locked", "Hardcoded" };
    int currentMode = (int)m_settings.aspectRatioMode;
    if (ImGui::Combo("Aspect Ratio Mode", &currentMode, modes, 3))
    {
        m_settings.aspectRatioMode = (ProjectSettings::AspectRatioMode)currentMode;
        NotifyChanged();
    }

    ImGui::Separator();

    // Aspect ratio value (for Locked mode)
    if (ImGui::DragFloat("Game Aspect Ratio (W/H)", &m_settings.gameAspectRatio, 0.01f, 0.5f, 5.0f, "%.3f"))
        NotifyChanged();

    // Game window size (for Hardcoded mode)
    if (ImGui::InputScalar("Game Window Width", ImGuiDataType_U32, &m_settings.gameWindowWidth))
        NotifyChanged();
    if (ImGui::InputScalar("Game Window Height", ImGuiDataType_U32, &m_settings.gameWindowHeight))
        NotifyChanged();

    // Letterbox color
    float letterboxColor[4] = { m_settings.letterboxColor.r, m_settings.letterboxColor.g, m_settings.letterboxColor.b, m_settings.letterboxColor.a };
    if (ImGui::ColorEdit4("Letterbox Color", letterboxColor))
    {
        m_settings.letterboxColor = glm::vec4(letterboxColor[0], letterboxColor[1], letterboxColor[2], letterboxColor[3]);
        NotifyChanged();
    }
}

// ---------------------------------------------------------------------------
// SaveSettings
// ---------------------------------------------------------------------------
bool PreferencesView::SaveSettings()
{
    try
    {
        pugi::xml_document doc;
        
        // Load existing XML
        if (!doc.load_file(m_projFilePath.c_str()))
        {
            std::cerr << "Failed to load project file for saving" << std::endl;
            return false;
        }

        auto projectNode = doc.child("Project");
        if (!projectNode)
            return false;

        // Update metadata
        for (auto prop : projectNode.children("PropertyGroup"))
        {
            auto nameNode = prop.child("ProjectName");
            if (nameNode)
                nameNode.text().set(m_settings.name.c_str());

            auto assetsNode = prop.child("AssetsDirectory");
            if (assetsNode)
                assetsNode.text().set(m_settings.assetsDirectory.c_str());

            auto defaultSceneNode = prop.child("DefaultScene");
            if (defaultSceneNode)
                defaultSceneNode.text().set(m_settings.defaultScene.c_str());

            auto clearColorR = prop.child("ClearColorR");
            if (clearColorR)
                clearColorR.text().set(std::to_string(m_settings.clearColor.r).c_str());

            auto clearColorG = prop.child("ClearColorG");
            if (clearColorG)
                clearColorG.text().set(std::to_string(m_settings.clearColor.g).c_str());

            auto clearColorB = prop.child("ClearColorB");
            if (clearColorB)
                clearColorB.text().set(std::to_string(m_settings.clearColor.b).c_str());

            auto framerate = prop.child("TargetFramerate");
            if (framerate)
                framerate.text().set(std::to_string(m_settings.targetFramerate).c_str());

            auto modeNode = prop.child("AspectRatioMode");
            if (modeNode)
            {
                const char* modeStr = "";
                switch (m_settings.aspectRatioMode)
                {
                    case ProjectSettings::AspectRatioMode::Free: modeStr = "Free"; break;
                    case ProjectSettings::AspectRatioMode::Locked: modeStr = "Locked"; break;
                    case ProjectSettings::AspectRatioMode::Hardcoded: modeStr = "Hardcoded"; break;
                }
                modeNode.text().set(modeStr);
            }

            auto aspectNode = prop.child("GameAspectRatio");
            if (aspectNode)
                aspectNode.text().set(std::to_string(m_settings.gameAspectRatio).c_str());

            auto widthNode = prop.child("GameWindowWidth");
            if (widthNode)
                widthNode.text().set(std::to_string(m_settings.gameWindowWidth).c_str());

            auto heightNode = prop.child("GameWindowHeight");
            if (heightNode)
                heightNode.text().set(std::to_string(m_settings.gameWindowHeight).c_str());

            auto lbR = prop.child("LetterboxColorR");
            if (lbR)
                lbR.text().set(std::to_string(m_settings.letterboxColor.r).c_str());

            auto lbG = prop.child("LetterboxColorG");
            if (lbG)
                lbG.text().set(std::to_string(m_settings.letterboxColor.g).c_str());

            auto lbB = prop.child("LetterboxColorB");
            if (lbB)
                lbB.text().set(std::to_string(m_settings.letterboxColor.b).c_str());

            auto lbA = prop.child("LetterboxColorA");
            if (lbA)
                lbA.text().set(std::to_string(m_settings.letterboxColor.a).c_str());
        }

        // Save to file
        if (!doc.save_file(m_projFilePath.c_str()))
        {
            std::cerr << "Failed to save project file" << std::endl;
            return false;
        }

        std::cout << "Project settings saved successfully" << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error saving project settings: " << e.what() << std::endl;
        return false;
    }
}
