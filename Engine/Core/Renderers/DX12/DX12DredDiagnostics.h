#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <sstream>
#include "Core/Renderers/GraphicsDebugLayer.h"

namespace Engine::Renderers
{
inline void EnableDredDiagnostics()
{
    if (!IsGraphicsDebugLayerEnabled())
        return;

    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
    {
        settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
}

inline void AppendDredDiagnostics(ID3D12Device* device, std::ostream& output)
{
    if (!device || !IsGraphicsDebugLayerEnabled())
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
}
