#include "Core/Renderers/DX12/DX12GraphicsBuffer.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
int main(){try{
 ComPtr<ID3D12Device> device;check(SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"device");
 D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;ComPtr<ID3D12CommandQueue> queue;check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"queue");
 Engine::Renderers::D3D12BufferFactory factory(device.Get(),queue.Get());
 std::array<unsigned char,4096> expected{};for(size_t i=0;i<expected.size();++i)expected[i]=static_cast<unsigned char>((i*37+11)&255);
 auto buffer=factory.CreateStaticVertexBufferDeferred(expected.size(),expected.data());check(buffer!=nullptr,"deferred buffer");
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;hp.CreationNodeMask=1;hp.VisibleNodeMask=1;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=expected.size();desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 ComPtr<ID3D12Resource> readback;check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))),"readback");
 ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))),"allocator");check(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"list");
 auto* resource=static_cast<ID3D12Resource*>(buffer->GetNativeHandle());D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=resource;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;list->ResourceBarrier(1,&barrier);list->CopyBufferRegion(readback.Get(),0,resource,0,expected.size());check(SUCCEEDED(list->Close()),"close");ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
 ComPtr<ID3D12Fence> fence;check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"fence");HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"event");check(SUCCEEDED(queue->Signal(fence.Get(),1))&&SUCCEEDED(fence->SetEventOnCompletion(1,event)),"signal");check(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"readback wait");
 void* mapped=nullptr;D3D12_RANGE range{0,expected.size()};check(SUCCEEDED(readback->Map(0,&range,&mapped)),"map");check(std::memcmp(mapped,expected.data(),expected.size())==0,"readback bytes differ");D3D12_RANGE empty{0,0};readback->Unmap(0,&empty);
 for(int i=0;i<16;++i){auto removed=factory.CreateStaticVertexBufferDeferred(expected.size(),expected.data());check(removed!=nullptr,"early release upload");removed.reset();}
 { Engine::Renderers::D3D12BufferFactory shortLived(device.Get(),queue.Get());for(int i=0;i<4;++i){auto b=shortLived.CreateStaticVertexBufferDeferred(expected.size(),expected.data());check(b!=nullptr,"factory destruction upload");} }
 check(SUCCEEDED(queue->Signal(fence.Get(),2))&&SUCCEEDED(fence->SetEventOnCompletion(2,event)),"release signal");check(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"release wait");CloseHandle(event);check(SUCCEEDED(device->GetDeviceRemovedReason()),"device removed");std::cout<<"DX12 deferred upload readback and early release passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
