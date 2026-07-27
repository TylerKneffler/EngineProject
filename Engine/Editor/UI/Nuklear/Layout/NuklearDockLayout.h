#pragma once

// Nuklear layout feature.

struct nk_context;

struct NuklearDockGeometry
{
    float width = 0.f;
    float height = 0.f;
    float toolbarHeight = 76.f;
    float splitterSize = 6.f;
    float left = 0.f;
    float center = 0.f;
    float right = 0.f;
    float workspace = 0.f;
    float console = 0.f;

    float CenterX() const { return left + splitterSize; }
    float RightX() const { return CenterX() + center + splitterSize; }
};

// Owns Nuklear's resizable editor-dock geometry and splitter interaction.
class NuklearDockLayout
{
public:
    NuklearDockGeometry Update(nk_context& context, float width, float height);
    void DrawSplitters(nk_context& context, const NuklearDockGeometry& geometry) const;

private:
    NuklearDockGeometry Calculate(float width, float height) const;

    float m_leftFraction = 0.20f;
    float m_rightFraction = 0.25f;
    float m_bottomFraction = 0.24f;
    int m_draggedSplitter = 0;
};
