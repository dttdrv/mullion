// contract: a D3D11 device made on a D3D12 device (D3D11On12CreateDevice) shares its resources: a wrapped resource
// is the D3D12 resource's memory, D3D11's work on it sees the queue's work before AcquireWrappedResources, and the
// queue's work after ReleaseWrappedResources sees D3D11's; the same holds for unwrapping and returning a resource.
// Microsoft Learn, D3D11On12CreateDevice: "Creates a device that uses Direct3D 11 functionality in Direct3D 12".
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-d3d11on12createdevice
// ID3D11On12Device::AcquireWrappedResources: "Acquires D3D11 resources for use with D3D 11on12. Indicates that
// rendering to the wrapped resources can begin again."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-id3d11on12device-acquirewrappedresources
// ID3D11On12Device2::UnwrapUnderlyingResource: "Takes an existing D3D11 resource that was created by ... and
// returns the underlying D3D12 resource", and ReturnUnderlyingResource: "Returns ownership of a D3D12 resource".
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nn-d3d11on12-id3d11on12device2
// the D3D12 work here is submitted without waiting on the CPU, so only the 11on12 synchronization orders it.
#include "d3d12_test.hpp"
#include <d3d11_4.h>
#include <d3d11on12.h>
#include <algorithm>
#include <thread>

static const UINT kSize = 64;

// a D3D11 texture's first texel, read through a staging copy
static UINT
texel11(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture) {
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
    return 0xdeadbeef;
  context->CopyResource(staging.Get(), texture);
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    return 0xdeadbeef;
  UINT value = *static_cast<UINT *>(mapped.pData);
  context->Unmap(staging.Get(), 0);
  return value;
}

// a D3D11 buffer's first word
static UINT
word11(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Buffer *buffer) {
  D3D11_BUFFER_DESC desc;
  buffer->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.MiscFlags = 0;
  desc.StructureByteStride = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Buffer> staging;
  if (FAILED(device->CreateBuffer(&desc, nullptr, &staging)))
    return 0xdeadbeef;
  context->CopyResource(staging.Get(), buffer);
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    return 0xdeadbeef;
  UINT value = *static_cast<UINT *>(mapped.pData);
  context->Unmap(staging.Get(), 0);
  return value;
}

int
main(int argc, char **argv) {
  std::thread([] {
    Sleep(120000);
    expect(false, "the test did not finish within 120 s: a wait was never satisfied");
    ExitProcess(verdict());
  }).detach();

  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  // the queue's work so far, which the CPU then waits for
  ComPtr<ID3D12Fence> drain;
  UINT64 drained = 0;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain)));
  auto finish = [&] {
    auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(drain.Get(), ++drained);
    drain->SetEventOnCompletion(drained, event);
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);
  };
  // a gate the CPU opens later, which holds the queue's work or D3D11's: a step whose handover isn't synchronized
  // then reads what was there before, every time, and not only when the GPU happens to be slow
  ComPtr<ID3D12Fence> gate;
  UINT64 gated = 0;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
  auto open_later = [&] {
    return std::thread([&, value = gated] {
      Sleep(200);
      gate->Signal(value);
    });
  };
  // records a list and gives it to the queue, without waiting; a held list waits for the gate's next opening
  auto record = [&](auto &&commands, bool held = false) {
    finish();
    if (held)
      queue->Wait(gate.Get(), ++gated);
    allocator->Reset();
    list->Reset(allocator.Get(), nullptr);
    commands();
    list->Close();
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
  };

  step("D3D11On12CreateDevice's arguments");
  {
    ComPtr<ID3D11Device> device11;
    IUnknown *queues[] = {queue.Get()};
    expect(
        D3D11On12CreateDevice(device.Get(), 0, nullptr, 0, queues, 0, 0, &device11, nullptr, nullptr) == E_INVALIDARG,
        "no queue is accepted"
    );
    expect(
        D3D11On12CreateDevice(nullptr, 0, nullptr, 0, queues, 1, 0, &device11, nullptr, nullptr) == E_INVALIDARG,
        "no device is accepted"
    );
  }

  step("the device and its interfaces");
  ComPtr<ID3D11Device> device11;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level{};
  IUnknown *queues[] = {queue.Get()};
  CHECK(D3D11On12CreateDevice(device.Get(), 0, nullptr, 0, queues, 1, 0, &device11, &context, &level));
  expect(level >= D3D_FEATURE_LEVEL_11_0, "feature level %#x", level);
  expect(
      device11->GetFeatureLevel() == level, "the device's feature level %#x, the chosen one %#x",
      device11->GetFeatureLevel(), level
  );
  ComPtr<ID3D11On12Device> on12;
  ComPtr<ID3D11On12Device2> on12_2;
  CHECK(device11.As(&on12));
  CHECK(device11.As(&on12_2));
  {
    ComPtr<ID3D11Device> back;
    CHECK(on12.As(&back));
    expect(back.Get() == device11.Get(), "ID3D11On12Device's ID3D11Device is not the device");
    ComPtr<ID3D12Device> d3d12;
    CHECK(on12_2->GetD3D12Device(__uuidof(ID3D12Device), d3d12.GetAddressOf()));
    expect(d3d12.Get() == device.Get(), "GetD3D12Device is not the D3D12 device");
    // with no feature levels asked for, the D3D12 device's, as far as D3D11 has it
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS query{UINT(std::size(levels)), levels};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &query, sizeof(query)));
    ComPtr<ID3D11Device> plain;
    D3D_FEATURE_LEVEL plain_level{};
    CHECK(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, std::size(levels), D3D11_SDK_VERSION, &plain,
        &plain_level, nullptr
    ));
    D3D_FEATURE_LEVEL want = std::min(query.MaxSupportedFeatureLevel, plain_level);
    expect(
        level == want, "feature level %#x with none asked for, want %#x (D3D12's %#x, D3D11's %#x)", level, want,
        query.MaxSupportedFeatureLevel, plain_level
    );
    ComPtr<ID3D11On12Device> none;
    expect(plain.As(&none) == E_NOINTERFACE, "a device D3D11CreateDevice made is an ID3D11On12Device");

    // and the one asked for
    const D3D_FEATURE_LEVEL asked[] = {D3D_FEATURE_LEVEL_10_1};
    ComPtr<ID3D11Device> device10;
    D3D_FEATURE_LEVEL level10{};
    CHECK(D3D11On12CreateDevice(device.Get(), 0, asked, 1, queues, 1, 0, &device10, nullptr, &level10));
    expect(level10 == D3D_FEATURE_LEVEL_10_1, "feature level %#x when 10_1 is asked for", level10);
  }

  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      0,
      kSize,
      kSize,
      1,
      1,
      DXGI_FORMAT_R8G8B8A8_UNORM,
      {1, 0},
      D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  ComPtr<ID3D12Resource> texture12;
  CHECK(device->CreateCommittedResource(
      &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
      IID_PPV_ARGS(&texture12)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(texture12.Get(), nullptr, rtv);
  auto clear12 = [&](float r, float g, float b, bool held = false) {
    record(
        [&] {
          const float color[] = {r, g, b, 1};
          list->ClearRenderTargetView(rtv, color, 0, nullptr);
        },
        held
    );
  };
  // the D3D12 texture's first texel, after the queue's work so far
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 256 * kSize, D3D12_RESOURCE_STATE_COPY_DEST);
  auto texel12 = [&]() -> UINT {
    forget(readback.Get());
    record([&] {
      transition(list.Get(), texture12.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, 256}};
      D3D12_TEXTURE_COPY_LOCATION src{texture12.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(list.Get(), texture12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    });
    finish();
    void *bytes;
    if (FAILED(readback->Map(0, nullptr, &bytes)))
      return 0xdeadbeef;
    UINT value = *static_cast<UINT *>(bytes);
    readback->Unmap(0, nullptr);
    return value;
  };

  step("wrapping a texture checks its bindings against the D3D12 resource's flags");
  {
    D3D11_RESOURCE_FLAGS flags{D3D11_BIND_UNORDERED_ACCESS};
    ComPtr<ID3D11Texture2D> wrapped;
    expect(
        on12->CreateWrappedResource(
            texture12.Get(), &flags, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
            IID_PPV_ARGS(&wrapped)
        ) == E_INVALIDARG,
        "a UAV binding of a texture without ALLOW_UNORDERED_ACCESS is accepted"
    );
  }

  step("a wrapped texture is the D3D12 texture, and D3D11 sees the queue's work before it was wrapped");
  clear12(1, 0, 0, true);
  auto opener = open_later();
  D3D11_RESOURCE_FLAGS flags{D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
  ComPtr<ID3D11Texture2D> texture11;
  CHECK(on12->CreateWrappedResource(
      texture12.Get(), &flags, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
      IID_PPV_ARGS(&texture11)
  ));
  {
    D3D11_TEXTURE2D_DESC desc;
    texture11->GetDesc(&desc);
    expect(
        desc.Width == kSize && desc.Height == kSize && desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
            desc.BindFlags == (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) &&
            desc.Usage == D3D11_USAGE_DEFAULT,
        "the wrapped texture's description: %ux%u, format %u, bindings %#x, usage %u", desc.Width, desc.Height,
        desc.Format, desc.BindFlags, desc.Usage
    );
  }
  UINT got = texel11(device11.Get(), context.Get(), texture11.Get());
  opener.join();
  expect(got == 0xff0000ff, "D3D11 read %#x of the queue's red clear before wrapping", got);
  ComPtr<ID3D11RenderTargetView> rtv11;
  CHECK(device11->CreateRenderTargetView(texture11.Get(), nullptr, &rtv11));
  ID3D11Resource *wrapped[] = {texture11.Get()};

  // a few rounds, each handing the texture over in both directions
  const float colors[][3] = {{0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {0, 1, 1}};
  const UINT want11[] = {0xff00ff00, 0xffff0000, 0xff00ffff, 0xffffff00};
  for (UINT round = 0; round < std::size(colors); round++) {
    step("round %u: the queue sees D3D11's clear after ReleaseWrappedResources", round);
    const float color[] = {colors[round][0], colors[round][1], colors[round][2], 1};
    // D3D11's clear waits for the gate (a returned resource's fence)
    UINT64 value = ++gated;
    ID3D12Fence *gates[] = {gate.Get()};
    CHECK(on12_2->ReturnUnderlyingResource(texture11.Get(), 1, &value, gates));
    opener = open_later();
    context->ClearRenderTargetView(rtv11.Get(), color);
    on12->ReleaseWrappedResources(wrapped, 1);
    context->Flush();
    got = texel12();
    opener.join();
    expect(got == want11[round], "the queue read %#x after D3D11's clear, want %#x", got, want11[round]);

    step("round %u: D3D11 sees the queue's clear after AcquireWrappedResources", round);
    clear12(0.5f, 0, 0, true);
    opener = open_later();
    on12->AcquireWrappedResources(wrapped, 1);
    got = texel11(device11.Get(), context.Get(), texture11.Get());
    opener.join();
    expect(got == 0xff000080, "D3D11 read %#x after the queue's clear, want 0xff000080", got);
  }

  step("UnwrapUnderlyingResource gives the D3D12 resource, after D3D11's work");
  {
    const float color[] = {1, 0, 1, 1};
    context->ClearRenderTargetView(rtv11.Get(), color);
    ComPtr<ID3D12Resource> unwrapped;
    CHECK(on12_2->UnwrapUnderlyingResource(texture11.Get(), queue.Get(), IID_PPV_ARGS(&unwrapped)));
    expect(unwrapped.Get() == texture12.Get(), "the unwrapped resource is not the wrapped one");
    got = texel12();
    expect(got == 0xffff00ff, "the queue read %#x after unwrapping, want 0xffff00ff", got);
    ComPtr<ID3D12Resource> none;
    expect(
        FAILED(on12_2->UnwrapUnderlyingResource(nullptr, queue.Get(), IID_PPV_ARGS(&none))), "a null resource unwraps"
    );
  }

  step("ReturnUnderlyingResource: D3D11's later work waits for the fences it is given");
  {
    ComPtr<ID3D12Fence> fence;
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    UINT64 value = 1;
    ID3D12Fence *fences[] = {fence.Get()};
    CHECK(on12_2->ReturnUnderlyingResource(texture11.Get(), 1, &value, fences));
    // the signal comes after the return, from the CPU: D3D11's read must not run before it
    std::thread late([&] {
      Sleep(200);
      clear12(0, 0, 0.5f);
      finish();
      fence->Signal(1);
    });
    got = texel11(device11.Get(), context.Get(), texture11.Get());
    late.join();
    expect(got == 0xff800000, "D3D11 read %#x after the returned fence, want 0xff800000", got);
  }

  step("a wrapped buffer is the D3D12 buffer, both ways");
  {
    auto buffer12 = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_COPY_DEST);
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(buffer12 && upload && back, "the buffers could not be created"))
      return verdict();
    UINT *words;
    CHECK(upload->Map(0, nullptr, reinterpret_cast<void **>(&words)));
    for (UINT i = 0; i < 64; i++)
      words[i] = 0x12340000 + i;
    upload->Unmap(0, nullptr);
    record([&] { list->CopyBufferRegion(buffer12.Get(), 0, upload.Get(), 0, 256); });

    D3D11_RESOURCE_FLAGS buffer_flags{D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Buffer> buffer11;
    CHECK(on12->CreateWrappedResource(
        buffer12.Get(), &buffer_flags, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST,
        IID_PPV_ARGS(&buffer11)
    ));
    D3D11_BUFFER_DESC desc;
    buffer11->GetDesc(&desc);
    expect(
        desc.ByteWidth == 256 && desc.BindFlags == D3D11_BIND_SHADER_RESOURCE,
        "the wrapped buffer: %u bytes, bindings %#x", desc.ByteWidth, desc.BindFlags
    );
    got = word11(device11.Get(), context.Get(), buffer11.Get());
    expect(got == 0x12340000, "D3D11 read %#x of the queue's upload, want 0x12340000", got);

    UINT replacement[64];
    for (UINT i = 0; i < 64; i++)
      replacement[i] = 0xabcd0000 + i;
    context->UpdateSubresource(buffer11.Get(), 0, nullptr, replacement, 0, 0);
    ID3D11Resource *wrapped_buffer[] = {buffer11.Get()};
    on12->ReleaseWrappedResources(wrapped_buffer, 1);
    context->Flush();
    forget(back.Get());
    record([&] { list->CopyBufferRegion(back.Get(), 0, buffer12.Get(), 0, 256); });
    finish();
    CHECK(back->Map(0, nullptr, reinterpret_cast<void **>(&words)));
    bool same = true;
    for (UINT i = 0; i < 64; i++)
      same &= words[i] == 0xabcd0000 + i;
    UINT first = words[0];
    back->Unmap(0, nullptr);
    expect(same, "the queue read %#x... of D3D11's update, want 0xabcd0000...", first);
  }

  finish();
  return verdict();
}
