// contract: Map grants only the access, usage, mode and subresource requested by the API, and a rejected call
// leaves the resource usable. D3D11.3 5.6.1: "Mapping/ locking is done at the Subresource level, instead of the
// Resource level." Its 5.6.1.1 says CPUREAD requires a resource "created to allow read access", CPUWRITE requires
// one "created to allow write access", and DISCARDRESOURCE requires one "created with the DYNAMIC flag".
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#5.6.1%20Mapping
// Microsoft Learn, D3D11_MAP: "The resource must have been created with read and write access" (READ_WRITE), and
// "D3D11_MAP_WRITE_NO_OVERWRITE cannot be used with dynamic textures."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_map
// ID3D11DeviceContext::Map: "Other D3D11_MAP-typed values are not supported for a deferred context."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map
// Direct3D 11 Return Codes: E_INVALIDARG means "An invalid parameter was passed to the returning function."
// D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD applies when the first Map after CreateDeferredContext
// or FinishCommandList per resource was not WRITE_DISCARD; S_OK means "No error occurred."
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d11-graphics-reference-returnvalues
// D3D11_MAPPED_SUBRESOURCE: "the pointer is aligned to 16 bytes" at feature level 10.0 and higher; RowPitch and
// DepthPitch advance rows and depth slices. Padding and unused pitches are not prescribed.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_mapped_subresource
// D3D11_MAP_FLAG: "D3D11_MAP_FLAG_DO_NOT_WAIT cannot be used with D3D11_MAP_WRITE_DISCARD" or NO_OVERWRITE.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_map_flag
// Subresources: "A buffer is defined as a single subresource."
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-resources-subresources
// Wine dlls/d3d11/tests/d3d11.c test_resource_map and test_deferred_context_map record Windows returning null pData
// and unchanged pitches for invalid staging buffer/2D subresources and deferred dynamic-buffer READ. Other failed
// outputs and double-map results are not prescribed here. Every defined word is checked, excluding padding and
// discarded data. Unknown modes and forbidden flag combinations must fail, without assuming their HRESULT.
// https://gitlab.winehq.org/wine/wine/-/blob/wine-11.0/dlls/d3d11/tests/d3d11.c
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <algorithm>
#include <optional>

int
main() {
  const UINT word_bytes = sizeof(UINT);
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate
  ));

  // the page comes from this build's configuration; the texture row comes from a mapped control.
  D3D11_TEXTURE2D_DESC probe_desc{1, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_STAGING, 0,
                                D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE};
  ComPtr<ID3D11Texture2D> probe;
  CHECK(device->CreateTexture2D(&probe_desc, nullptr, &probe));
  D3D11_MAPPED_SUBRESOURCE probe_map{};
  CHECK(immediate->Map(probe.Get(), 0, D3D11_MAP_READ, 0, &probe_map));
  const UINT row_words = probe_map.RowPitch / word_bytes, page_words = DXMT_PAGE_SIZE / word_bytes;
  immediate->Unmap(probe.Get(), 0);
  if (!expect(row_words > 0, "the control has no row pitch"))
    return verdict();

  const D3D11_MAP modes[] = {D3D11_MAP_READ, D3D11_MAP_WRITE, D3D11_MAP_READ_WRITE, D3D11_MAP_WRITE_DISCARD,
                            D3D11_MAP_WRITE_NO_OVERWRITE};
  const struct {
    D3D11_USAGE usage;
    UINT access;
  } descriptions[] = {{D3D11_USAGE_DEFAULT, 0},
                      {D3D11_USAGE_IMMUTABLE, 0},
                      {D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE},
                      {D3D11_USAGE_STAGING, 0},
                      {D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ},
                      {D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_WRITE},
                      {D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE}};
  for (auto dimension : {D3D11_RESOURCE_DIMENSION_BUFFER, D3D11_RESOURCE_DIMENSION_TEXTURE1D,
                         D3D11_RESOURCE_DIMENSION_TEXTURE2D, D3D11_RESOURCE_DIMENSION_TEXTURE3D}) {
    const UINT boundary = dimension == D3D11_RESOURCE_DIMENSION_BUFFER ? page_words : row_words;
    for (UINT width : {std::max(1u, boundary / 2), std::max(1u, boundary - 1), boundary, boundary + 1}) {
      for (auto [usage, access] : descriptions) {
        if (width != boundary && usage != D3D11_USAGE_DYNAMIC &&
            !(usage == D3D11_USAGE_STAGING && access == (D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE)))
          continue;
        const bool dynamic = usage == D3D11_USAGE_DYNAMIC, staging = usage == D3D11_USAGE_STAGING;
        const bool is_buffer = dimension == D3D11_RESOURCE_DIMENSION_BUFFER;
        const UINT height = dimension >= D3D11_RESOURCE_DIMENSION_TEXTURE2D ? 3 : 1;
        const UINT depth = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 2 : 1;
        const UINT mips = is_buffer || dynamic ? 1 : 2;
        const UINT arrays = is_buffer || dynamic || dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1 : 3;
        const UINT subresources = mips * arrays;
        std::vector<std::vector<UINT>> words(subresources);
        std::vector<D3D11_SUBRESOURCE_DATA> initial(subresources);
        for (UINT sub = 0; sub < subresources; sub++) {
          UINT mip = sub % mips, w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
          UINT d = std::max(1u, depth >> mip);
          words[sub].resize(w * h * d);
          for (UINT i = 0; i < words[sub].size(); i++)
            words[sub][i] = sub * width * height * depth + i + 1;
          initial[sub] = {words[sub].data(), w * word_bytes, w * h * word_bytes};
        }
        auto create = [&](D3D11_USAGE use, UINT cpu, const D3D11_SUBRESOURCE_DATA *data) {
          ComPtr<ID3D11Resource> resource;
          HRESULT hr;
          UINT bind = use == D3D11_USAGE_STAGING ? 0 :
                      is_buffer ? D3D11_BIND_VERTEX_BUFFER : D3D11_BIND_SHADER_RESOURCE;
          switch (dimension) {
          case D3D11_RESOURCE_DIMENSION_BUFFER: {
            D3D11_BUFFER_DESC desc{width * word_bytes, use, bind, cpu};
            ComPtr<ID3D11Buffer> buffer;
            hr = device->CreateBuffer(&desc, data, &buffer);
            resource = buffer;
            break;
          }
          case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
            D3D11_TEXTURE1D_DESC desc{width, mips, arrays, DXGI_FORMAT_R32_UINT, use, bind, cpu};
            ComPtr<ID3D11Texture1D> texture;
            hr = device->CreateTexture1D(&desc, data, &texture);
            resource = texture;
            break;
          }
          case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
            D3D11_TEXTURE2D_DESC desc{width, height, mips, arrays, DXGI_FORMAT_R32_UINT, {1, 0}, use, bind, cpu};
            ComPtr<ID3D11Texture2D> texture;
            hr = device->CreateTexture2D(&desc, data, &texture);
            resource = texture;
            break;
          }
          default: {
            D3D11_TEXTURE3D_DESC desc{width, height, depth, mips, DXGI_FORMAT_R32_UINT, use, bind, cpu};
            ComPtr<ID3D11Texture3D> texture;
            hr = device->CreateTexture3D(&desc, data, &texture);
            resource = texture;
            break;
          }
          }
          expect(hr == S_OK && resource, "Create resource returned %#lx", (long)hr);
          return resource;
        };
        step("dimension %u, width %u, usage %u, access %#x", dimension, width, usage, access);
        auto resource = create(usage, access, staging || dynamic ? nullptr : initial.data());
        auto snapshot = create(D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, nullptr);
        if (!resource || !snapshot)
          return verdict();
        if (staging) {
          auto source = create(D3D11_USAGE_IMMUTABLE, 0, initial.data());
          if (!source)
            return verdict();
          immediate->CopyResource(resource.Get(), source.Get());
        }
        D3D11_RESOURCE_DIMENSION got_dimension;
        resource->GetType(&got_dimension);
        expect(got_dimension == dimension, "GetType returned %u, want %u", got_dimension, dimension);

        auto map = [&](ID3D11DeviceContext *context, ID3D11Resource *of, UINT sub, D3D11_MAP mode,
                       std::optional<HRESULT> wanted, UINT flags = 0)
            -> D3D11_MAPPED_SUBRESOURCE {
          snprintf(trace::doing, sizeof(trace::doing),
               "Map %s: dimension %u, width %u, source usage %u/access %#x, %s, sub %u, mode %u, flags %#x",
               of == resource.Get() ? "resource" : "readback", dimension, width, usage, access,
               context == immediate.Get() ? "immediate" : "deferred", sub, mode, flags);
          UINT guard = width * word_bytes;
          D3D11_MAPPED_SUBRESOURCE outputs[3] = {{&guard, guard, ~guard}, {&guard, guard, ~guard},
                                                {&guard, guard, ~guard}};
          auto hr = context->Map(of, sub, mode, flags, &outputs[1]);
          if (wanted)
            expect(hr == *wanted, "Map(%u, %u, %#x) returned %#lx, want %#lx", sub, mode, flags, (long)hr,
                   (long)*wanted);
          else
            expect(FAILED(hr), "Map(%u, %u, %#x) returned %#lx, want failure", sub, mode, flags, (long)hr);
          for (UINT outside : {0u, 2u})
            expect(outputs[outside].pData == &guard && outputs[outside].RowPitch == guard &&
                       outputs[outside].DepthPitch == ~guard,
                   "Map wrote outside its output");
          auto mapped = outputs[1];
          if (FAILED(hr)) {
            // Windows outputs recorded by Wine's test_resource_map and test_deferred_context_map.
            if ((context == immediate.Get() && staging && sub >= subresources &&
                 (is_buffer || dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)) ||
                (context != immediate.Get() && dynamic && is_buffer && mode == D3D11_MAP_READ))
              expect(!mapped.pData && mapped.RowPitch == guard && mapped.DepthPitch == ~guard,
                     "failed Map changed its documented outputs");
            return {};
          }
          if (wanted != S_OK) {
            context->Unmap(of, sub);
            return {};
          }
          const UINT pointer_alignment = 16; // D3D11.3 5.6.1, feature level 10.0 and higher
          UINT mip = sub % mips, w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
          bool usable = expect(mapped.pData && mapped.pData != &guard &&
                                   reinterpret_cast<uintptr_t>(mapped.pData) % pointer_alignment == 0,
                               "Map did not return aligned storage");
          if (dimension >= D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            usable &= expect(mapped.RowPitch >= w * word_bytes, "Map returned a short row pitch");
          if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
            usable &= expect(mapped.DepthPitch >= mapped.RowPitch * h, "Map returned a short depth pitch");
          if (!usable) {
            context->Unmap(of, sub);
            return {};
          }
          return mapped;
        };
        auto data = [&](D3D11_MAPPED_SUBRESOURCE mapped, UINT sub, bool write) {
          UINT mip = sub % mips, w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
          UINT d = std::max(1u, depth >> mip);
          bool same = true;
          for (UINT z = 0; z < d; z++) {
            for (UINT y = 0; y < h; y++) {
              auto row = static_cast<char *>(mapped.pData) + z * mapped.DepthPitch + y * mapped.RowPitch;
              auto expected = words[sub].data() + (z * h + y) * w;
              if (write)
                memcpy(row, expected, w * word_bytes);
              else
                same &= memcmp(row, expected, w * word_bytes) == 0;
            }
          }
          expect(same, "subresource %u does not contain the written words", sub);
        };
        auto readback = [&] {
          immediate->CopyResource(snapshot.Get(), resource.Get());
          for (UINT sub = 0; sub < subresources; sub++) {
            for (UINT reuse = 0; reuse < 2; reuse++) {
              auto mapped = map(immediate.Get(), snapshot.Get(), sub, D3D11_MAP_READ, S_OK);
              if (!mapped.pData)
                return false;
              data(mapped, sub, false);
              immediate->Unmap(snapshot.Get(), sub);
            }
          }
          return true;
        };
        for (bool deferred : {false, true}) {
          ComPtr<ID3D11DeviceContext> context = immediate;
          if (deferred)
            CHECK(device->CreateDeferredContext(0, &context));
          step("dimension %u, width %u, usage %u, access %#x, %s", dimension, width, usage, access,
               deferred ? "deferred" : "immediate");
          if (deferred && dynamic && is_buffer)
            map(context.Get(), resource.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE,
                D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD);
          auto observe = [&] {
            if (deferred) {
              ComPtr<ID3D11CommandList> list;
              auto hr = context->FinishCommandList(FALSE, &list);
              if (!expect(hr == S_OK && list, "FinishCommandList returned %#lx", (long)hr))
                return false;
              immediate->ExecuteCommandList(list.Get(), FALSE);
            }
            return readback();
          };
          auto reuse = [&] {
            if (dynamic || (staging && access)) {
              auto control = dynamic ? D3D11_MAP_WRITE_DISCARD :
                             access & D3D11_CPU_ACCESS_READ ? D3D11_MAP_READ : D3D11_MAP_WRITE;
              auto on = dynamic ? context.Get() : immediate.Get();
              for (UINT sub = 0; sub < subresources; sub++) {
                auto mapped = map(on, resource.Get(), sub, control, S_OK);
                if (mapped.pData) {
                  data(mapped, sub, dynamic || !(access & D3D11_CPU_ACCESS_READ));
                  on->Unmap(resource.Get(), sub);
                }
              }
            }
          };
          // D3D11.3 5.6.9: "DISCARD means that the system may discard the entire contents of the destination memory
          // outside the region being updated." A following NO_OVERWRITE update must preserve the other bytes.
          if (usage == D3D11_USAGE_DEFAULT && is_buffer) {
            ComPtr<ID3D11DeviceContext1> context1;
            CHECK(context.As(&context1));
            for (UINT flags : {0u, UINT(D3D11_COPY_DISCARD)}) {
              for (auto &word : words[0])
                word += words[0].size();
              context1->UpdateSubresource1(resource.Get(), 0, nullptr, words[0].data(), 0, 0, flags);
              if (flags == D3D11_COPY_DISCARD) {
                D3D11_BOX box{width / 2 * word_bytes, 0, 0, width * word_bytes, 1, 1};
                for (UINT i = width / 2; i < width; i++)
                  words[0][i] += width;
                context1->UpdateSubresource1(resource.Get(), 0, &box, words[0].data() + width / 2, 0, 0,
                                            D3D11_COPY_NO_OVERWRITE);
              }
              if (!observe())
                return verdict();
            }
          }
          reuse();
          for (auto mode : modes) {
            // Wine check_resource_cpu_access records S_OK/E_INVALIDARG for these usage/access combinations,
            // including E_INVALIDARG for plain WRITE on DYNAMIC. test_deferred_context_map records the dynamic
            // deferred modes; Learn Map forbids other deferred modes but does not assign their HRESULT.
            bool allowed = dynamic ? mode == D3D11_MAP_WRITE_DISCARD ||
                                         (is_buffer && mode == D3D11_MAP_WRITE_NO_OVERWRITE) :
                           staging && !deferred &&
                               ((mode == D3D11_MAP_READ && (access & D3D11_CPU_ACCESS_READ)) ||
                                (mode == D3D11_MAP_WRITE && (access & D3D11_CPU_ACCESS_WRITE)) ||
                                (mode == D3D11_MAP_READ_WRITE &&
                                 access == (D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE)));
            std::optional<HRESULT> wanted = allowed ? S_OK : E_INVALIDARG;
            if (!allowed && ((deferred && !(dynamic && is_buffer)) ||
                             (!dynamic && mode == D3D11_MAP_WRITE_NO_OVERWRITE)))
              wanted.reset();
            auto mapped = map(context.Get(), resource.Get(), 0, mode, wanted);
            if (mapped.pData) {
              if (mode == D3D11_MAP_READ_WRITE)
                data(mapped, 0, false);
              if (mode != D3D11_MAP_READ)
                for (auto &word : words[0])
                  word += words[0].size();
              data(mapped, 0, mode != D3D11_MAP_READ);
              context->Unmap(resource.Get(), 0);
            }
            if (!observe())
              return verdict();
            reuse();
          }
          for (auto mode : {static_cast<D3D11_MAP>(0), static_cast<D3D11_MAP>(D3D11_MAP_WRITE_NO_OVERWRITE + 1)}) {
            // Learn D3D11_MAP enumerates the permitted modes, without assigning an error code to other values.
            map(context.Get(), resource.Get(), 0, mode, std::nullopt);
            if (!observe())
              return verdict();
            reuse();
          }
          if (dynamic || (staging && access && !deferred)) {
            auto control = dynamic ? D3D11_MAP_WRITE_DISCARD :
                           access & D3D11_CPU_ACCESS_READ ? D3D11_MAP_READ : D3D11_MAP_WRITE;
            for (UINT sub : {subresources, subresources + 1}) {
              // Wine test_resource_map records E_INVALIDARG for invalid staging buffer, 2D and 3D subresources.
              map(context.Get(), resource.Get(), sub, control,
                  staging && control == D3D11_MAP_READ && dimension != D3D11_RESOURCE_DIMENSION_TEXTURE1D
                      ? std::optional<HRESULT>(E_INVALIDARG) : std::nullopt);
              if (!observe())
                return verdict();
              reuse();
            }
          }
          if (dynamic) {
            for (auto mode : {D3D11_MAP_WRITE_DISCARD, D3D11_MAP_WRITE_NO_OVERWRITE}) {
              if (mode == D3D11_MAP_WRITE_NO_OVERWRITE && !is_buffer)
                continue;
              // Learn D3D11_MAP_FLAG forbids this combination, without assigning an HRESULT.
              map(context.Get(), resource.Get(), 0, mode, std::nullopt, D3D11_MAP_FLAG_DO_NOT_WAIT);
              if (!observe())
                return verdict();
              reuse();
            }
          }
          if (deferred) {
            ComPtr<ID3D11CommandList> list;
            CHECK(context->FinishCommandList(FALSE, &list));
            immediate->ExecuteCommandList(list.Get(), FALSE);
            if (dynamic && is_buffer) {
              map(context.Get(), resource.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE,
                  D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD);
              reuse();
              CHECK(context->FinishCommandList(TRUE, &list));
              immediate->ExecuteCommandList(list.Get(), FALSE);
              map(context.Get(), resource.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE,
                  D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD);
              reuse();
              if (!observe())
                return verdict();
            }
          }
          step("readback: dimension %u, width %u, usage %u, access %#x, %s", dimension, width, usage, access,
               deferred ? "deferred" : "immediate");
          if (!readback())
            return verdict();
        }
      }
    }
  }
  return verdict();
}
