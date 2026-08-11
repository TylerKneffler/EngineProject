#include "Core/Compoonents/UI/Canvas.h"
#include "Core/Compoonents/UI/UIObject.h"
#include "Core/Compoonents/UI/UIText.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/UI/UILayout.h"
#include <cmath>
#include <iostream>
#include <memory>

namespace
{
void Parent(Object* child, Object* parent)
{
    child->Parent = parent;
    parent->Children.push_back(child);
}

bool Near(float first, float second)
{
    return std::abs(first - second) < 0.01f;
}
}

int main()
{
    Scene scene;
    Object* root = scene.AddObject("HUD");
    Canvas* canvas = root->AddComponent<Canvas>();
    canvas->referenceResolution = { 1920.f, 1080.f, 0.f };
    UIObject* row = root->AddComponent<UIObject>();
    row->layoutDirection = "Row";
    row->alignItems = "Stretch";
    row->paddingLeft = row->paddingRight = 10.f;
    row->paddingTop = row->paddingBottom = 20.f;
    row->spacing = 5.f;

    Object* fixedObject = scene.AddObject("Fixed Label");
    Parent(fixedObject, root);
    UIObject* fixed = fixedObject->AddComponent<UIObject>();
    fixed->sizeDelta = { 100.f, 50.f, 0.f };
    UIText* fixedText = fixedObject->AddComponent<UIText>();
    fixedText->text = "Native UI";

    Object* flexibleObject = scene.AddObject("Flexible Label");
    Parent(flexibleObject, root);
    UIObject* flexible = flexibleObject->AddComponent<UIObject>();
    flexible->sizeDelta = { 100.f, 50.f, 0.f };
    flexible->flexGrow = 1.f;
    UIText* flexibleText = flexibleObject->AddComponent<UIText>();
    flexibleText->text = "Flexible";

    const std::vector<UITextLayout> resolved = UILayout::Resolve(scene, 16.f / 9.f);
    if (resolved.size() != 2 || !Near(fixed->GetComputedRect().x, 10.f) ||
        !Near(fixed->GetComputedRect().width, 100.f) ||
        !Near(fixed->GetComputedRect().height, 1040.f) ||
        !Near(flexible->GetComputedRect().x, 115.f) ||
        !Near(flexible->GetComputedRect().width, 1795.f))
    {
        std::cerr << "Retained row/flex layout produced unexpected rectangles\n";
        return 1;
    }

    Scene restored;
    if (!SceneSerializer::LoadFromString(restored, scene.SaveToString()))
    {
        std::cerr << "UI scene serialization failed\n";
        return 1;
    }
    bool restoredText = false;
    for (const auto& object : restored.GetObjects())
        if (const UIText* text = object->GetComponent<UIText>())
            restoredText |= text->text == "Native UI";
    if (!restoredText)
    {
        std::cerr << "UIText properties did not survive serialization\n";
        return 1;
    }

    const char* types[] = { "Canvas", "UIObject", "UIText" };
    for (const char* type : types)
    {
        std::unique_ptr<Component> component(SceneSerializer::CreateRegisteredComponent(type));
        if (!component || component->GetTypeName() != type)
        {
            std::cerr << type << " is not registered\n";
            return 1;
        }
    }
    return 0;
}
