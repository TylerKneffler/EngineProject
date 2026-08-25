#include "Core/UI/UILayout.h"
#include "Core/Compoonents/UI/UIObject.h"

#include "Core/Compoonents/UI/Canvas.h"
#include "Core/Compoonents/UI/UIText.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <cmath>

namespace Engine::UI
{
Engine::Model::UIRect Intersect(const Engine::Model::UIRect& first, const Engine::Model::UIRect& second)
{
    const float left = std::max(first.x, second.x);
    const float top = std::max(first.y, second.y);
    const float right = std::min(first.x + first.width, second.x + second.width);
    const float bottom = std::min(first.y + first.height, second.y + second.height);
    return { left, top, std::max(0.f, right - left), std::max(0.f, bottom - top) };
}

Engine::Model::UIRect AnchoredRect(const Engine::Components::UIObject& item, const Engine::Model::UIRect& parent)
{
    const float anchorWidth = (item.anchorMax.x - item.anchorMin.x) * parent.width;
    const float anchorHeight = (item.anchorMax.y - item.anchorMin.y) * parent.height;
    const float width = std::clamp(anchorWidth + item.sizeDelta.x,
        std::max(0.f, item.minWidth), std::max(item.minWidth, item.maxWidth));
    const float height = std::clamp(anchorHeight + item.sizeDelta.y,
        std::max(0.f, item.minHeight), std::max(item.minHeight, item.maxHeight));
    const float anchorX = parent.x + parent.width * item.anchorMin.x;
    const float anchorY = parent.y + parent.height * item.anchorMin.y;
    return {
        anchorX + item.anchoredPosition.x - item.pivot.x * width,
        anchorY + item.anchoredPosition.y - item.pivot.y * height,
        width, height
    };
}

void ResolveObject(Engine::Core::Object* object, Engine::Components::Canvas* canvas, const glm::vec2& canvasSize,
    const Engine::Model::UIRect& parentRect, const Engine::Model::UIRect& parentClip, int& hierarchyOrder,
    std::vector<Engine::Model::UITextLayout>& output);

void ResolveChildren(Engine::Core::Object* object, Engine::Components::Canvas* canvas, const glm::vec2& canvasSize,
    Engine::Components::UIObject* parentLayout, const Engine::Model::UIRect& parentRect, const Engine::Model::UIRect& parentClip,
    int& hierarchyOrder, std::vector<Engine::Model::UITextLayout>& output)
{
    const bool row = parentLayout && parentLayout->layoutDirection == "Row";
    const bool column = parentLayout && parentLayout->layoutDirection == "Column";
    if (!row && !column)
    {
        const Engine::Model::UIRect childClip = parentLayout && parentLayout->clipChildren
            ? Intersect(parentClip, parentRect) : parentClip;
        for (Engine::Core::Object* child : object->Children)
            ResolveObject(child, canvas, canvasSize, parentRect, childClip,
                hierarchyOrder, output);
        return;
    }

    Engine::Model::UIRect content = parentRect;
    content.x += parentLayout->paddingLeft;
    content.y += parentLayout->paddingTop;
    content.width = std::max(0.f, content.width - parentLayout->paddingLeft - parentLayout->paddingRight);
    content.height = std::max(0.f, content.height - parentLayout->paddingTop - parentLayout->paddingBottom);

    std::vector<std::pair<Engine::Core::Object*, Engine::Components::UIObject*>> children;
    for (Engine::Core::Object* child : object->Children)
        if (child && child->IsEnabledInHierarchy())
            if (Engine::Components::UIObject* layout = child->GetComponent<Engine::Components::UIObject>(); layout && layout->visible)
                children.emplace_back(child, layout);

    float fixed = 0.f;
    float totalGrow = 0.f;
    for (const auto& [child, layout] : children)
    {
        (void)child;
        fixed += row
            ? std::max(0.f, layout->sizeDelta.x) + layout->marginLeft + layout->marginRight
            : std::max(0.f, layout->sizeDelta.y) + layout->marginTop + layout->marginBottom;
        totalGrow += std::max(0.f, layout->flexGrow);
    }
    if (children.size() > 1) fixed += parentLayout->spacing * (children.size() - 1);
    const float mainExtent = row ? content.width : content.height;
    const float remaining = std::max(0.f, mainExtent - fixed);
    float gap = parentLayout->spacing;
    float cursor = row ? content.x : content.y;
    if (parentLayout->justifyContent == "Center") cursor += remaining * 0.5f;
    else if (parentLayout->justifyContent == "End") cursor += remaining;
    else if (parentLayout->justifyContent == "SpaceBetween" && children.size() > 1 && totalGrow <= 0.f)
        gap += remaining / static_cast<float>(children.size() - 1);

    for (const auto& [child, layout] : children)
    {
        const float grow = totalGrow > 0.f
            ? remaining * std::max(0.f, layout->flexGrow) / totalGrow : 0.f;
        Engine::Model::UIRect rect{};
        if (row)
        {
            rect.width = std::clamp(std::max(0.f, layout->sizeDelta.x) + grow,
                std::max(0.f, layout->minWidth), std::max(layout->minWidth, layout->maxWidth));
            rect.height = std::clamp(std::max(0.f, layout->sizeDelta.y),
                std::max(0.f, layout->minHeight), std::max(layout->minHeight, layout->maxHeight));
            if (parentLayout->alignItems == "Stretch")
                rect.height = std::max(0.f, content.height - layout->marginTop - layout->marginBottom);
            rect.x = cursor + layout->marginLeft;
            rect.y = content.y + layout->marginTop;
            if (parentLayout->alignItems == "Center")
                rect.y = content.y + (content.height - rect.height) * 0.5f;
            else if (parentLayout->alignItems == "End")
                rect.y = content.y + content.height - rect.height - layout->marginBottom;
            cursor += layout->marginLeft + rect.width + layout->marginRight + gap;
        }
        else
        {
            rect.height = std::clamp(std::max(0.f, layout->sizeDelta.y) + grow,
                std::max(0.f, layout->minHeight), std::max(layout->minHeight, layout->maxHeight));
            rect.width = std::clamp(std::max(0.f, layout->sizeDelta.x),
                std::max(0.f, layout->minWidth), std::max(layout->minWidth, layout->maxWidth));
            if (parentLayout->alignItems == "Stretch")
                rect.width = std::max(0.f, content.width - layout->marginLeft - layout->marginRight);
            rect.y = cursor + layout->marginTop;
            rect.x = content.x + layout->marginLeft;
            if (parentLayout->alignItems == "Center")
                rect.x = content.x + (content.width - rect.width) * 0.5f;
            else if (parentLayout->alignItems == "End")
                rect.x = content.x + content.width - rect.width - layout->marginRight;
            cursor += layout->marginTop + rect.height + layout->marginBottom + gap;
        }

        const Engine::Model::UIRect clip = parentLayout->clipChildren
            ? Intersect(parentClip, parentRect) : parentClip;
        layout->SetComputedLayout(rect, clip);
        ++hierarchyOrder;
        if (Engine::Components::UIText* text = child->GetComponent<Engine::Components::UIText>())
            output.push_back({ canvas, layout, text, canvasSize,
                canvas->sortingOrder * 1000000 + layout->zOrder * 1000 + hierarchyOrder });
        ResolveChildren(child, canvas, canvasSize, layout, rect,
            layout->GetComputedClipRect(), hierarchyOrder, output);
    }

    // Non-layout grouping objects still pass their descendants through.
    for (Engine::Core::Object* child : object->Children)
        if (!child->GetComponent<Engine::Components::UIObject>())
            ResolveChildren(child, canvas, canvasSize, parentLayout, parentRect,
                parentClip, hierarchyOrder, output);
}

void ResolveObject(Engine::Core::Object* object, Engine::Components::Canvas* canvas, const glm::vec2& canvasSize,
    const Engine::Model::UIRect& parentRect, const Engine::Model::UIRect& parentClip, int& hierarchyOrder,
    std::vector<Engine::Model::UITextLayout>& output)
{
    if (!object || !object->IsEnabledInHierarchy()) return;
    Engine::Components::UIObject* layout = object->GetComponent<Engine::Components::UIObject>();
    if (!layout)
    {
        ResolveChildren(object, canvas, canvasSize, nullptr, parentRect,
            parentClip, hierarchyOrder, output);
        return;
    }
    if (!layout->visible) return;
    layout->SetComputedLayout(AnchoredRect(*layout, parentRect), parentClip);
    ++hierarchyOrder;
    if (Engine::Components::UIText* text = object->GetComponent<Engine::Components::UIText>())
        output.push_back({ canvas, layout, text, canvasSize,
            canvas->sortingOrder * 1000000 + layout->zOrder * 1000 + hierarchyOrder });
    ResolveChildren(object, canvas, canvasSize, layout, layout->GetComputedRect(),
        layout->GetComputedClipRect(), hierarchyOrder, output);
}
std::vector<Engine::Model::UITextLayout> UILayout::Resolve(
    Engine::Scene::Scene& scene, float viewportAspect)
{
    std::vector<Engine::Model::UITextLayout> output;
    for (const auto& candidate : scene.GetObjects())
    {
        Engine::Core::Object* object = candidate.get();
        Engine::Components::Canvas* canvas = object && object->IsEnabledInHierarchy()
            ? object->GetComponent<Engine::Components::Canvas>() : nullptr;
        for (Engine::Core::Object* ancestor = object ? object->Parent : nullptr;
            canvas && ancestor; ancestor = ancestor->Parent)
            if (ancestor->GetComponent<Engine::Components::Canvas>()) canvas = nullptr;
        if (!canvas) continue;
        const glm::vec2 size = canvas->GetLogicalSize(viewportAspect);
        const Engine::Model::UIRect root { 0.f, 0.f, size.x, size.y };
        int hierarchyOrder = 0;
        if (Engine::Components::UIObject* rootLayout = object->GetComponent<Engine::Components::UIObject>())
        {
            rootLayout->SetComputedLayout(root, root);
            if (Engine::Components::UIText* text = object->GetComponent<Engine::Components::UIText>())
                output.push_back({ canvas, rootLayout, text, size,
                    canvas->sortingOrder * 1000000 + rootLayout->zOrder * 1000 });
            ResolveChildren(object, canvas, size, rootLayout, root, root,
                hierarchyOrder, output);
        }
        else
            ResolveChildren(object, canvas, size, nullptr, root, root,
                hierarchyOrder, output);
    }
    std::stable_sort(output.begin(), output.end(), [](const auto& first, const auto& second)
    { return first.sortKey < second.sortKey; });
    return output;
}
}
