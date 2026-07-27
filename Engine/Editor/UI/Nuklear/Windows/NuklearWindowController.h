#pragma once

// Nuklear window-lifecycle feature.

#include <string>
#include <unordered_set>

struct nk_context;

// Owns one-at-a-time Nuklear window lifecycle state: requested bounds,
// begin/end balancing, and close/reopen translation.
class NuklearWindowController
{
public:
    void SetContext(nk_context* context) { m_context = context; }
    void SetNextRect(float x, float y, float width, float height);
    bool Begin(const char* title, bool* open);
    void End();

    const std::string& CurrentWindow() const { return m_currentWindow; }

private:
    nk_context* m_context = nullptr;
    float m_x = 0.f;
    float m_y = 0.f;
    float m_width = 400.f;
    float m_height = 300.f;
    bool m_begun = false;
    bool m_hasNextRect = false;
    std::string m_currentWindow;
    std::unordered_set<std::string> m_closedWindows;
};
