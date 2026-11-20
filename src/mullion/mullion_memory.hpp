#pragma once
#include "Metal.hpp"
#include "mullion_workarounds.hpp"
#include <cstdint>

namespace mullion::memory {

// the memory an application is told it may use and the most Mullion lets it allocate: 95% of the device's
// recommendedMaxWorkingSetSize, leaving the system its share of unified memory (and macOS 26 its kernel: Admit)
uint64_t Limit(WMT::Device device);

// whether the macOS release faults on going past recommendedMaxWorkingSetSize (macOS 26 and earlier)
bool OverLimitPanics();

// the budget an application is told (DXGI's QueryVideoMemoryInfo): Limit while macOS has memory to spare, and less as
// it runs short, as Windows lowers a budget under memory pressure so a game shrinks its pools before the system pages:
// at warning pressure three quarters of Limit, and no more than what the device holds plus what macOS can still give
// without paging; at critical pressure half of Limit. Asked of macOS at most every 250 ms, since a game polls it each
// frame (MULLION_MEMORY_BUDGET=fixed keeps it at Limit)
uint64_t Budget(WMT::Device device);

// events set when the budget changes (DXGI's RegisterVideoMemoryBudgetChangeNotificationEvent), and once when
// registered, as Windows does. A thread watches the budget while any is registered
uint32_t RegisterBudgetEvent(WMT::Device device, void *event);
void UnregisterBudgetEvent(uint32_t cookie);

// whether an application allocation of `size()` bytes may be made. Before macOS 27, one that takes the device's
// allocated memory past Limit is refused, and the creation fails with E_OUTOFMEMORY as Windows fails one past what a
// GPU has: allocating more than recommendedMaxWorkingSetSize and making it resident panics the kernel there
// (workaround::Id::MemoryLimit). `size` is called only when that applies, as it may ask Metal for the size
template <typename Size>
bool
Admit(WMT::Device device, Size &&size) {
  if (!OverLimitPanics() || !workaround::Enabled(workaround::Id::MemoryLimit))
    return true;
  uint64_t bytes = size();
  if (device.currentAllocatedSize() + bytes <= Limit(device))
    return true;
  workaround::Acted(workaround::Id::MemoryLimit, "an allocation past 95% of the GPU's working set was refused");
  return false;
}

// whether a texture of this description is made with lossy compression, the memory saver: MULLION_LOSSY_TEXTURES=1
// (off by default; it trades some of a texture's precision for half its memory) on an Apple8 GPU or later, for a
// private texture Metal can compress so (an M6, macOS 27.3, with API validation: a color format of 8, 16 or 32 bits a
// channel, not packed, block-compressed, depth or stencil; nothing a shader writes, no view of another format; MSAA,
// arrays, 3D, cubes, mipmaps and textures placed in a heap all can). a 4096x4096 RGBA8 texture takes 32 MB instead of
// 64 (tools/metal-bench/lossy.swift). in practice that is Direct3D 12's private textures without a format view: its
// half and float targets (RGBA16F, R32F, RG16F...), the large ones of an HDR renderer; an RGBA8 texture keeps the view
// relaxed format casting gives it, and stays lossless. D3D11's textures are shared, and Metal compresses none of those
// so (d3d12_lossy_textures)
bool Lossy(WMT::Device device, const WMTTextureInfo &info);

} // namespace mullion::memory
