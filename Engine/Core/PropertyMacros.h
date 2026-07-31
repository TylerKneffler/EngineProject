#pragma once

// ---------------------------------------------------------------------------
// Property Inspector Macros
//
// Use these macros to annotate component properties that should be visible
// and editable in the property inspector. These serve as documentation and
// markers for which fields are serialized and exposed in the editor.
//
// IMPORTANT: Properties marked with PROPERTY(Inspector) MUST be included in
// both Serialize() and Deserialize() methods for the generic DrawProperties
// to display and edit them automatically.
//
// Usage:
//   class MyComponent : public Component {
//   public:
//       PROPERTY(Inspector)
//       float speed = 10.f;
//
//       PROPERTY(Inspector, EditAnywhere)
//       glm::vec3 position { 0.f, 0.f, 0.f };
//
//       PROPERTY(Inspector, EditAnywhere, Category = "Rendering")
//       glm::vec4 color { 1.f, 1.f, 1.f, 1.f };
//
//       JsonValue Serialize() const override {
//           JsonValue data = JsonValue::MakeObject();
//           data.Set("type", JsonValue(std::string("MyComponent")));
//           data.Set("speed", JsonValue(speed));           // Include Inspector properties
//           data.Set("position", J3(position));            // Include Inspector properties
//           data.Set("color", J4(color));                  // Include Inspector properties
//           return data;
//       }
//   };
//
// Workflow:
//   1. Mark public fields with PROPERTY(Inspector, ...)
//   2. Add them to Serialize() - the generic DrawProperties will automatically
//      create appropriate UI controls (sliders, color pickers, etc.)
//   3. Add them to Deserialize() - changes from the UI are written back
//
// Specifiers:
//   - Inspector: Core marker - property appears in inspector via DrawProperties
//   - EditAnywhere: Can be edited in all contexts
//   - EditDefaultsOnly: Only editable on archetypes
//   - VisibleAnywhere: Visible but not editable (read-only)
//   - Category: Groups properties (e.g., Category = "Movement")
//   - Range: Hints for slider ranges (e.g., Range = "0.0, 100.0")
// ---------------------------------------------------------------------------

// Main property macro - expands to nothing at compile time but serves as
// a marker for properties that should be visible in the inspector.
//
// Note: specifier tokens passed into PROPERTY(...) are intentionally *not*
// defined as global macros to avoid leaking names like Category/Tooltip/Range
// into third-party headers.
#define PROPERTY(...)

// Alternative verbose macro name for clarity.
#define INSPECTOR_PROPERTY(...)
