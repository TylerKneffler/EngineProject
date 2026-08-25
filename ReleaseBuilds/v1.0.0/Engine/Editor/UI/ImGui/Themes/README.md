# ImGui editor themes

The editor scans this directory for files ending in `.imguitheme`. A valid file
uses an `ImGuiTheme` root with `version="1"`, one `Style` element, and one
`Colors` element. Copy either bundled theme when creating a new one.

Theme names must be unique. Style attributes must be supported by
`ImGuiThemeManager`, and every color name must match the active Dear ImGui
version. At minimum, a theme must define `Text`, `WindowBg`, `FrameBg`,
`Button`, and `Header`; unspecified colors inherit Dear ImGui's dark style.

Use **Project Preferences > Editor > Rescan Themes** after adding or changing a
file. Invalid files are excluded from the dropdown and reported in the window.
