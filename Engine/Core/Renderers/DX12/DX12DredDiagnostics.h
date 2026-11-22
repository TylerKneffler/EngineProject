#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include "Core/Renderers/GraphicsDebugLayer.h"
#include "Core/Renderers/GpuFailureHook.h"
#include "Core/Rendering/DebugDrawTags.h"
#include <cwchar>

namespace Engine::Renderers
{
// DRED alone, without the validation layers: ENGINE_GPU_DRED=1. The debug
// layer changes timing enough to hide some faults (the landscape GPU hangs
// were never seen with it on); breadcrumbs alone cost a few percent.
inline bool IsDredEnabled()
{
    static const bool enabled = []
    {
        if (IsGraphicsDebugLayerEnabled()) return true;
        const char* value = std::getenv("ENGINE_GPU_DRED");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

inline void EnableDredDiagnostics()
{
    if (!IsDredEnabled())
        return;

    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
    {
        settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
    // Marker text (SetDebugMarker) is kept only with breadcrumb contexts on.
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings1;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings1))))
        settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
}

inline void AppendDredDiagnostics(ID3D12Device* device, std::ostream& output)
{
    if (!device || !IsDredEnabled())
        return;

    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
        return;

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
    {
        size_t listIndex = 0;
        for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode;
             node; node = node->pNext, ++listIndex)
        {
            const UINT completed = node->pLastBreadcrumbValue
                ? *node->pLastBreadcrumbValue : 0u;
            output << "\nDRED breadcrumb list " << listIndex
                << ": completed=" << completed
                << "/" << node->BreadcrumbCount;
            if (node->pCommandListDebugNameW)
                output << ", commandList=" << node->pCommandListDebugNameW;
            if (node->pCommandQueueDebugNameW)
                output << ", commandQueue=" << node->pCommandQueueDebugNameW;
            // A list the GPU did not finish: its whole history, one hex digit
            // pair per operation (D3D12_AUTO_BREADCRUMB_OP), so the pass that
            // hung can be counted out (6 = Dispatch, 5 = ExecuteIndirect).
            if (node->pCommandHistory && completed < node->BreadcrumbCount)
            {
                output << ", history=";
                const UINT shown = (std::min)(node->BreadcrumbCount, 4000u);
                for (UINT operation = 0; operation < shown; ++operation)
                {
                    if (operation == completed) output << '|';
                    output << std::hex << static_cast<unsigned>(node->pCommandHistory[operation])
                        << std::dec << ' ';
                }
            }
            if (node->pCommandHistory && node->BreadcrumbCount)
            {
                const UINT first = completed > 3u ? completed - 3u : 0u;
                const UINT last = (std::min)(node->BreadcrumbCount,
                    completed + 5u);
                output << ", nearbyOps=";
                for (UINT operation = first; operation < last; ++operation)
                {
                    if (operation != first) output << ',';
                    output << operation << ':'
                        << static_cast<unsigned>(node->pCommandHistory[operation]);
                }
            }
        }
    }

    // Markers (SetDebugMarker) around the operation each unfinished list
    // stopped at, with the renderer's description of the draw they tag.
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs1{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred1))) &&
        SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&breadcrumbs1)))
    {
        size_t listIndex = 0;
        for (auto* node = breadcrumbs1.pHeadAutoBreadcrumbNode; node; node = node->pNext, ++listIndex)
        {
            const UINT completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0u;
            if (completed == 0u || completed >= node->BreadcrumbCount || !node->pBreadcrumbContexts)
                continue;
            for (UINT index = 0; index < node->BreadcrumbContextsCount; ++index)
            {
                const auto& entry = node->pBreadcrumbContexts[index];
                if (!entry.pContextString || entry.BreadcrumbIndex + 40u < completed ||
                    entry.BreadcrumbIndex > completed + 12u) continue;
                unsigned long long tag = 0;
                if (swscanf_s(entry.pContextString, L"tag %llu", &tag) != 1) continue;
                output << "\nDRED list " << listIndex << " marker at op " << entry.BreadcrumbIndex
                    << (entry.BreadcrumbIndex <= completed ? " (before stop): " : " (after stop): ")
                    << Engine::Rendering::DebugDrawTags::Get().Describe(tag);
            }
        }
    }

    if (Engine::Rendering::DebugDrawTags::Enabled())
    {
        output << std::dec;
        Engine::Rendering::DebugDrawTags::Get().DumpAll(output);
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)) &&
        pageFault.PageFaultVA != 0)
    {
        output << "\nDRED page fault VA=0x" << std::hex << pageFault.PageFaultVA;
        const auto appendAllocations = [&](const char* label,
            const D3D12_DRED_ALLOCATION_NODE* allocation)
        {
            size_t index = 0;
            for (; allocation && index < 16u;
                 allocation = allocation->pNext, ++index)
            {
                output << "\nDRED " << label << '[' << index << "]: type="
                    << static_cast<unsigned>(allocation->AllocationType);
                if (allocation->ObjectNameA)
                    output << ", name=" << allocation->ObjectNameA;
            }
        };
        appendAllocations("existing allocation",
            pageFault.pHeadExistingAllocationNode);
        appendAllocations("recently freed allocation",
            pageFault.pHeadRecentFreedAllocationNode);
    }
}

// Prints the removed reason and the DRED output to stderr the first time a
// graphics call fails on a removed device (see GpuFailureHook.h).
inline void RegisterDeviceRemovedReport(ID3D12Device* device)
{
    GpuFailureHook() = [device](const char* where)
    {
        static bool reported = false;
        if (reported || !device) return;
        const HRESULT reason = device->GetDeviceRemovedReason();
        if (SUCCEEDED(reason)) return;
        reported = true;
        std::ostringstream message;
        message << "[gpu] device removed at " << (where ? where : "?") << ": reason=0x" << std::hex
            << static_cast<unsigned long>(reason) << std::dec;
        if (!IsDredEnabled()) message << " (set ENGINE_GPU_DRED=1 for breadcrumbs)";
        AppendDredDiagnostics(device, message);
        std::fprintf(stderr, "%s%c", message.str().c_str(), 10);
        std::fflush(stderr);
    };
}
}
