// contract: D3D11.3 8.4.1 and 8.6.1 begin instance fetches at
// "Slot[s].StrideInBytes*StartInstanceLocation"; 8.1.3 says rate 0 means "the instance data should never be
// stepped at all". 8.16: "VertexID starts at 0" for sequential draws and "represents the index value" for indexed
// draws. 8.18: "InstanceID starts at 0 for the first instance". starts select data, not shader counter values.
// 8.19.1: "should any calculation overflow 32-bits, it would wrap". 8.20: an unbound slot reads zero.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// each output record contains both IDs and four independent fetches. every slot, sampled low-rate boundary, zero
// and maximum stride, both index formats, signed base and wrapped starts use the same CPU address calculation.
// a rate with its high and low bits set distinguishes full UINT storage from truncation to 31 bits.
// IAGetVertexBuffers: "Any returned interfaces will have their reference count incremented by one."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-iagetvertexbuffers
// getters expose the selected bindings, an index buffer bound again at another offset included; ClearState restores
// NULL buffers/layout and undefined topology (21.1.1).
#include "d3d11_test.hpp"
#include "../assembly.hpp"
#include <d3d10_1.h>
#include <algorithm>
#include <array>
#include <limits>

static const char hlsl[] = R"hlsl(
struct V { uint2 ids : IDS; uint4 data : DATA; };
V vs(uint a : A, uint b : B, uint c : C, uint d : D, uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  V v;
  v.ids = uint2(vid, iid);
  v.data = uint4(a, b, c, d);
  return v;
}
)hlsl";

int
main() {
  step("compile both shader profiles before creating devices");
  auto code10 = compile(hlsl, "vs", "vs_4_0"), code11 = compile(hlsl, "vs", "vs");
  if (!expect(code10 && code11, "HLSL did not compile"))
    return verdict();
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create10 != nullptr, "no D3D10CreateDevice1"))
    return verdict();
  const UINT word = sizeof(UINT), record_words = 6, record_stride = record_words * word;
  const UINT maximum_stride = D3D11_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES;
  const UINT records = 16, vertex_count = 3, instance_count = 7, sentinel = 0xa5a5a5a5;
  const UINT last = std::numeric_limits<UINT>::max();
  const char *names[] = {"A", "B", "C", "D"};
  const D3D11_SO_DECLARATION_ENTRY entries[] = {{0, "IDS", 0, 0, 2, 0}, {0, "DATA", 0, 0, 4, 0}};
  for (UINT api : {11u, 10u, 101u}) {
    step("create D3D%u device", api);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate, deferred;
    ComPtr<ID3D10Device1> device10;
    if (api == 11) {
      const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
      CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                              &device, nullptr, &immediate));
      CHECK(device->CreateDeferredContext(0, &deferred));
    } else {
      CHECK(create10(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
                     api == 10 ? D3D10_FEATURE_LEVEL_10_0 : D3D10_FEATURE_LEVEL_10_1,
                     D3D10_1_SDK_VERSION, &device10));
      CHECK(device10.As(&device));
      device->GetImmediateContext(&immediate);
    }
    const UINT slots = api == 10 ? D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
                                : D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
    auto code = api == 11 ? code11.Get() : code10.Get();
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11GeometryShader> stream;
    CHECK(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs));
    CHECK(device->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(), entries,
        std::size(entries), &record_stride, 1, api == 11 ? D3D11_SO_NO_RASTERIZED_STREAM : 0, nullptr, &stream));
    ComPtr<ID3D10VertexShader> vs10;
    ComPtr<ID3D10GeometryShader> stream10;
    if (device10) {
      CHECK(vs.As(&vs10));
      CHECK(stream.As(&stream10));
    }
    std::array<std::vector<UINT>, 4> data;
    ComPtr<ID3D11Buffer> buffers[4];
    ComPtr<ID3D10Buffer> buffers10[4];
    for (UINT field = 0; field < std::size(buffers); field++) {
      data[field].resize(records * maximum_stride / word);
      for (UINT i = 0; i < data[field].size(); i++)
        data[field][i] = (field + 1) * UINT(data[field].size()) + i + 1;
      D3D11_BUFFER_DESC desc{UINT(data[field].size() * word), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
      const D3D11_SUBRESOURCE_DATA initial{data[field].data()};
      CHECK(device->CreateBuffer(&desc, &initial, &buffers[field]));
      if (device10)
        CHECK(buffers[field].As(&buffers10[field]));
    }
    const UINT output_words = record_words * (vertex_count * instance_count + 1);
    auto output = buffer(device.Get(), output_words * word, D3D11_BIND_STREAM_OUTPUT, sentinel);
    if (!expect(bool(output), "no stream output buffer"))
      return verdict();
    ComPtr<ID3D10Buffer> output10;
    if (device10)
      CHECK(output.As(&output10));
    struct Configuration { UINT first_slot, stride, rate; bool bound; };
    std::vector<Configuration> configurations;
    for (UINT slot = 0; slot < slots; slot++)
      configurations.push_back({slot, word, 3, true});
    for (UINT stride : {0u, word, maximum_stride - word, maximum_stride})
      for (UINT rate : {0u, 1u, 2u, 3u, (last >> 1) + 2, last})
        for (bool bound : {false, true})
          if (!bound || stride != word || rate != 3)
            configurations.push_back({0, stride, rate, bound});
    for (auto config : configurations) {
      D3D11_INPUT_ELEMENT_DESC elements[4];
      for (UINT field = 0; field < std::size(elements); field++)
        elements[field] = {names[field], 0, DXGI_FORMAT_R32_UINT, (config.first_slot + field) % slots,
            D3D11_APPEND_ALIGNED_ELEMENT, field ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA,
            field == 2 ? 1u : field == 3 ? config.rate : 0u};
      ComPtr<ID3D11InputLayout> layout;
      CHECK(device->CreateInputLayout(elements, std::size(elements), code->GetBufferPointer(), code->GetBufferSize(), &layout));
      ComPtr<ID3D10InputLayout> layout10;
      if (device10)
        CHECK(layout.As(&layout10));
      for (UINT recorded = 0; recorded < (api == 11 ? 2u : 1u); recorded++) {
        auto context = recorded ? deferred.Get() : immediate.Get();
        const UINT calls = api == 11 ? 6 : 4, scenarios = 6, format_count = 2;
        const UINT collected_words = output_words * calls * scenarios * format_count;
        auto collected = buffer(device.Get(), collected_words * word, 0, sentinel);
        if (!expect(bool(collected), "no collected output buffer"))
          return verdict();
        std::vector<UINT> all_wanted;
        ID3D11Buffer *bound[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
        ID3D10Buffer *bound10[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
        UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {}, offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
        for (UINT field = 0; field < std::size(elements); field++) {
          const UINT slot = elements[field].InputSlot;
          bound[slot] = config.bound ? buffers[field].Get() : nullptr;
          bound10[slot] = config.bound ? buffers10[field].Get() : nullptr;
          strides[slot] = field ? word : config.stride;
          offsets[slot] = word;
        }
        if (device10) {
          device10->IASetInputLayout(layout10.Get());
          device10->IASetVertexBuffers(0, slots, bound10, strides, offsets);
          device10->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_POINTLIST);
          device10->VSSetShader(vs10.Get());
          device10->GSSetShader(stream10.Get());
          device10->PSSetShader(nullptr);
        } else {
          context->IASetInputLayout(layout.Get());
          context->IASetVertexBuffers(0, slots, bound, strides, offsets);
          context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
          context->VSSetShader(vs.Get(), nullptr, 0);
          context->GSSetShader(stream.Get(), nullptr, 0);
        }
        step("D3D%u recorded=%u: input layout and vertex-buffer getters", api, recorded);
        if (device10) {
          ComPtr<ID3D10InputLayout> returned;
          device10->IAGetInputLayout(&returned);
          expect(returned.Get() == layout10.Get(), "wrong returned input layout");
        } else {
          ComPtr<ID3D11InputLayout> returned;
          context->IAGetInputLayout(&returned);
          expect(returned.Get() == layout.Get(), "wrong returned input layout");
        }
        for (UINT slot = 0; slot < slots; slot++) {
          UINT stride = last, offset = last;
          IUnknown *selected = device10 ? static_cast<IUnknown *>(bound10[slot]) : bound[slot];
          ULONG before = 0;
          if (selected) { selected->AddRef(); before = selected->Release(); }
          ComPtr<ID3D11Buffer> returned;
          ComPtr<ID3D10Buffer> returned10;
          if (device10)
            device10->IAGetVertexBuffers(slot, 1, &returned10, &stride, &offset);
          else
            context->IAGetVertexBuffers(slot, 1, &returned, &stride, &offset);
          expect(device10 ? returned10.Get() == bound10[slot] : returned.Get() == bound[slot],
                 "slot %u returned a different buffer", slot);
          if (selected) {
            expect(stride == strides[slot] && offset == offsets[slot], "slot %u stride/offset %u/%u, want %u/%u",
                   slot, stride, offset, strides[slot], offsets[slot]);
            selected->AddRef();
            expect(selected->Release() == before + 1, "slot %u getter did not retain its buffer", slot);
          }
          if (!device10) {
            context->IAGetVertexBuffers(slot, 1, nullptr, &stride, nullptr);
            context->IAGetVertexBuffers(slot, 1, nullptr, nullptr, &offset);
            context->IAGetVertexBuffers(slot, 0, nullptr, nullptr, nullptr);
          }
        }
        for (UINT call = 0; call < calls; call++) {
          const bool indexed = call % 2;
          for (DXGI_FORMAT format : {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R32_UINT}) {
            if (!indexed && format == DXGI_FORMAT_R32_UINT)
              continue;
            for (UINT scenario = 0; scenario < scenarios; scenario++) {
              if (scenario == 5 && (!indexed || format != DXGI_FORMAT_R32_UINT))
                continue;
              const UINT count = scenario == 3 ? 0 : scenario == 4 ? last : vertex_count;
              const UINT instances = call < 2 ? 1 : scenario == 4 ? 0 : scenario == 3 ? last : instance_count;
              if (call < 2 && (scenario == 3 || scenario == 4))
                continue;
              const UINT first = scenario == 2 ? last : 2, first_instance = call < 2 ? 0 : scenario == 2 ? last : 3;
              const INT base = scenario == 5 ? std::numeric_limits<INT>::min() : scenario == 0 ? 2 : scenario == 1 ? -1 : 0;
              UINT raw_indices[] = {9, 8, 7, 3, 1, 4};
              if (scenario == 5)
                for (auto &index : raw_indices)
                  index += UINT(std::numeric_limits<INT>::min());
              const UINT index_size = format == DXGI_FORMAT_R32_UINT ? word : sizeof(UINT16);
              std::vector<UINT16> narrow(std::begin(raw_indices), std::end(raw_indices));
              D3D11_BUFFER_DESC ib_desc{UINT(std::size(raw_indices)) * index_size, D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER};
              D3D11_SUBRESOURCE_DATA initial{index_size == word ? (void *)raw_indices : (void *)narrow.data()};
              ComPtr<ID3D11Buffer> ib;
              CHECK(device->CreateBuffer(&ib_desc, &initial, &ib));
              ComPtr<ID3D10Buffer> ib10;
              if (device10)
                CHECK(ib.As(&ib10));
              const UINT index_start = indexed ? 1 : first, index_offset = 2 * index_size;
              // bound at offset 0 first, then again at its offset: the second binding differs in the offset alone
              if (device10) {
                device10->IASetIndexBuffer(ib10.Get(), format, 0);
                device10->IASetIndexBuffer(ib10.Get(), format, index_offset);
              } else {
                context->IASetIndexBuffer(ib.Get(), format, 0);
                context->IASetIndexBuffer(ib.Get(), format, index_offset);
              }
              ComPtr<ID3D11Buffer> returned_ib;
              ComPtr<ID3D10Buffer> returned_ib10;
              DXGI_FORMAT returned_format = DXGI_FORMAT_UNKNOWN;
              UINT returned_offset = 0;
              if (device10)
                device10->IAGetIndexBuffer(&returned_ib10, &returned_format, &returned_offset);
              else
                context->IAGetIndexBuffer(&returned_ib, &returned_format, &returned_offset);
              expect(device10 ? returned_ib10.Get() == ib10.Get() : returned_ib.Get() == ib.Get(), "wrong returned IB");
              expect(returned_format == format && returned_offset == index_offset, "wrong IB format or offset");
              step("D3D%u recorded=%u slot=%u stride=%u rate=%u bound=%u case=%zu call=%u format=%u scenario=%u",
                   api, recorded, config.first_slot, config.stride, config.rate, config.bound,
                   all_wanted.size() / output_words, call, format, scenario);
              std::vector<UINT> want(output_words, sentinel);
              UINT written = 0;
              if (count && instances)
                for (UINT instance = 0; instance < instances; instance++)
                  for (UINT vertex = 0; vertex < count; vertex++) {
                    const UINT id = indexed ? raw_indices[3 + vertex] : vertex;
                    want[written++] = id;
                    want[written++] = instance;
                    for (UINT field = 0; field < std::size(elements); field++) {
                      const UINT rate = elements[field].InstanceDataStepRate;
                      const UINT element = !field ? indexed ? UINT(base) + id : first + vertex
                                                 : first_instance + (rate ? instance / rate : 0);
                      const UINT stride = strides[elements[field].InputSlot], offset = offsets[elements[field].InputSlot];
                      const UINT address = offset + element * stride;
                      const bool inside = address >= offset && uint64_t(address) + word <= data[field].size() * word;
                      want[written++] = config.bound && inside ? data[field][address / word] : 0;
                    }
                  }
              std::vector<UINT> fill(output_words, sentinel);
              context->UpdateSubresource(output.Get(), 0, nullptr, fill.data(), 0, 0);
              const UINT zero = 0;
              if (device10)
                device10->SOSetTargets(1, output10.GetAddressOf(), &zero);
              else
                context->SOSetTargets(1, output.GetAddressOf(), &zero);
              if (call >= 4) {
                const D3D11_DRAW_INSTANCED_INDIRECT_ARGS plain{count, instances, first, first_instance};
                const D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS indices{count, instances, index_start, base, first_instance};
                const UINT padding = word, size = indexed ? sizeof(indices) : sizeof(plain);
                std::vector<unsigned char> bytes(padding + size);
                memcpy(bytes.data() + padding, indexed ? (const void *)&indices : &plain, size);
                D3D11_BUFFER_DESC desc{UINT(bytes.size()), D3D11_USAGE_DEFAULT};
                desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
                initial.pSysMem = bytes.data();
                ComPtr<ID3D11Buffer> arguments;
                CHECK(device->CreateBuffer(&desc, &initial, &arguments));
                if (indexed)
                  context->DrawIndexedInstancedIndirect(arguments.Get(), padding);
                else
                  context->DrawInstancedIndirect(arguments.Get(), padding);
              } else if (device10) {
                if (call == 0) device10->Draw(count, first);
                if (call == 1) device10->DrawIndexed(count, index_start, base);
                if (call == 2) device10->DrawInstanced(count, instances, first, first_instance);
                if (call == 3) device10->DrawIndexedInstanced(count, instances, index_start, base, first_instance);
              } else {
                if (call == 0) context->Draw(count, first);
                if (call == 1) context->DrawIndexed(count, index_start, base);
                if (call == 2) context->DrawInstanced(count, instances, first, first_instance);
                if (call == 3) context->DrawIndexedInstanced(count, instances, index_start, base, first_instance);
              }
              if (device10)
                device10->SOSetTargets(0, nullptr, nullptr);
              else
                context->SOSetTargets(0, nullptr, nullptr);
              context->CopySubresourceRegion(collected.Get(), 0, all_wanted.size() * word, 0, 0, output.Get(), 0, nullptr);
              all_wanted.insert(all_wanted.end(), want.begin(), want.end());
            }
          }
        }
        if (recorded) {
          ComPtr<ID3D11CommandList> commands;
          CHECK(context->FinishCommandList(FALSE, &commands));
          immediate->ExecuteCommandList(commands.Get(), TRUE);
        }
        all_wanted.resize(collected_words, sentinel);
        auto got = read(device.Get(), immediate.Get(), collected.Get());
        step("D3D%u recorded=%u slot=%u stride=%u rate=%u bound=%u: collected output",
             api, recorded, config.first_slot, config.stride, config.rate, config.bound);
        if (expect(got.size() == all_wanted.size(), "read %zu words, want %zu", got.size(), all_wanted.size()))
          for (UINT i = 0; i < all_wanted.size(); i++)
            if (got[i] != all_wanted[i]) {
              expect(false, "case %u word %u is %#x, want %#x", i / output_words, i % output_words, got[i], all_wanted[i]);
              break;
            }
      }
    }
    for (UINT recorded = 0; recorded < (api == 11 ? 2u : 1u); recorded++) {
      auto context = recorded ? deferred.Get() : immediate.Get();
      std::vector<D3D_PRIMITIVE_TOPOLOGY> topologies{D3D_PRIMITIVE_TOPOLOGY_UNDEFINED};
      for (const auto &topology : assembly::cases) topologies.push_back(topology.topology);
      if (api == 11)
        for (UINT points = 1; points <= D3D11_IA_PATCH_MAX_CONTROL_POINT_COUNT; points++)
          topologies.push_back(D3D_PRIMITIVE_TOPOLOGY(D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + points - 1));
      for (auto topology : topologies) {
        D3D_PRIMITIVE_TOPOLOGY returned = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        step("D3D%u recorded=%u topology %u: setter/getter", api, recorded, topology);
        if (device10) { device10->IASetPrimitiveTopology(topology); device10->IAGetPrimitiveTopology(&returned); }
        else { context->IASetPrimitiveTopology(topology); context->IAGetPrimitiveTopology(&returned); }
        expect(returned == topology, "topology %u returned %u", topology, returned);
      }
      if (device10) device10->ClearState();
      else context->ClearState();
      step("D3D%u recorded=%u: clear-state getters for all %u slots", api, recorded, slots);
      std::vector<ID3D11Buffer *> returned(slots);
      std::vector<ID3D10Buffer *> returned10(slots);
      std::vector<UINT> strides(slots, last), offsets(slots, last);
      if (device10) device10->IAGetVertexBuffers(0, slots, returned10.data(), strides.data(), offsets.data());
      else context->IAGetVertexBuffers(0, slots, returned.data(), strides.data(), offsets.data());
      for (UINT slot = 0; slot < slots; slot++) {
        expect(!returned[slot] && !returned10[slot] && !strides[slot] && !offsets[slot], "slot %u not cleared", slot);
        if (returned[slot]) returned[slot]->Release();
        if (returned10[slot]) returned10[slot]->Release();
      }
      ComPtr<ID3D11InputLayout> layout;
      ComPtr<ID3D10InputLayout> layout10;
      ComPtr<ID3D11Buffer> index;
      ComPtr<ID3D10Buffer> index10;
      DXGI_FORMAT format = DXGI_FORMAT_R32_UINT;
      UINT offset = last;
      D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      if (device10) {
        device10->IAGetInputLayout(&layout10);
        device10->IAGetIndexBuffer(&index10, &format, &offset);
        device10->IAGetPrimitiveTopology(&topology);
      } else {
        context->IAGetInputLayout(&layout);
        context->IAGetIndexBuffer(&index, &format, &offset);
        context->IAGetPrimitiveTopology(&topology);
      }
      expect(!layout && !layout10 && !index && !index10 && format == DXGI_FORMAT_UNKNOWN && !offset &&
             topology == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED, "ClearState left an IA binding");
    }
  }
  return verdict();
}
