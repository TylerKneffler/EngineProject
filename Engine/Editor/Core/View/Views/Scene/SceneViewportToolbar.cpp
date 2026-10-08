#include "SceneViewportToolbar.h"

#include "SceneCameraController.h"
#include "Core/Object.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>

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

EditorUiVec2 Add(EditorUiVec2 a,EditorUiVec2 b)
{
    return {a.x+b.x,a.y+b.y};
}

EditorUiVec2 Subtract(EditorUiVec2 a,EditorUiVec2 b)
{
    return {a.x-b.x,a.y-b.y};
}

EditorUiVec2 Multiply(EditorUiVec2 value,float scale)
{
    return {value.x*scale,value.y*scale};
}

float Dot(EditorUiVec2 a,EditorUiVec2 b)
{
    return a.x*b.x+a.y*b.y;
}

float Length(EditorUiVec2 value)
{
    return std::sqrt(Dot(value,value));
}

bool SceneContains(const Engine::Scene::Scene& scene,
    const Engine::Core::Object* object)
{
    for(const auto& candidate:scene.GetObjects())
        if(candidate.get()==object)
            return true;
    return false;
}

void DrawBoxHandle(IEditorUi& ui,EditorUiVec2 center,float half,
    EditorUiColor color)
{
    const EditorUiVec2 a{center.x-half,center.y-half};
    const EditorUiVec2 b{center.x+half,center.y-half};
    const EditorUiVec2 c{center.x+half,center.y+half};
    const EditorUiVec2 d{center.x-half,center.y+half};
    ui.DrawViewportTriangle(a,b,c,color);
    ui.DrawViewportTriangle(a,c,d,color);
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
    const EditorUiViewportInput& input, Engine::Scene::Scene* scene,
    bool allowTransformTools, bool allowObjectTransform)
{
    const bool toolbarConsumedClick = m_sceneToolsVisible && allowTransformTools
        ? DrawTransformToolbar(ui, input) : false;
    const bool gridToggleConsumedClick = m_sceneToolsVisible &&
        DrawGridToggle(ui, input, scene);
    const bool renderModeConsumedClick =
        m_sceneToolsVisible && DrawRenderModeMenu(ui, input, scene);
    const bool sceneUiOverlayConsumedClick =
        m_sceneToolsVisible && DrawSceneUiOverlayToggle(ui, input, scene);
    const bool orientationConsumedClick=
        DrawOrientationGizmo(ui,input,scene,allowObjectTransform);
    return toolbarConsumedClick || gridToggleConsumedClick ||
        renderModeConsumedClick || sceneUiOverlayConsumedClick ||
        orientationConsumedClick;
}

bool SceneViewportToolbar::DrawOrientationGizmo(IEditorUi& ui,
    const EditorUiViewportInput& input,Engine::Scene::Scene* scene,
    bool allowObjectTransform)
{
    if (!allowObjectTransform)
    {
        m_cubeDragObject = nullptr;
        m_cubeDragAxis = -1;
    }
    if(!scene||scene->IsEditorMode2D()||input.available.x<90.f||
        input.available.y<70.f)
    {
        m_cubeDragObject=nullptr;
        m_cubeDragAxis=-1;
        return false;
    }
    auto* camera=scene->editorCamera.GetComponent<Engine::Components::Camera>();
    if(!camera)
        return false;

    if(m_cubeDragObject&&(!SceneContains(*scene,m_cubeDragObject)||
        input.leftReleased||!input.leftDown))
    {
        m_cubeDragObject=nullptr;
        m_cubeDragAxis=-1;
    }

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
    constexpr float gizmoScale=.5f;
    const EditorUiVec2 center{input.available.x-31.f,29.f};
    constexpr float cubeScale=24.f*gizmoScale;
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
    float edgeDistance=5.f;
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
    float cornerDistance=6.f;
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

    Engine::Core::Object* selected=allowObjectTransform
        ? scene->GetSelectedObject() : nullptr;
    constexpr glm::vec3 worldAxes[3]={{1.f,0.f,0.f},{0.f,1.f,0.f},
        {0.f,0.f,1.f}};
    constexpr EditorUiColor axisColors[3]={{.95f,.20f,.18f,1.f},
        {.25f,.85f,.30f,1.f},{.22f,.48f,1.f,1.f}};
    constexpr EditorUiColor hoverColor{1.f,.82f,.16f,1.f};
    EditorUiVec2 handleDirections[3]{};
    EditorUiVec2 handleStarts[3]{};
    EditorUiVec2 handleEnds[3]{};
    int hoveredHandle=-1;
    float closestHandleDistance=6.f;
    EditorUiVec2 hoveredDragDirection{};
    if(selected&&m_transformTool!=EditorTransformTool::Hand)
    {
        constexpr EditorUiVec2 fallbackDirections[3]={{1.f,0.f},{0.f,-1.f},
            {-.70710678f,.70710678f}};
        for(int axis=0;axis<3;++axis)
        {
            EditorUiVec2 direction{glm::dot(worldAxes[axis],right),
                -glm::dot(worldAxes[axis],up)};
            const float directionLength=Length(direction);
            direction=directionLength>.16f
                ?Multiply(direction,1.f/directionLength)
                :fallbackDirections[axis];
            handleDirections[axis]=direction;
            handleStarts[axis]=Add(center,Multiply(direction,29.f*gizmoScale));
            handleEnds[axis]=Add(center,Multiply(direction,53.f*gizmoScale));
        }

        if(m_transformTool==EditorTransformTool::Rotate)
        {
            constexpr int segments=64;
            constexpr float radius=47.f*gizmoScale;
            for(int axis=0;axis<3;++axis)
            {
                const glm::vec3 basisA=worldAxes[(axis+1)%3];
                const glm::vec3 basisB=worldAxes[(axis+2)%3];
                EditorUiVec2 previous{};
                for(int segment=0;segment<=segments;++segment)
                {
                    constexpr float twoPi=6.28318530718f;
                    const float angle=twoPi*static_cast<float>(segment)/segments;
                    const glm::vec3 point=basisA*std::cos(angle)+
                        basisB*std::sin(angle);
                    const EditorUiVec2 current{center.x+glm::dot(point,right)*radius,
                        center.y-glm::dot(point,up)*radius};
                    if(segment>0)
                    {
                        const float distance=DistanceToSegment(
                            input.mousePosInViewport,previous,current);
                        if(distance<closestHandleDistance)
                        {
                            closestHandleDistance=distance;
                            hoveredHandle=axis;
                            const EditorUiVec2 tangent=Subtract(current,previous);
                            const float tangentLength=Length(tangent);
                            if(tangentLength>.001f)
                                hoveredDragDirection=Multiply(tangent,
                                    1.f/tangentLength);
                        }
                    }
                    previous=current;
                }
            }
        }
        else
        {
            for(int axis=0;axis<3;++axis)
            {
                const float distance=DistanceToSegment(input.mousePosInViewport,
                    handleStarts[axis],handleEnds[axis]);
                if(distance<closestHandleDistance)
                {
                    closestHandleDistance=distance;
                    hoveredHandle=axis;
                    hoveredDragDirection=handleDirections[axis];
                }
            }
        }
    }

    ui.DrawViewportCircle(center,56.f*gizmoScale,
        {0.055f,0.065f,0.085f,.82f},true);
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
                projected[face.index[(side+1)%4]],{.04f,.05f,.07f,.95f},1.f);
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
            projected[edges[hoveredEdge][1]],{1.f,.82f,.16f,1.f},2.f);
    if(hoveredCorner>=0)
        ui.DrawViewportCircle(projected[hoveredCorner],3.5f,
            {1.f,.82f,.16f,1.f},true);

    if(selected&&m_transformTool!=EditorTransformTool::Hand)
    {
        const char* labels[3]={"X","Y","Z"};
        if(m_transformTool==EditorTransformTool::Rotate)
        {
            constexpr int segments=64;
            constexpr float radius=47.f*gizmoScale;
            for(int axis=0;axis<3;++axis)
            {
                const glm::vec3 basisA=worldAxes[(axis+1)%3];
                const glm::vec3 basisB=worldAxes[(axis+2)%3];
                const EditorUiColor color=axis==hoveredHandle||
                    (m_cubeDragObject&&axis==m_cubeDragAxis)
                    ?hoverColor:axisColors[axis];
                EditorUiVec2 previous{};
                for(int segment=0;segment<=segments;++segment)
                {
                    constexpr float twoPi=6.28318530718f;
                    const float angle=twoPi*static_cast<float>(segment)/segments;
                    const glm::vec3 point=basisA*std::cos(angle)+
                        basisB*std::sin(angle);
                    const EditorUiVec2 current{center.x+glm::dot(point,right)*radius,
                        center.y-glm::dot(point,up)*radius};
                    if(segment>0)
                    {
                        ui.DrawViewportLine(previous,current,
                            {.025f,.03f,.04f,.96f},2.5f);
                        ui.DrawViewportLine(previous,current,color,1.25f);
                    }
                    previous=current;
                }
            }
        }
        else
        {
            for(int axis=0;axis<3;++axis)
            {
                const EditorUiColor color=axis==hoveredHandle||
                    (m_cubeDragObject&&axis==m_cubeDragAxis)
                    ?hoverColor:axisColors[axis];
                ui.DrawViewportLine(handleStarts[axis],handleEnds[axis],
                    {.025f,.03f,.04f,.96f},3.f);
                ui.DrawViewportLine(handleStarts[axis],handleEnds[axis],color,1.5f);
                if(m_transformTool==EditorTransformTool::Translate)
                {
                    const EditorUiVec2 perpendicular{
                        -handleDirections[axis].y,handleDirections[axis].x};
                    const EditorUiVec2 arrowBase=Subtract(handleEnds[axis],
                        Multiply(handleDirections[axis],5.f));
                    ui.DrawViewportTriangle(handleEnds[axis],
                        Add(arrowBase,Multiply(perpendicular,2.5f)),
                        Subtract(arrowBase,Multiply(perpendicular,2.5f)),color);
                }
                else
                    DrawBoxHandle(ui,handleEnds[axis],2.5f,color);
                ui.DrawViewportText(Add(handleEnds[axis],
                    Multiply(EditorUiVec2{-handleDirections[axis].y,
                        handleDirections[axis].x},3.f)),labels[axis],color);
            }
        }
    }

    const bool projectionHovered=input.mousePosInViewport.x>=center.x-15.f&&
        input.mousePosInViewport.x<=center.x+15.f&&
        input.mousePosInViewport.y>=center.y+21.5f&&
        input.mousePosInViewport.y<=center.y+30.5f;
    ui.DrawViewportText({center.x-12.f,center.y+22.5f},
        camera->orthographic?"Ortho":"Persp",
        projectionHovered?EditorUiColor{1.f,.82f,.16f,1.f}:
            EditorUiColor{.82f,.86f,.94f,.95f});

    if(m_cubeDragObject&&input.leftDown)
    {
        const float pixels=Dot(Subtract(input.mousePosInViewport,
            m_cubeDragStartMouse),m_cubeDragScreenDirection);
        if(m_cubeDragTool==EditorTransformTool::Translate)
        {
            const glm::vec3 worldDelta=worldAxes[m_cubeDragAxis]*pixels*
                m_cubeDragWorldUnitsPerPixel;
            glm::vec3 localDelta=worldDelta;
            if(m_cubeDragObject->Parent)
            {
                const glm::mat4 parentWorld=
                    m_cubeDragObject->Parent->transform.GetWorldMatrix();
                if(std::abs(glm::determinant(parentWorld))>.000001f)
                    localDelta=glm::vec3(glm::inverse(parentWorld)*
                        glm::vec4(worldDelta,0.f));
            }
            m_cubeDragObject->transform.position=
                m_cubeDragStartPosition+localDelta;
        }
        else if(m_cubeDragTool==EditorTransformTool::Rotate)
            m_cubeDragObject->transform.rotation[m_cubeDragAxis]=
                m_cubeDragStartRotation[m_cubeDragAxis]+pixels*.01f;
        else if(m_cubeDragTool==EditorTransformTool::Scale)
            m_cubeDragObject->transform.scale[m_cubeDragAxis]=std::max(.001f,
                m_cubeDragStartScale[m_cubeDragAxis]+pixels*.01f);
        const uint8_t channel = m_cubeDragTool == EditorTransformTool::Translate
            ? Engine::Components::Transform::EditorPosition
            : m_cubeDragTool == EditorTransformTool::Rotate
                ? Engine::Components::Transform::EditorRotation
                : Engine::Components::Transform::EditorScale;
        m_cubeDragObject->transform.NotifyEditorTransformChanged(channel);
        if(auto* body=m_cubeDragObject->GetComponent<
            Engine::Components::RigidBody>())
            body->NotifyEditorTransformChanged();
        return true;
    }

    if(!input.rawLeftClicked)
        return false;
    if(selected&&hoveredHandle>=0&&
        Length(hoveredDragDirection)>.5f)
    {
        m_cubeDragObject=selected;
        m_cubeDragAxis=hoveredHandle;
        m_cubeDragTool=m_transformTool;
        m_cubeDragStartMouse=input.mousePosInViewport;
        m_cubeDragScreenDirection=hoveredDragDirection;
        m_cubeDragStartPosition=selected->transform.position;
        m_cubeDragStartRotation=selected->transform.rotation;
        m_cubeDragStartScale=selected->transform.scale;
        const float objectDistance=glm::length(
            selected->transform.GetWorldPosition()-eye);
        m_cubeDragWorldUnitsPerPixel=std::clamp(objectDistance*.003f,
            .0025f,.25f);
        return true;
    }
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
    if (input.available.x < 164.f || input.available.y < 90.f)
        return false;

    constexpr float scale = .72f;
    constexpr float radius = 16.f;
    constexpr float spacing = 38.f;
    constexpr float padding = 54.f;
    constexpr EditorTransformTool tools[] = {
        EditorTransformTool::Hand,
        EditorTransformTool::Translate,
        EditorTransformTool::Rotate,
        EditorTransformTool::Scale
    };
    bool consumed = false;
    for (int index = 0; index < 4; ++index)
    {
        const EditorUiVec2 center{10.f+radius+
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
    if (!scene || input.available.x < 202.f || input.available.y < 90.f)
        return false;

    constexpr float scale = .72f;
    constexpr float radius = 16.f;
    constexpr float spacing = 38.f;
    constexpr float padding = 54.f;
    const EditorUiVec2 center{10.f+radius+spacing*4.f,padding+radius};
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
    if (!scene || input.available.x < 240.f || input.available.y < 90.f)
        return false;

    constexpr float scale = .72f;
    constexpr float radius = 16.f;
    constexpr float spacing = 38.f;
    constexpr float padding = 54.f;
    const EditorUiVec2 center{
        10.f + radius + spacing * 5.f,
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
    if (!scene || input.available.x < 278.f || input.available.y < 90.f)
        return false;

    constexpr float scale = .72f;
    constexpr float radius = 16.f;
    constexpr float spacing = 38.f;
    constexpr float padding = 54.f;
    const EditorUiVec2 center{
        10.f + radius + spacing * 6.f,
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
