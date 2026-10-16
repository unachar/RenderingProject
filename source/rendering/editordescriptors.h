#pragma once
#include "main.h"
#include <memory>

struct EditorDescriptorLease
{
    UINT Index = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu{};
    ~EditorDescriptorLease();
};

class EditorDescriptors
{
public:
    static std::shared_ptr<EditorDescriptorLease> Allocate();
    static void Retire(UINT index);
    static void Collect();
    static UINT Available();
    static void Shutdown();
};

