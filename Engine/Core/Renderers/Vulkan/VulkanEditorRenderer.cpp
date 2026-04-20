#include "pch.h"
#include "VulkanEditorRenderer.h"
#include "VulkanGraphicsProvider.h"

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
VulkanEditorRenderer::VulkanEditorRenderer()
{
    // Initialize SRV free list (slot 0 reserved for ImGui font atlas)
    for (uint32_t i = MAX_SRV_SLOTS - 1; i >= 1; --i)
        m_freeSrvSlots.push_back(i);
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
VulkanEditorRenderer::~VulkanEditorRenderer()
{
    // Cleanup handled by unique_ptr members
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool VulkanEditorRenderer::Init(void* hwnd, uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;

    // TODO: Initialize Vulkan instance, physical device, logical device
    // TODO: Create swapchain for window
    // TODO: Create command pool and command buffers
    // TODO: Create synchronization primitives (fences, semaphores)
    // TODO: Create render pass and framebuffers

    // Create graphics provider
    m_graphicsProvider = std::make_unique<VulkanGraphicsProvider>(
        m_device, m_graphicsQueue, m_commandPool);

    return m_graphicsProvider != nullptr;
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------
void VulkanEditorRenderer::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;
    
    m_width = width;
    m_height = height;
    MarkDirty();

    // TODO: Recreate swapchain and framebuffers
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------
void VulkanEditorRenderer::Clear(float r, float g, float b, float a)
{
    MarkDirty();
    // TODO: Record clear command for next frame
}

// ---------------------------------------------------------------------------
// GetGraphicsProvider
// ---------------------------------------------------------------------------
IGraphicsProvider* VulkanEditorRenderer::GetGraphicsProvider()
{
    return m_graphicsProvider.get();
}

// ---------------------------------------------------------------------------
// MarkDirty
// ---------------------------------------------------------------------------
void VulkanEditorRenderer::MarkDirty()
{
    m_isDirty = true;
}

// ---------------------------------------------------------------------------
// RenderIfNeeded
// ---------------------------------------------------------------------------
void VulkanEditorRenderer::RenderIfNeeded(std::function<void()> drawFn)
{
    if (!m_isDirty) return;

    // TODO: Record and execute render commands
    // - Begin command buffer
    // - Begin render pass
    // - Invoke drawFn callback
    // - End render pass
    // - Submit command buffer
    // - Present swapchain image

    m_isDirty = false;
}

// ---------------------------------------------------------------------------
// AllocateSrvSlot
// ---------------------------------------------------------------------------
std::pair<std::pair<void*, void*>, uint32_t> VulkanEditorRenderer::AllocateSrvSlot()
{
    if (m_freeSrvSlots.empty())
        return {{nullptr, nullptr}, 0};

    uint32_t slotIndex = m_freeSrvSlots.back();
    m_freeSrvSlots.pop_back();

    // TODO: Return actual descriptor handles for the allocated slot
    return {{nullptr, nullptr}, slotIndex};
}

// ---------------------------------------------------------------------------
// FreeSrvSlot
// ---------------------------------------------------------------------------
void VulkanEditorRenderer::FreeSrvSlot(uint32_t slotIndex)
{
    m_freeSrvSlots.push_back(slotIndex);
}

// ---------------------------------------------------------------------------
// CanAllocateSrvSlot
// ---------------------------------------------------------------------------
bool VulkanEditorRenderer::CanAllocateSrvSlot() const
{
    return !m_freeSrvSlots.empty();
}

// ---------------------------------------------------------------------------
// GetAvailableSrvSlots
// ---------------------------------------------------------------------------
uint32_t VulkanEditorRenderer::GetAvailableSrvSlots() const
{
    return static_cast<uint32_t>(m_freeSrvSlots.size());
}
