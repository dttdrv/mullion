// contract: CreateBuffer rejects null pSysMem, copies every ByteWidth byte of valid data regardless of either
// pitch, and validates valid inputs with null output without creating a buffer, in Direct3D 10 and 11.
// ID3D11Device::CreateBuffer, ppBuffer: "Set this parameter to NULL to validate the other input parameters
// (S_FALSE indicates a pass)." ID3D10Device::CreateBuffer gives the same rule.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-createbuffer
// D3D11_BUFFER_DESC, ByteWidth: "Size of the buffer in bytes."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_buffer_desc
// D3D11_SUBRESOURCE_DATA: "Pointer to the initialization data." Direct3D 11 Return Codes, E_INVALIDARG:
// "An invalid parameter was passed to the returning function."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_subresource_data
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d11-graphics-reference-returnvalues
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createbuffer
// D3D11_SUBRESOURCE_DATA: "System-memory-slice pitch is only used for 3D texture data as it has no meaning for
// the other resource types." SysMemPitch likewise has no meaning for buffers.
// Wine's test_create_rendertarget_view records the null-pSysMem HRESULT on Windows (d3d11.c and d3d10core.c).
// sizes surround every power of two through two allocation pages, including byte, alignment and allocation
// boundaries. there are no shaders.
#include "d3d11_test.hpp"
#include <d3d10.h>
#include <d3d11_1.h>
#include <limits>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D10Device> device10;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  step("device and Direct3D 10 interface");
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                        nullptr, &context));
  // a D3D11-created device gives its D3D10 interface once a context state exists (d3d11_device_interfaces)
  ComPtr<ID3D11Device1> device1;
  ComPtr<ID3DDeviceContextState> state;
  CHECK(device.As(&device1));
  CHECK(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D10Device), nullptr, &state));
  CHECK(device.As(&device10));
  for (bool via10 : {false, true}) {
    step("Direct3D %u: null output with invalid description", via10 ? 10 : 11);
    D3D11_BUFFER_DESC zero{};
    D3D10_BUFFER_DESC zero10{};
    expect((via10 ? device10->CreateBuffer(nullptr, nullptr, nullptr) :
                    device->CreateBuffer(nullptr, nullptr, nullptr)) == E_INVALIDARG,
           "null description returned success");
    expect((via10 ? device10->CreateBuffer(&zero10, nullptr, nullptr) :
                    device->CreateBuffer(&zero, nullptr, nullptr)) == E_INVALIDARG,
           "zero ByteWidth returned success");
    for (UINT use = D3D11_USAGE_DEFAULT; use <= D3D11_USAGE_STAGING; use++)
      for (UINT boundary = 2; boundary <= 2 * DXMT_PAGE_SIZE; boundary *= 2)
        for (UINT size : {boundary - 1, boundary, boundary + 1})
          for (UINT row_pitch : {0u, ~size})
            for (UINT slice_pitch : {0u, ~size}) {
              auto usage = D3D11_USAGE(use);
              UINT cpu = usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE :
                         usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ : 0;
              UINT bind = usage == D3D11_USAGE_STAGING ? 0 :
                          usage == D3D11_USAGE_DEFAULT ? D3D11_BIND_RENDER_TARGET : D3D11_BIND_VERTEX_BUFFER;
              D3D11_BUFFER_DESC desc{size, usage, bind, cpu};
              D3D10_BUFFER_DESC desc10{size, D3D10_USAGE(use), bind, cpu};
              UINT seed = size + use;
              std::vector<uint8_t> bytes(size);
              for (UINT i = 0; i < size; i++)
                bytes[i] = uint8_t((i + seed) ^ (i >> std::numeric_limits<uint8_t>::digits));
              D3D11_SUBRESOURCE_DATA initial{bytes.data(), row_pitch, slice_pitch};
              D3D11_SUBRESOURCE_DATA invalid{nullptr, row_pitch, slice_pitch};
              D3D10_SUBRESOURCE_DATA initial10{bytes.data(), row_pitch, slice_pitch};
              D3D10_SUBRESOURCE_DATA invalid10{nullptr, row_pitch, slice_pitch};
              ComPtr<ID3D11Buffer> source;
              ComPtr<ID3D10Buffer> source10;
              step("Direct3D %u, usage %u, %u bytes, seed %u, pitches %#x/%#x: initial data",
                   via10 ? 10 : 11, use, size, seed, row_pitch, slice_pitch);
              if (via10) {
                CHECK(device10->CreateBuffer(&desc10, &initial10, &source10));
                CHECK(source10.As(&source));
              } else {
                CHECK(device->CreateBuffer(&desc, &initial, &source));
              }
              if (!expect(!!source, "CreateBuffer returned no buffer"))
                return verdict();
              for (bool output : {true, false}) {
                step("Direct3D %u, usage %u, %u bytes, seed %u, pitches %#x/%#x: null pSysMem, output %u",
                     via10 ? 10 : 11, use, size, seed, row_pitch, slice_pitch, output);
                auto before = device->AddRef();
                device->Release();
                HRESULT hr;
                if (via10) {
                  auto rejected = source10.Get();
                  hr = device10->CreateBuffer(&desc10, &invalid10, output ? &rejected : nullptr);
                  expect(!output || !rejected, "rejected call did not clear its output");
                  if (output && rejected && rejected != source10.Get())
                    rejected->Release();
                } else {
                  auto rejected = source.Get();
                  hr = device->CreateBuffer(&desc, &invalid, output ? &rejected : nullptr);
                  expect(!output || !rejected, "rejected call did not clear its output");
                  if (output && rejected && rejected != source.Get())
                    rejected->Release();
                }
                expect(hr == E_INVALIDARG, "null pSysMem returned %#lx, want E_INVALIDARG", (long)hr);
                auto after = device->AddRef();
                device->Release();
                expect(before == after, "rejected call retained a device reference");
              }
              for (bool supplied : {true, false}) {
                if (!supplied && usage == D3D11_USAGE_IMMUTABLE)
                  continue;
                step("Direct3D %u, usage %u, %u bytes, seed %u, pitches %#x/%#x: null output, initial data %u",
                     via10 ? 10 : 11, use, size, seed, row_pitch, slice_pitch, supplied);
                auto before = device->AddRef();
                device->Release();
                HRESULT hr = via10 ? device10->CreateBuffer(&desc10, supplied ? &initial10 : nullptr, nullptr) :
                                     device->CreateBuffer(&desc, supplied ? &initial : nullptr, nullptr);
                expect(hr == S_FALSE, "valid null-output call returned %#lx, want S_FALSE", (long)hr);
                auto after = device->AddRef();
                device->Release();
                expect(before == after, "validation call retained a device reference");
              }
              step("Direct3D %u, usage %u, %u bytes, seed %u, pitches %#x/%#x: bytes after validation",
                   via10 ? 10 : 11, use, size, seed, row_pitch, slice_pitch);
              D3D11_BUFFER_DESC staging_desc{size, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
              ComPtr<ID3D11Buffer> staging;
              CHECK(device->CreateBuffer(&staging_desc, nullptr, &staging));
              context->CopyResource(staging.Get(), source.Get());
              D3D11_MAPPED_SUBRESOURCE mapped{};
              CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
              expect(!memcmp(mapped.pData, bytes.data(), size), "initial bytes changed or were not copied completely");
              context->Unmap(staging.Get(), 0);
              if (usage != D3D11_USAGE_IMMUTABLE) {
                step("Direct3D %u, usage %u, %u bytes: no initial data", via10 ? 10 : 11, use, size);
                if (via10) {
                  ComPtr<ID3D10Buffer> empty;
                  CHECK(device10->CreateBuffer(&desc10, nullptr, &empty));
                  expect(!!empty, "omitted initial data returned no buffer");
                } else {
                  ComPtr<ID3D11Buffer> empty;
                  CHECK(device->CreateBuffer(&desc, nullptr, &empty));
                  expect(!!empty, "omitted initial data returned no buffer");
                }
              }
            }
  }
  return verdict();
}
