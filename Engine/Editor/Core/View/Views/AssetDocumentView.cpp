#include "AssetDocumentView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Engine/Editor/Core/Paint/ImagePaint.h"
#include "Engine/Editor/Core/Paint/BrushPreview.h"
#include "Engine/Editor/Core/Paint/PaintInput.h"
#include "Engine/Editor/Core/Paint/PngPaintWriter.h"
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <commdlg.h>

namespace Engine::Editor
{
AssetDocumentView::AssetDocumentView(const std::string& path,
    const std::string& editorType, Engine::Scene::Scene* previewScene)
    : m_path([&path]()
        {
            std::error_code error;
            const std::filesystem::path canonical =
                std::filesystem::weakly_canonical(path, error);
            return (error ? std::filesystem::absolute(path).lexically_normal()
                : canonical).string();
        }())
    , m_identity(m_path)
    , m_editorType(editorType)
    , m_previewScene(previewScene)
{
#ifdef _WIN32
    std::transform(m_identity.begin(), m_identity.end(), m_identity.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
#endif
    m_inspector.Select(m_path);
    m_knownContents = ReadContents();
    SetDefaultDockArea(EditorPanelDockArea::MainDocument);
    RefreshTitle();
    m_inspector.OnContentsChanged = [this](const std::string&)
    {
        m_assetWriteObserved = true;
    };
    m_inspector.OnRenamed = [this](const std::string& oldPath,
        const std::string& newPath)
    {
        std::error_code error;
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(newPath, error);
        m_path = (error ? std::filesystem::absolute(newPath).lexically_normal()
            : canonical).string();
        m_identity = m_path;
#ifdef _WIN32
        std::transform(m_identity.begin(), m_identity.end(), m_identity.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
#endif
        m_undo.clear();
        m_redo.clear();
        m_imagePixels.clear();
        m_imageTexture.reset();
        m_imageStrokeActive = false;
        m_imageStrokeChanged = false;
        m_knownContents = ReadContents();
        RefreshTitle();
        if (OnRenamed)
            OnRenamed(oldPath, m_path);
    };
}

void AssetDocumentView::RefreshTitle()
{
    const std::string filename = std::filesystem::path(m_path).filename().string();
    // ImGui's ### keeps a path-specific ID while displaying only the filename.
    SetTitle(filename + (m_dirty ? " *" : "") + "###AssetDocument:" + m_identity);
}

std::string AssetDocumentView::ReadContents() const
{
    std::error_code error;
    const uintmax_t fileSize = std::filesystem::file_size(m_path, error);
    if (error || fileSize > SnapshotSizeLimit)
        return {};
    std::ifstream file(m_path, std::ios::binary);
    if (!file)
        return {};
    return std::string(std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

void AssetDocumentView::DrawPanel(IEditorUi& ui)
{
    if (!ui.BeginWindow(m_title.c_str(), &m_open))
    {
        ui.EndWindow();
        return;
    }
    ui.WindowTitleTooltip(m_path.c_str());
    if (ui.IsWindowFocused() && OnFocused)
        OnFocused();

    ui.ColoredLabel(m_editorType.c_str(), { 0.35f, 0.7f, 1.f, 1.f });
    ui.ValueLabel("File", std::filesystem::path(m_path).filename().string().c_str());
    if (ui.IsItemHovered())
        ui.Tooltip(m_path.c_str());
    if (CanPaintImage()) DrawImageCanvas(ui);
    m_inspector.Draw(ui, m_previewScene);
    if (m_assetWriteObserved)
    {
        m_assetWriteObserved = false;
        const std::string current = ReadContents();
        if (current != m_knownContents)
        {
            if (m_undo.size() == HistoryLimit)
                m_undo.pop_front();
            m_undo.push_back(m_knownContents);
            m_redo.clear();
            m_knownContents = current;
            m_dirty = false; // Inspector-backed asset editors save on edit.
            RefreshTitle();
            if (OnChanged)
                OnChanged();
        }
    }
    ui.EndWindow();
}

bool AssetDocumentView::RestoreSnapshot(const std::string& contents)
{
    std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file)
        return false;
    m_inspector.Select(m_path);
    if (CanPaintImage())
    {
        Engine::Components::Texture::Invalidate(m_path);
        m_imagePixels.clear();
        m_imageTexture.reset();
        LoadImageCanvas();
    }
    m_knownContents = contents;
    m_dirty = false;
    RefreshTitle();
    if (OnChanged)
        OnChanged();
    return true;
}

bool AssetDocumentView::Undo()
{
    if (m_imageStrokeActive && !CommitImageStroke()) return false;
    if (m_undo.empty())
        return false;
    const std::string current = ReadContents();
    const std::string target = std::move(m_undo.back());
    m_undo.pop_back();
    m_redo.push_back(current);
    if (!RestoreSnapshot(target))
    {
        m_undo.push_back(target);
        m_redo.pop_back();
        return false;
    }
    return true;
}

bool AssetDocumentView::Redo()
{
    if (m_imageStrokeActive && !CommitImageStroke()) return false;
    if (m_redo.empty())
        return false;
    const std::string current = ReadContents();
    const std::string target = std::move(m_redo.back());
    m_redo.pop_back();
    if (m_undo.size() == HistoryLimit)
        m_undo.pop_front();
    m_undo.push_back(current);
    if (!RestoreSnapshot(target))
    {
        m_redo.push_back(target);
        m_undo.pop_back();
        return false;
    }
    return true;
}

bool AssetDocumentView::Save()
{
    if (m_imageStrokeActive && !CommitImageStroke()) return false;
    const std::string current = ReadContents();
    if (current.empty() && !std::filesystem::exists(m_path))
        return false;
    m_knownContents = current;
    m_dirty = false;
    RefreshTitle();
    return true;
}

bool AssetDocumentView::CanPaintImage() const
{
    std::string extension = std::filesystem::path(m_path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return m_editorType == "Texture Editor" && extension == ".png";
}

bool AssetDocumentView::LoadImageCanvas()
{
    if (!m_imagePixels.empty()) return true;
    auto image = Engine::Components::Texture::Acquire(m_path, false);
    if (!image || !image->Load() || !image->GetWidth() || !image->GetHeight())
    { m_imageError = "Could not decode PNG for painting."; return false; }
    m_imageWidth = image->GetWidth();
    m_imageHeight = image->GetHeight();
    const size_t bytes = static_cast<size_t>(m_imageWidth) * m_imageHeight * 4;
    if (image->GetPixels().size() < bytes)
    { m_imageError = "PNG pixel data is incomplete."; return false; }
    m_imagePixels.assign(image->GetPixels().begin(),
        image->GetPixels().begin() + bytes);
    m_imageTextureDirty = true;
    m_imageError.clear();
    return true;
}

bool AssetDocumentView::CommitImageStroke()
{
    if (!m_imageStrokeActive) return true;
    if (!m_imageStrokeChanged)
    { m_imageStrokeActive = false; return true; }
    if (!Paint::SavePng(m_path, m_imageWidth, m_imageHeight,
        m_imagePixels, m_imageError)) return false;
    if (m_undo.size() == HistoryLimit) m_undo.pop_front();
    m_undo.push_back(m_knownContents);
    m_redo.clear();
    m_knownContents = ReadContents();
    m_imageStrokeActive = false;
    m_imageStrokeChanged = false;
    m_dirty = false;
    RefreshTitle();
    Engine::Components::Texture::Invalidate(m_path);
    if (OnChanged) OnChanged();
    return true;
}

void AssetDocumentView::DrawImageCanvas(IEditorUi& ui)
{
    if (!LoadImageCanvas())
    { ui.ColoredLabel(m_imageError.c_str(), {1.f, .4f, .3f, 1.f}); return; }
    auto* graphics = m_previewScene ? m_previewScene->GetGraphicsProvider() : nullptr;
    auto* factory = graphics ? graphics->GetTextureFactory() : nullptr;
    if (m_imageTextureDirty && factory)
    {
        m_imageTexture = factory->CreateTexture2D(m_imageWidth, m_imageHeight,
            m_imagePixels.data(), 1,
            Engine::Graphics::GraphicsTextureFormat::Rgba8, true);
        m_imageTextureDirty = !m_imageTexture;
    }
    if (!m_imageTexture)
    { ui.DisabledLabel("Image canvas requires an active graphics provider."); return; }
    const float availableWidth = std::max(1.f, ImGui::GetContentRegionAvail().x);
    const float availableHeight = std::max(100.f, ImGui::GetContentRegionAvail().y - 120.f);
    const float scale = std::min({ availableWidth / m_imageWidth,
        availableHeight / m_imageHeight, 4.f });
    const ImVec2 size(m_imageWidth * scale, m_imageHeight * scale);
    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(
        m_imageTexture->GetNativeHandle())), size);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 cursor = ImGui::GetIO().MousePos;
    const float x = (cursor.x - minimum.x) / scale;
    const float y = (cursor.y - minimum.y) / scale;
    if (hovered && m_imageTool != 0)
        ImGui::GetWindowDrawList()->AddCircle(cursor,
            m_imageTool == 3 ? 4.f : m_imageBrush.size * .5f * scale *
                Paint::PressureScale(m_imageBrush.pressureSize),
            IM_COL32(255, 220, 60, 230), 0, 1.5f);
    if (m_imageStrokeActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        CommitImageStroke();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        m_imageTool != 0)
    {
        m_imageStrokeActive = true;
        m_imageStrokeChanged = false;
        m_lastImageX = -1.f;
        m_lastImageY = -1.f;
        if (m_imageTool == 5)
        {
            const uint32_t sampleX = std::min(m_imageWidth - 1,
                static_cast<uint32_t>(std::max(0.f, x)));
            const uint32_t sampleY = std::min(m_imageHeight - 1,
                static_cast<uint32_t>(std::max(0.f, y)));
            const size_t offset =
                (static_cast<size_t>(sampleY) * m_imageWidth + sampleX) * 4;
            std::copy_n(m_imagePixels.data() + offset, 4, m_smearColor);
        }
    }
    if (m_imageStrokeActive && hovered &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const uint8_t color[4]{
            static_cast<uint8_t>(std::lround(std::clamp(m_imageColor[0], 0.f, 1.f) * 255.f)),
            static_cast<uint8_t>(std::lround(std::clamp(m_imageColor[1], 0.f, 1.f) * 255.f)),
            static_cast<uint8_t>(std::lround(std::clamp(m_imageColor[2], 0.f, 1.f) * 255.f)),
            static_cast<uint8_t>(std::lround(std::clamp(m_imageColor[3], 0.f, 1.f) * 255.f)) };
        bool tipReady = true;
        if (!m_imageBrush.tipPath.empty() &&
            m_loadedTipPath != m_imageBrush.tipPath)
        {
            m_loadedTipPath.clear();
            auto tip = Engine::Components::Texture::Acquire(
                m_imageBrush.tipPath, false);
            if (tip && tip->Load() && tip->GetWidth() && tip->GetHeight() &&
                tip->GetPixels().size() ==
                    static_cast<size_t>(tip->GetWidth()) * tip->GetHeight() * 4)
            {
                m_tipPixels = tip->GetPixels();
                m_tipWidth = tip->GetWidth();
                m_tipHeight = tip->GetHeight();
                m_tipUseLuminance = true;
                for (size_t channel = 3; channel < m_tipPixels.size();
                    channel += 4)
                    if (m_tipPixels[channel] != 255)
                    { m_tipUseLuminance = false; break; }
                m_loadedTipPath = m_imageBrush.tipPath;
            }
            else
                m_imageError = "Could not load the image brush tip: " +
                    m_imageBrush.tipPath;
        }
        if (!m_imageBrush.tipPath.empty() && m_loadedTipPath.empty())
            tipReady = false;
        bool changed = false;
        if (m_imageTool == 3)
        {
            if (m_imageMirrorX || m_imageMirrorY)
                m_imageError = "Bucket fill does not support mirroring.";
            else
                changed = Paint::Fill(m_imagePixels, m_imageWidth,
                    m_imageHeight, static_cast<uint32_t>(x),
                    static_cast<uint32_t>(y), m_imageBucketTolerance, color);
        }
        else if (m_imageTool == 2 && m_imageAlphaLock)
            m_imageError = "Turn off Alpha Lock to erase pixels.";
        else if (!tipReady)
        { /* Keep the reported tip error and leave the canvas unchanged. */ }
        else if (m_imageTool == 4 && !m_imageBrush.tipPath.empty())
            m_imageError = "Blur does not support image tips; no stroke was applied.";
        else if (m_imageBrush.shape != 0 ||
            m_imageBrush.rotation != 0.f ||
            m_imageBrush.scatter != 0.f ||
            m_imageBrush.falloff < 0 || m_imageBrush.falloff > 2)
            m_imageError = "This brush uses shape, rotation, scatter, or falloff settings unsupported by PNG paint; no stroke was applied.";
        else
        {
            const float radius = std::max(1.f, m_imageBrush.size * .5f *
                Paint::PressureScale(m_imageBrush.pressureSize));
            const float opacity = m_imageBrush.strength *
                m_imageBrush.opacity * m_imageBrush.flow *
                Paint::PressureScale(m_imageBrush.pressureStrength);
            const float spacing = std::max(1.f,
                m_imageBrush.size * m_imageBrush.spacing);
            const float distance = m_lastImageX < 0.f ? 0.f :
                std::hypot(x - m_lastImageX, y - m_lastImageY);
            if (m_lastImageX < 0.f || distance >= spacing)
            {
                const int steps = m_lastImageX < 0.f ? 1 :
                    std::max(1, static_cast<int>(std::ceil(distance / spacing)));
                for (int step = 1; step <= steps; ++step)
                {
                    const float t = static_cast<float>(step) / steps;
                    const float px = m_lastImageX < 0.f ? x :
                        m_lastImageX + (x - m_lastImageX) * t;
                    const float py = m_lastImageY < 0.f ? y :
                        m_lastImageY + (y - m_lastImageY) * t;
                    for (int mirrorY = 0;
                        mirrorY <= static_cast<int>(m_imageMirrorY); ++mirrorY)
                    for (int mirrorX = 0;
                        mirrorX <= static_cast<int>(m_imageMirrorX); ++mirrorX)
                    {
                        const float stampX = mirrorX ? m_imageWidth - px : px;
                        const float stampY = mirrorY ? m_imageHeight - py : py;
                        if ((mirrorX && std::abs(stampX - px) < .01f) ||
                            (mirrorY && std::abs(stampY - py) < .01f))
                            continue;
                        if (m_imageTool == 4)
                        {
                            changed |= Paint::BlurStamp(m_imagePixels,
                                m_imageWidth, m_imageHeight, stampX, stampY,
                                radius, opacity, m_imageBrush.hardness,
                                m_imageBrush.falloff, m_imageAlphaLock);
                            continue;
                        }
                        uint8_t sampled[4]{};
                        const uint8_t* paintColor = color;
                        if (m_imageTool == 5) paintColor = m_smearColor;
                        else if (m_imageTool == 6)
                        {
                            Paint::AverageColor(m_imagePixels, m_imageWidth,
                                m_imageHeight, stampX, stampY, radius, sampled);
                            paintColor = sampled;
                        }
                        changed |= Paint::Stamp(m_imagePixels, m_imageWidth,
                            m_imageHeight, stampX, stampY, radius,
                            m_imageBrush.hardness, opacity, paintColor,
                            m_imageTool == 2, m_imageBrush.falloff,
                            m_imageBrush.tipPath.empty() ? nullptr : &m_tipPixels,
                            m_tipWidth, m_tipHeight, m_tipUseLuminance,
                            m_imageAlphaLock, mirrorX != 0, mirrorY != 0);
                    }
                }
                m_lastImageX = x;
                m_lastImageY = y;
            }
        }
        if (changed)
        {
            m_imageStrokeChanged = true;
            m_imageTextureDirty = true;
            m_dirty = true;
            RefreshTitle();
            m_imageError.clear();
        }
        if (m_imageTool == 3) CommitImageStroke();
    }
    if (!m_imageError.empty())
        ui.ColoredLabel(m_imageError.c_str(), {1.f, .4f, .3f, 1.f});
}

void AssetDocumentView::DrawPaintToolbar(IEditorUi& ui,
    const std::string& assetsDirectory, bool singleRow, bool* overflow)
{
    if (!CanPaintImage()) return;
    const float start = ImGui::GetCursorScreenPos().x;
    const float right = start + ImGui::GetContentRegionAvail().x;
    const char* tools[]{ "Cursor", "Brush", "Eraser", "Bucket", "Blur",
        "Smear", "Average" };
    ui.SetNextItemWidth(83.f);
    ui.Combo("##ImagePaintTool", &m_imageTool, tools, 7);
    ImGui::SameLine();
    if (ImGui::Button("Paint...", {0.f, 20.f}))
        ImGui::OpenPopup("##ImagePaintOptions");
    const float remaining = right - ImGui::GetCursorScreenPos().x;
    if (remaining >= 85.f || !singleRow)
    {
        ImGui::SameLine();
        ui.SetNextItemWidth(70.f);
        if (m_imageTool == 3)
            ui.SliderFloat("##ImageBucketTolerance",
                &m_imageBucketTolerance, 0.f, 1.f);
        else
            ui.SliderFloat("##ImageBrushSize", &m_imageBrush.size,
                2.f, 250.f);
    }
    else if (overflow) *overflow = true;
    if (ImGui::IsPopupOpen("##ImagePaintOptions"))
        ImGui::SetNextWindowSizeConstraints({290.f, 0.f}, {460.f, 650.f});
    if (!ImGui::BeginPopup("##ImagePaintOptions")) return;
    ImGui::TextUnformatted("Image Paint Options");
    ImGui::Separator();
    Paint::DrawBrushPreview(m_imageBrush);
    namespace fs = std::filesystem;
    std::vector<fs::path> paths;
    std::error_code error;
    const fs::path root = assetsDirectory.empty() ? fs::path("Assets") :
        fs::path(assetsDirectory);
    if (fs::is_directory(root, error))
        for (fs::recursive_directory_iterator it(root,
                fs::directory_options::skip_permission_denied, error), end;
            !error && it != end; it.increment(error))
            if (it->is_regular_file() && it->path().extension() == ".brush")
                paths.push_back(it->path());
    std::sort(paths.begin(), paths.end());
    std::vector<std::string> labels{ "Custom" };
    int choice = 0;
    for (const auto& path : paths)
    {
        labels.push_back(path.lexically_relative(root).string());
        if (path.string() == m_imageBrushPresetPath)
            choice = static_cast<int>(labels.size()) - 1;
    }
    std::vector<const char*> names;
    for (const auto& label : labels) names.push_back(label.c_str());
    if (ui.Combo("Brush preset", &choice, names.data(),
        static_cast<int>(names.size())))
    {
        if (choice == 0) m_imageBrushPresetPath.clear();
        else
        {
            Paint::BrushPreset loaded;
            if (Paint::LoadBrushPreset(paths[choice - 1], loaded, m_imageError))
            {
                m_imageBrush = std::move(loaded);
                m_imageBrushPresetPath = paths[choice - 1].string();
            }
        }
    }
    char presetName[128]{};
    const size_t nameLength = std::min(m_imageBrush.name.size(),
        sizeof(presetName) - 1);
    std::copy_n(m_imageBrush.name.data(), nameLength, presetName);
    if (ui.InputText("Preset name", presetName, sizeof(presetName)))
        m_imageBrush.name = presetName;
    const auto savePreset = [&](bool duplicate)
    {
        const fs::path brushDirectory = root / "Brushes";
        std::error_code createError;
        fs::create_directories(brushDirectory, createError);
        if (createError)
        { m_imageError = createError.message(); return; }
        fs::path path = m_imageBrushPresetPath;
        if (duplicate || path.empty())
        {
            std::string base = m_imageBrush.name.empty() ? "Brush" :
                m_imageBrush.name;
            for (char& ch : base)
                if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
                    ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
                    ch = '_';
            path = brushDirectory / (base + ".brush");
            for (int suffix = 2; fs::exists(path); ++suffix)
                path = brushDirectory /
                    (base + " " + std::to_string(suffix) + ".brush");
        }
        if (Paint::SaveBrushPreset(path, m_imageBrush, m_imageError))
            m_imageBrushPresetPath = path.string();
    };
    if (ui.Button("Create brush asset")) savePreset(true);
    if (!m_imageBrushPresetPath.empty() &&
        ui.Button("Save brush asset")) savePreset(false);
    if (!m_imageBrushPresetPath.empty() &&
        ui.Button("Duplicate brush asset")) savePreset(true);
    char tipPath[512]{};
    const size_t tipLength = std::min(m_imageBrush.tipPath.size(),
        sizeof(tipPath) - 1);
    std::copy_n(m_imageBrush.tipPath.data(), tipLength, tipPath);
    if (ui.InputText("Image tip asset path", tipPath, sizeof(tipPath)))
        m_imageBrush.tipPath = tipPath;
    if (ui.Button("Import image tip..."))
    {
        wchar_t selected[MAX_PATH]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
        dialog.lpstrFile = selected;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrFilter = L"Image tips (*.png;*.jpg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All files (*.*)\0*.*\0";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&dialog))
        {
            const fs::path source(selected);
            const fs::path directory = root / "Brushes" / "Tips";
            std::error_code copyError;
            fs::create_directories(directory, copyError);
            fs::path destination = directory / source.filename();
            for (int suffix = 2; !copyError && fs::exists(destination);
                ++suffix)
                destination = directory / (source.stem().string() + " " +
                    std::to_string(suffix) + source.extension().string());
            if (!copyError)
                fs::copy_file(source, destination, copyError);
            if (copyError) m_imageError = copyError.message();
            else
            {
                const fs::path relative = fs::relative(destination,
                    fs::current_path(), copyError);
                m_imageBrush.tipPath = copyError ? destination.string() :
                    relative.string();
                m_loadedTipPath.clear();
                m_imageError.clear();
            }
        }
    }
    if (!m_imageBrush.tipPath.empty() && m_previewScene)
    {
        auto* graphics = m_previewScene->GetGraphicsProvider();
        auto tip = Engine::Components::Texture::Acquire(
            m_imageBrush.tipPath, false);
        if (tip && graphics && tip->Prepare(graphics) &&
            tip->GetGraphicsTexture())
            ui.DrawImage(tip->GetGraphicsTexture()->GetNativeHandle(),
                72.f, 72.f);
        else ui.DisabledLabel("Image tip preview unavailable.");
    }
    ui.ColorEdit4("Paint color", m_imageColor);
    ImGui::Checkbox("Mirror X", &m_imageMirrorX);
    ImGui::Checkbox("Mirror Y", &m_imageMirrorY);
    ImGui::Checkbox("Alpha Lock", &m_imageAlphaLock);
    if (m_imageTool == 3)
        ui.SliderFloat("Color tolerance", &m_imageBucketTolerance, 0.f, 1.f);
    else
    {
        ui.SliderFloat("Size", &m_imageBrush.size, 2.f, 250.f);
        ui.SliderFloat("Hardness", &m_imageBrush.hardness, 0.f, 1.f);
        ui.SliderFloat("Strength", &m_imageBrush.strength, 0.f, 1.f);
        ui.SliderFloat("Opacity", &m_imageBrush.opacity, 0.f, 1.f);
        ui.SliderFloat("Flow", &m_imageBrush.flow, 0.f, 1.f);
        ui.SliderFloat("Spacing", &m_imageBrush.spacing, 0.f, 1.f);
        ui.SliderFloat("Pressure size", &m_imageBrush.pressureSize, 0.f, 1.f);
        ui.SliderFloat("Pressure strength", &m_imageBrush.pressureStrength,
            0.f, 1.f);
    }
    if (m_imageBrush.shape != 0 || m_imageBrush.rotation != 0.f ||
        m_imageBrush.scatter != 0.f ||
        m_imageBrush.falloff < 0 || m_imageBrush.falloff > 2)
        ui.ColoredLabel("This preset has settings unsupported by PNG paint; strokes are blocked.",
            {1.f, .6f, .3f, 1.f});
    if (!m_imageError.empty())
        ui.ColoredLabel(m_imageError.c_str(), {1.f, .4f, .3f, 1.f});
    ImGui::EndPopup();
}
}
