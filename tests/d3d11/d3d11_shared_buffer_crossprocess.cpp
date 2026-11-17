// contract: with MULLION_METALVR set, a default buffer created with D3D11_RESOURCE_MISC_SHARED has a shared handle,
// and a device that opens the handle, in this process or another, reads and writes the same bytes. Microsoft Learn,
// IDXGIResource::GetSharedHandle: "You can also marshal this handle to another process to share a resource with a
// device in another process", and "If no resource objects exist on any devices that refer to this resource, the
// handle is no longer valid." A VR compositor and its applications pass a frame's bookkeeping this way (SteamVR's
// shared constant buffers). Without MULLION_METALVR the buffer has no shared handle, as before (--off).
// the child of this test is the test itself, as in d3d11_shared_texture_crossprocess.cpp. there are no shaders.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiresource-getsharedhandle
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-opensharedresource
#include "d3d11_test.hpp"
#include <cstdlib>
#include <string>
#include <vector>

// a constant buffer's size, and not a multiple of a page
static const UINT bytes = 208;
static const DWORD process_ms = 60000;

struct a_device {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;

  bool
  open() {
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                       &device, nullptr, &context));
  }
};

// one byte pattern per turn, the same in both processes
static std::vector<BYTE>
pattern(unsigned turn) {
  std::vector<BYTE> out(bytes);
  for (UINT i = 0; i < bytes; i++)
    out[i] = BYTE((turn * 0x3b + i * 5 + 1) & 0xff);
  return out;
}

// the commands before this have run: a raw share carries no ordering of its own
static bool
drained(a_device &d) {
  const D3D11_QUERY_DESC event_desc{D3D11_QUERY_EVENT};
  ComPtr<ID3D11Query> completed;
  if (FAILED(d.device->CreateQuery(&event_desc, &completed)))
    return false;
  d.context->End(completed.Get());
  d.context->Flush();
  BOOL done = FALSE;
  HRESULT hr;
  while ((hr = d.context->GetData(completed.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
    Sleep(0);
  return hr == S_OK && done;
}

// the buffer's bytes, read back through a staging copy
static std::vector<BYTE>
contents(a_device &d, ID3D11Buffer *buffer) {
  D3D11_BUFFER_DESC desc;
  buffer->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Buffer> staging;
  if (FAILED(d.device->CreateBuffer(&desc, nullptr, &staging)))
    return {};
  d.context->CopyResource(staging.Get(), buffer);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(d.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    return {};
  std::vector<BYTE> out(bytes);
  memcpy(out.data(), mapped.pData, bytes);
  d.context->Unmap(staging.Get(), 0);
  return out;
}

static HANDLE
handle_of(ID3D11Buffer *buffer, HRESULT *result) {
  ComPtr<IDXGIResource> resource;
  HANDLE shared = nullptr;
  *result = buffer->QueryInterface(IID_PPV_ARGS(&resource));
  if (SUCCEEDED(*result))
    *result = resource->GetSharedHandle(&shared);
  return shared;
}

static const D3D11_BUFFER_DESC shared_desc{bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0,
                                           D3D11_RESOURCE_MISC_SHARED, 0};

// another process: reads what the parent wrote, and writes the next turn
static int
child(char **argv) {
  HANDLE shared = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[2], nullptr, 10)));
  a_device d;
  if (!expect(d.open(), "the child's device"))
    return verdict();
  ComPtr<ID3D11Buffer> buffer;
  if (!expect(SUCCEEDED(d.device->OpenSharedResource(shared, IID_PPV_ARGS(&buffer))), "the child opens the buffer"))
    return verdict();
  D3D11_BUFFER_DESC desc;
  buffer->GetDesc(&desc);
  expect(desc.ByteWidth == bytes && desc.BindFlags == D3D11_BIND_CONSTANT_BUFFER &&
             (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED),
         "the opened buffer's description: %u bytes, bind %x, misc %x", desc.ByteWidth, desc.BindFlags, desc.MiscFlags);
  expect(contents(d, buffer.Get()) == pattern(2), "the child reads the parent's second turn");
  d.context->UpdateSubresource(buffer.Get(), 0, nullptr, pattern(3).data(), 0, 0);
  expect(drained(d), "the child's write ran");
  return verdict();
}

// without the switch a shared buffer is made as any other and has no handle to share
static int
off() {
  a_device d;
  if (!expect(d.open(), "a device"))
    return verdict();
  ComPtr<ID3D11Buffer> buffer;
  CHECK(d.device->CreateBuffer(&shared_desc, nullptr, &buffer));
  HRESULT hr;
  handle_of(buffer.Get(), &hr);
  expect(FAILED(hr), "GetSharedHandle without MULLION_METALVR: %08lx", static_cast<unsigned long>(hr));
  return verdict();
}

int
main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "--off"))
    return off();
  // what a VR runtime sets before it makes a device; the child inherits it
  SetEnvironmentVariableA("MULLION_METALVR", "1");
  if (argc >= 3 && !strcmp(argv[1], "--child"))
    return child(argv);

  a_device first, second;
  if (!expect(first.open() && second.open(), "two devices"))
    return verdict();
  const auto initial = pattern(0);
  const D3D11_SUBRESOURCE_DATA data{initial.data(), 0, 0};
  ComPtr<ID3D11Buffer> buffer;
  CHECK(first.device->CreateBuffer(&shared_desc, &data, &buffer));
  HRESULT hr;
  HANDLE shared = handle_of(buffer.Get(), &hr);
  if (!expect(SUCCEEDED(hr) && shared, "GetSharedHandle: %08lx", static_cast<unsigned long>(hr)))
    return verdict();
  expect(contents(first, buffer.Get()) == initial, "the buffer's initial data");

  // a second device of this process
  ComPtr<ID3D11Buffer> opened;
  if (!expect(SUCCEEDED(second.device->OpenSharedResource(shared, IID_PPV_ARGS(&opened))), "a second device opens it"))
    return verdict();
  expect(contents(second, opened.Get()) == initial, "the second device reads the initial data");
  second.context->UpdateSubresource(opened.Get(), 0, nullptr, pattern(1).data(), 0, 0);
  expect(drained(second), "the second device's write ran");
  expect(contents(first, buffer.Get()) == pattern(1), "the first device reads the second's write");
  opened.Reset();

  // another process
  first.context->UpdateSubresource(buffer.Get(), 0, nullptr, pattern(2).data(), 0, 0);
  expect(drained(first), "the first device's write ran");
  char executable[MAX_PATH];
  GetModuleFileNameA(nullptr, executable, sizeof(executable));
  std::string command =
      "\"" + std::string(executable) + "\" --child " + std::to_string(reinterpret_cast<uintptr_t>(shared));
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!expect(!!CreateProcessA(executable, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                               &process),
              "CreateProcess: %lu", GetLastError()))
    return verdict();
  CloseHandle(process.hThread);
  if (!expect(WaitForSingleObject(process.hProcess, process_ms) == WAIT_OBJECT_0, "the child did not exit")) {
    TerminateProcess(process.hProcess, 1);
    return verdict();
  }
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  expect(code == 0, "the child exited %lu", code);
  expect(contents(first, buffer.Get()) == pattern(3), "the first device reads the child's write");

  // the handle dies with the last buffer of the share
  buffer.Reset();
  first.context->Flush();
  ComPtr<ID3D11Buffer> late;
  expect(FAILED(second.device->OpenSharedResource(shared, IID_PPV_ARGS(&late))),
         "the handle of a released share still opens");
  return verdict();
}
