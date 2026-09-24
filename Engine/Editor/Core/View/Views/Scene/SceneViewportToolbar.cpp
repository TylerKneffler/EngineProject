#include "SceneViewportToolbar.h"

#include "SceneCameraController.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/geometric.hpp>

namespace Engine::Editor
{
namespace
{
const char* RenderModeLabel(Engine::Model::SceneRenderMode mode)
{
    switch (mode)
    {
    case Engine::Model::SceneRenderMode::Unlit:
        return "Unlit";
    case Engine::Model::SceneRenderMode::Wireframe:
        return "Wire";
    default:
        return "Lit";
    }
}

bool Contains(EditorUiVec2 point, EditorUiVec2 center, float radius)
{
    const float x = point.x - center.x;
    const float y = point.y - center.y;
    return x * x + y * y <= radius * radius;
}

float Cross(EditorUiVec2 a, EditorUiVec2 b, EditorUiVec2 point)
{
    return (b.x-a.x)*(point.y-a.y)-(b.y-a.y)*(point.x-a.x);
}

bool PointInTriangle(EditorUiVec2 point, EditorUiVec2 a,
    EditorUiVec2 b, EditorUiVec2 c)
{
    const float first=Cross(a,b,point);
    const float second=Cross(b,c,point);
    const float third=Cross(c,a,point);
    const bool negative=first<0.f||second<0.f||third<0.f;
    const bool positive=first>0.f||second>0.f||third>0.f;
    return !(negative&&positive);
}

float DistanceToSegment(EditorUiVec2 point,EditorUiVec2 a,EditorUiVec2 b)
{
    const float dx=b.x-a.x,dy=b.y-a.y;
    const float lengthSquared=dx*dx+dy*dy;
    const float parameter=lengthSquared>0.0001f?std::clamp(
        ((point.x-a.x)*dx+(point.y-a.y)*dy)/lengthSquared,0.f,1.f):0.f;
    const float x=a.x+dx*parameter-point.x;
    const float y=a.y+dy*parameter-point.y;
    return std::sqrt(x*x+y*y);
}

EditorUiColor FaceColor(int axis,int sign,bool hovered)
{
    const EditorUiColor colors[3]={
        {0.80f,0.18f,0.16f,0.94f},
        {0.18f,0.66f,0.24f,0.94f},
        {0.16f,0.38f,0.86f,0.94f}
    };
    EditorUiColor result=colors[axis];
    const float shade=sign>0?1.f:.68f;
    result.r*=shade;result.g*=shade;result.b*=shade;
    if(hovered)
    {
        result.r=std::min(1.f,result.r+.22f);
        result.g=std::min(1.f,result.g+.22f);
        result.b=std::min(1.f,result.b+.22f);
    }
    return result;
}

void DrawToolIcon(IEditorUi& ui, EditorTransformTool tool,
    EditorUiVec2 center, EditorUiColor color, float scale = 1.f)
{
    if (tool == EditorTransformTool::Translate)
    {
        ui.DrawViewportLine({center.x - 10.f * scale, center.y},
            {center.x + 10.f * scale, center.y}, color, 2.f * scale);
        ui.DrawViewportLine({center.x, center.y - 10.f * scale},
            {center.x, center.y + 10.f * scale}, color, 2.f * scale);
        ui.DrawViewportTriangle({center.x + 12.f * scale, center.y},
            {center.x + 7.f * scale, center.y - 4.f * scale},
            {center.x + 7.f * scale, center.y + 4.f * scale}, color);
        ui.DrawViewportTriangle({center.x - 12.f * scale, center.y},
            {center.x - 7.f * scale, center.y - 4.f * scale},
            {center.x - 7.f * scale, center.y + 4.f * scale}, color);
        ui.DrawViewportTriangle({center.x, center.y - 12.f * scale},
            {center.x - 4.f * scale, center.y - 7.f * scale},
            {center.x + 4.f * scale, center.y - 7.f * scale}, color);
        ui.DrawViewportTriangle({center.x, center.y + 12.f * scale},
            {center.x - 4.f * scale, center.y + 7.f * scale},
            {center.x + 4.f * scale, center.y + 7.f * scale}, color);
    }
    else if (tool == EditorTransformTool::Rotate)
    {
        ui.DrawViewportCircle(center, 10.f * scale, color, false, 2.f * scale);
        ui.DrawViewportTriangle({center.x + 11.f * scale, center.y - 2.f * scale},
            {center.x + 5.f * scale, center.y - 7.f * scale},
            {center.x + 12.f * scale, center.y - 9.f * scale}, color);
    }
    else if (tool == EditorTransformTool::Scale)
    {
        ui.DrawViewportLine({center.x - 8.f * scale, center.y + 8.f * scale},
            {center.x + 8.f * scale, center.y - 8.f * scale}, color, 3.f * scale);
        ui.DrawViewportCircle({center.x - 9.f * scale, center.y + 9.f * scale}, 4.f * scale,
            color, true);
        ui.DrawViewportCircle({center.x + 9.f * scale, center.y - 9.f * scale}, 4.f * scale,
            color, true);
    }
    else
    {
        ui.DrawViewportCircle({center.x, center.y + 4.f * scale}, 7.f * scale,
            color, false, 2.f * scale);
        for (int finger = -2; finger <= 2; ++finger)
            ui.DrawViewportLine({center.x + finger * 3.f * scale, center.y + 2.f * scale},
                {center.x + finger * 3.f * scale,
                    center.y + (-9.f + std::abs(finger)) * scale},
                color, 2.f * scale);
    }
}

void DrawGridIcon(IEditorUi& ui, EditorUiVec2 center,
    EditorUiColor color, float scale = 1.f)
{
    const float half = 9.f * scale;
    for (int line = -1; line <= 1; ++line)
    {
        const float offset = static_cast<float>(line) * 6.f * scale;
        ui.DrawViewportLine(
            { center.x - half, center.y + offset },
            { center.x + half, center.y + offset },
            color, 1.8f * scale);
        ui.DrawViewportLine(
            { center.x + offset, center.y - half },
            { center.x + offset, center.y + half },
            color, 1.8f * scale);
    }
}
}

bool SceneViewportToolbar::Draw(IEditorUi& ui,
    const EditorUiViewportInput& input, Engine::Scene::Scene* scene)
{
    const bool toolbarConsumedClick = DrawTransformToolbar(ui, input);
    const bool gridToggleConsumedClick = DrawGridToggle(ui, input, scene);
    const bool renderModeConsumedClick =
        DrawRenderModeMenu(ui, input, scene);
    const bool sceneUiOverlayConsumedClick =
        DrawSceneUiOverlayToggle(ui, input, scene);
    const bool orientationConsumedClick=
        DrawOrientationGizmo(ui,input,scene);
    return toolbarConsumedClick || gridToggleConsumedClick ||
        renderModeConsumedClick || sceneUiOverlayConsumedClick ||
        orientationConsumedClick;
}

bool SceneViewportToolbar::DrawOrientationGizmo(IEditorUi& ui,
    const EditorUiViewportInput& input,Engine::Scene::Scene* scene)
{
    if(!scene||scene->IsEditorMode2D()||input.available.x<150.f||
        input.available.y<120.f)
        return false;
    auto* camera=scene->editorCamera.GetComponent<Engine::Components::Camera>();
    if(!camera)
        return false;

    const glm::vec3 eye=scene->editorCamera.transform.position;
    glm::vec3 forward=camera->target-eye;
    if(glm::length(forward)<0.0001f)
        return false;
    forward=glm::normalize(forward);
    glm::vec3 right=glm::cross(camera->up,forward);
    if(glm::length(right)<0.0001f)
        right={1.f,0.f,0.f};
    right=glm::normalize(right);
    const glm::vec3 up=glm::normalize(glm::cross(forward,right));
    const glm::vec3 cameraDirection=-forward;
    const EditorUiVec2 center{input.available.x-54.f,50.f};
    constexpr float cubeScale=27.f;
    constexpr glm::vec3 vertices[8]={
        {-1.f,-1.f,-1.f},{1.f,-1.f,-1.f},{1.f,1.f,-1.f},{-1.f,1.f,-1.f},
        {-1.f,-1.f,1.f},{1.f,-1.f,1.f},{1.f,1.f,1.f},{-1.f,1.f,1.f}
    };
    EditorUiVec2 projected[8]{};
    for(int index=0;index<8;++index)
        projected[index]={center.x+glm::dot(vertices[index],right)*cubeScale,
            center.y-glm::dot(vertices[index],up)*cubeScale};

    struct Face{int index[4];glm::vec3 normal;int axis;int sign;float depth;};
    std::array<Face,6> faces={{
        {{0,3,7,4},{-1,0,0},0,-1,0},{{1,5,6,2},{1,0,0},0,1,0},
        {{0,4,5,1},{0,-1,0},1,-1,0},{{3,2,6,7},{0,1,0},1,1,0},
        {{0,1,2,3},{0,0,-1},2,-1,0},{{4,7,6,5},{0,0,1},2,1,0}
    }};
    for(Face& face:faces)
        face.depth=glm::dot(face.normal,cameraDirection);
    std::sort(faces.begin(),faces.end(),[](const Face& a,const Face& b)
        {return a.depth<b.depth;});

    int hoveredFace=-1;
    for(int index=0;index<6;++index)
    {
        const Face& face=faces[index];
        if(face.depth<=0.001f)
            continue;
        if(PointInTriangle(input.mousePosInViewport,projected[face.index[0]],
            projected[face.index[1]],projected[face.index[2]])||
            PointInTriangle(input.mousePosInViewport,projected[face.index[0]],
            projected[face.index[2]],projected[face.index[3]]))
            hoveredFace=index;
    }

    constexpr int edges[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},
        {6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    int hoveredEdge=-1;
    float edgeDistance=6.f;
    for(int edge=0;edge<12;++edge)
    {
        const glm::vec3 midpoint=(vertices[edges[edge][0]]+
            vertices[edges[edge][1]])*.5f;
        if(glm::dot(midpoint,cameraDirection)<=-0.001f)
            continue;
        const float distance=DistanceToSegment(input.mousePosInViewport,
            projected[edges[edge][0]],projected[edges[edge][1]]);
        if(distance<edgeDistance)
        {edgeDistance=distance;hoveredEdge=edge;}
    }
    int hoveredCorner=-1;
    float cornerDistance=8.f;
    for(int corner=0;corner<8;++corner)
    {
        if(glm::dot(vertices[corner],cameraDirection)<=-0.001f)
            continue;
        const float dx=input.mousePosInViewport.x-projected[corner].x;
        const float dy=input.mousePosInViewport.y-projected[corner].y;
        const float distance=std::sqrt(dx*dx+dy*dy);
        if(distance<cornerDistance)
        {cornerDistance=distance;hoveredCorner=corner;}
    }

    ui.DrawViewportCircle(center,45.f,{0.055f,0.065f,0.085f,.82f},true);
    for(int index=0;index<6;++index)
    {
        const Face& face=faces[index];
        if(face.depth<=0.001f)
            continue;
        const bool hovered=index==hoveredFace&&hoveredEdge<0&&hoveredCorner<0;
        const EditorUiColor color=FaceColor(face.axis,face.sign,hovered);
        ui.DrawViewportTriangle(projected[face.index[0]],projected[face.index[1]],
            projected[face.index[2]],color);
        ui.DrawViewportTriangle(projected[face.index[0]],projected[face.index[2]],
            projected[face.index[3]],color);
        for(int side=0;side<4;++side)
            ui.DrawViewportLine(projected[face.index[side]],
                projected[face.index[(side+1)%4]],{.04f,.05f,.07f,.95f},1.5f);
        EditorUiVec2 label{};
        for(int vertex:face.index)
        {label.x+=projected[vertex].x*.25f;label.y+=projected[vertex].y*.25f;}
        const char* positiveLabels[3]={"X","Y","Z"};
        const char* negativeLabels[3]={"-X","-Y","-Z"};
        ui.DrawViewportText({label.x-(face.sign>0?3.f:7.f),label.y-6.f},
            face.sign>0?positiveLabels[face.axis]:negativeLabels[face.axis],
            {1.f,1.f,1.f,.95f});
    }
    if(hoveredEdge>=0)
        ui.DrawViewportLine(projected[edges[hoveredEdge][0]],
            projected[edges[hoveredEdge][1]],{1.f,.82f,.16f,1.f},4.f);
    if(hoveredCorner>=0)
        ui.DrawViewportCircle(projected[hoveredCorner],7.f,
            {1.f,.82f,.16f,1.f},true);

    const bool projectionHovered=input.mousePosInViewport.x>=center.x-28.f&&
        input.mousePosInViewport.x<=center.x+28.f&&
        input.mousePosInViewport.y>=center.y+43.f&&
        input.mousePosInViewport.y<=center.y+61.f;
    ui.DrawViewportText({center.x-24.f,center.y+45.f},
        camera->orthographic?"Ortho":"Persp",
        projectionHovered?EditorUiColor{1.f,.82f,.16f,1.f}:
            EditorUiColor{.82f,.86f,.94f,.95f});

    if(!input.rawLeftClicked)
        return false;
    if(projectionHovered)
    {
        camera->orthographic=!camera->orthographic;
        return true;
    }
    if(hoveredCorner>=0)
    {
        SceneCameraController::SnapToDirection(*scene,vertices[hoveredCorner]);
        return true;
    }
    if(hoveredEdge>=0)
    {
        SceneCameraController::SnapToDirection(*scene,
            vertices[edges[hoveredEdge][0]]+vertices[edges[hoveredEdge][1]]);
        return true;
    }
    if(hoveredFace>=0)
    {
        SceneCameraController::SnapToDirection(*scene,faces[hoveredFace].normal);
        return true;
    }
    return false;
}

bool SceneViewportToolbar::DrawTransformToolbar(IEditorUi& ui,
    const EditorUiViewportInput& input)
{
    if (input.available.x < 164.f || input.available.y < 44.f)
        return false;

    constexpr float scale = .72f;
    constexpr float radius = 16.f;
    constexpr float spacing = 38.f;
    constexpr float padding = 10.f;
    constexpr EditorTransformTool tools[] = {
        EditorTransformTool::Hand,
        EditorTransformTool::Translate,
        EditorTransformTool::Rotate,
        EditorTransformTool::Scale
    };
    bool consumed = false;
    for (int index = 0; index < 4; ++index)
    {
        const EditorUiVec2 center{padding+radius+
            spacing*static_cast<float>(index),padding+radius};
        const bool hovered = Contains(input.mousePosInViewport, center, radius);
        if (input.rawLeftClicked && hovered)
        {
            m_transformTool = tools[index];
            consumed = true;
        }
        const bool selected = tools[index] == m_transformTool;
        ui.DrawViewportCircle(center, radius,
            selected ? EditorUiColor{0.18f, 0.38f, 0.68f, 0.98f}
                     : hovered ? EditorUiColor{0.22f, 0.25f, 0.31f, 0.98f}
                               : EditorUiColor{0.10f, 0.12f, 0.16f, 0.94f}, true);
        ui.DrawViewportCircle(center, radius,
            selected ? EditorUiColor{0.45f, 0.72f, 1.f, 1.f}
                     : EditorUiColor{0.62f, 0.68f, 0.78f, 0.9f}, false,
            1.5f * scale);
        DrawToolIcon(ui, tools[index], center,
            {0.92f, 0.95f, 1.f, 1.f}, scale);
    }
    return consumed;
}

bool SceneViewportToolbar::DrawGridToggle(IEditorUi& ui,
    const EditorUiViewportInput& input, Engine::Scene::Scene* scene)
{
    if (!scene || input.available.x < 202.f || input.available.y < 44.f)
        return false;

    constexpr float scale = .72f;
    constexpr float radius = 16.f;
    constexpr float spacing = 38.f;
    constexpr float padding = 10.f;
    const EditorUiVec2 center{padding+radius+spacing*4.f,padding+radius};
    if (center.x + radius > input.available.x)
        return false;

    const bool hovered = Contains(input.mousePosInViewport, center, radius);
    bool consumed = false;
    if (input.rawLeftClicked && hovered)
    {
        scene->settings.showGrid = !scene->settings.showGrid;
        consumed = true;
    }

    const bool enabled = scene->settings.showGrid;
    ui.DrawViewportCircle(center, radius,
        enabled ? EditorUiColor{0.18f, 0.38f, 0.68f, 0.98f}
                : hovered ? EditorUiColor{0.22f, 0.25f, 0.31f, 0.98f}
                          : EditorUiColor{0.10f, 0.12f, 0.16f, 0.94f}, true);
    ui.DrawViewportCircle(center, radius,
        enabled ? EditorUiColor{0.45f, 0.72f, 1.f, 1.f}
                : EditorUiColor{0.62f, 0.68f, 0.78f, 0.9f}, false,
        1.5f * scale);
    DrawGridIcon(ui, center, {0.92f, 0.95f, 1.f, 1.f}, scale);
    return consumed;
}

bool SceneViewportToolbar::DrawRenderModeMenu(IEditorUi& ui,
    const EditorUiViewportInput& input, Engine::Scene::Scene* scene)
{
    if (!scene || input.available.x < 240.f || input.available.y < 44.f)
        return false;

    constexpr float scale = .72f;
    constexpr float radius = 16.f;
    constexpr float spacing = 38.f;
    constexpr float padding = 10.f;
    const EditorUiVec2 center{
        padding + radius + spacing * 5.f,
        padding + radius
    };
    if (center.x + radius > input.available.x)
        return false;

    bool consumed = false;
    const bool hovered = Contains(input.mousePosInViewport, center, radius);
    if (input.rawLeftClicked && hovered)
    {
        m_renderModeExpanded = !m_renderModeExpanded;
        consumed = true;
    }

    const Engine::Model::SceneRenderMode activeMode = scene->settings.renderMode;
    ui.DrawViewportCircle(center, radius,
        hovered ? EditorUiColor{0.22f, 0.25f, 0.31f, 0.98f}
                : EditorUiColor{0.10f, 0.12f, 0.16f, 0.94f}, true);
    ui.DrawViewportCircle(center, radius,
        {0.62f, 0.68f, 0.78f, 0.9f}, false,
        1.5f * scale);
    ui.DrawViewportText({ center.x - 11.f * scale, center.y - 3.f * scale },
        RenderModeLabel(activeMode), {0.92f, 0.95f, 1.f, 1.f});

    if (!m_renderModeExpanded)
        return consumed;

    constexpr Engine::Model::SceneRenderMode modes[] = {
        Engine::Model::SceneRenderMode::Lit,
        Engine::Model::SceneRenderMode::Unlit,
        Engine::Model::SceneRenderMode::Wireframe
    };
    for (int index = 0; index < 3; ++index)
    {
        const EditorUiVec2 optionCenter{center.x,
            center.y + spacing * static_cast<float>(index + 1)};
        if (optionCenter.y + radius > input.available.y)
            break;
        const bool optionHovered =
            Contains(input.mousePosInViewport, optionCenter, radius);
        if (input.rawLeftClicked && optionHovered)
        {
            scene->settings.renderMode = modes[index];
            m_renderModeExpanded = false;
            consumed = true;
        }
        const bool selected = scene->settings.renderMode == modes[index];
        ui.DrawViewportCircle(optionCenter, radius,
            selected ? EditorUiColor{0.18f, 0.38f, 0.68f, 0.98f}
                     : optionHovered
                        ? EditorUiColor{0.22f, 0.25f, 0.31f, 0.98f}
                        : EditorUiColor{0.10f, 0.12f, 0.16f, 0.94f}, true);
        ui.DrawViewportCircle(optionCenter, radius,
            selected ? EditorUiColor{0.45f, 0.72f, 1.f, 1.f}
                     : EditorUiColor{0.62f, 0.68f, 0.78f, 0.9f}, false,
            1.5f * scale);
        ui.DrawViewportText(
            { optionCenter.x - 11.f * scale, optionCenter.y - 3.f * scale },
            RenderModeLabel(modes[index]), {0.92f, 0.95f, 1.f, 1.f});
    }

    return consumed;
}

bool SceneViewportToolbar::DrawSceneUiOverlayToggle(IEditorUi& ui,
    const EditorUiViewportInput& input, Engine::Scene::Scene* scene)
{
    if (!scene || input.available.x < 278.f || input.available.y < 44.f)
        return false;

    constexpr float scale = .72f;
    constexpr float radius = 16.f;
    constexpr float spacing = 38.f;
    constexpr float padding = 10.f;
    const EditorUiVec2 center{
        padding + radius + spacing * 6.f,
        padding + radius
    };
    if (center.x + radius > input.available.x)
        return false;

    const bool hovered = Contains(input.mousePosInViewport, center, radius);
    bool consumed = false;
    if (input.rawLeftClicked && hovered)
    {
        scene->settings.sceneViewUiOverlay =
            !scene->settings.sceneViewUiOverlay;
        consumed = true;
    }

    const bool enabled = scene->settings.sceneViewUiOverlay;
    ui.DrawViewportCircle(center, radius,
        enabled ? EditorUiColor{0.18f, 0.38f, 0.68f, 0.98f}
                : hovered ? EditorUiColor{0.22f, 0.25f, 0.31f, 0.98f}
                          : EditorUiColor{0.10f, 0.12f, 0.16f, 0.94f}, true);
    ui.DrawViewportCircle(center, radius,
        enabled ? EditorUiColor{0.45f, 0.72f, 1.f, 1.f}
                : EditorUiColor{0.62f, 0.68f, 0.78f, 0.9f}, false,
        1.5f * scale);
    ui.DrawViewportText(
        { center.x - 5.f * scale, center.y - 3.f * scale },
        "UI", {0.92f, 0.95f, 1.f, 1.f});
    return consumed;
}
}
