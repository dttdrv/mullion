// contract: advertised vertex formats decode every component, including the defaults, before the vertex shader.
// "The starting n-bit value is converted to float (0.0f, 1.0f, 2.0f, etc.) and then divided by (2n-1)."
// (D3D11.3 3.2.3.5). "the return is 0 in all non-missing components of the format" (8.19.2); missing channels
// use 19.1.3.3 defaults. the CPU reference also follows signed/float conversions (3.2) and component order
// (19.1.3.1). stream output preserves the shader's bits. tightly packed and padded elements/bindings exercise
// every generated encoding, then one vertex past the buffer, on immediate/deferred contexts and FL10/10.1/11.
// all shaders compile before device creation; SM4 and SM5 variants share the same expected bytes.
#include "d3d11_test.hpp"
#include "../depth_formats.hpp"
#include "../format_values.hpp"

int
main() {
  using namespace format_values;
  const char *profiles[] = {"vs_4_0", "vs_5_0"};
  ComPtr<ID3DBlob> codes[std::size(profiles)][3];
  for (UINT profile = 0; profile < std::size(profiles); profile++)
    for (UINT kind = 0; kind < std::size(codes[profile]); kind++) {
      codes[profile][kind] = compile(vertices_hlsl, "vs", profiles[profile], defines(kind, 0));
      if (!expect(bool(codes[profile][kind]), "vertex HLSL did not compile"))
        return verdict();
    }
  UINT tested = 0;
  for (auto level : {D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_11_0}) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate, deferred;
    CHECK(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate
    ));
    CHECK(device->CreateDeferredContext(0, &deferred));
    for (const auto &f : formats()) {
      UINT caps = 0;
      HRESULT support = device->CheckFormatSupport(f.id, &caps);
      step("FL%#x %s vertex capability", level, f.name.c_str());
      if (level == D3D_FEATURE_LEVEL_11_0 && (required_vertex(f) || f.id == DXGI_FORMAT_B8G8R8A8_UNORM))
        expect(SUCCEEDED(support) && (caps & D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER), "vertex capability was lost");
      if (FAILED(support) || !(caps & D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER))
        continue;
      auto bytes = inputs(f);
      const UINT count = UINT(bytes.size() / f.bytes), kind = f.shader_kind();
      const UINT output_bytes = (count + 2) * sizeof(Pixel);
      std::vector<Pixel> poison(count + 2, Pixel{~0u, ~0u, ~0u, ~0u});
      std::vector<uint8_t> zero(f.bytes);
      for (UINT profile = 0; profile < std::size(profiles); profile++) {
        if (level < D3D_FEATURE_LEVEL_11_0 && profile)
          continue;
        auto code = codes[profile][kind].Get();
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11GeometryShader> stream;
        CHECK(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs));
        const D3D11_SO_DECLARATION_ENTRY entry{0, "VALUE", 0, 0, UINT8(std::tuple_size_v<Pixel>), 0};
        const UINT output_stride = sizeof(Pixel);
        // below 11_0 stream 0 is the one rasterized, and with no pixel shader nothing is (Wine's
        // test_fl10_stream_output_desc)
        CHECK(device->CreateGeometryShaderWithStreamOutput(
            code->GetBufferPointer(), code->GetBufferSize(), &entry, 1, &output_stride, 1,
            level < D3D_FEATURE_LEVEL_11_0 ? 0 : D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &stream
        ));
        for (UINT padding : {0u, UINT(sizeof(Pixel))}) {
          const UINT stride = f.bytes + 2 * padding, offset = padding;
          std::vector<uint8_t> interleaved(offset + count * stride, uint8_t(~0u));
          for (UINT i = 0; i < count; i++)
            memcpy(interleaved.data() + offset + i * stride + padding, bytes.data() + i * f.bytes, f.bytes);
          D3D11_BUFFER_DESC bd{UINT(interleaved.size()), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
          D3D11_SUBRESOURCE_DATA data{interleaved.data()};
          ComPtr<ID3D11Buffer> vertices, output;
          CHECK(device->CreateBuffer(&bd, &data, &vertices));
          bd = {output_bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT};
          data.pSysMem = poison.data();
          CHECK(device->CreateBuffer(&bd, &data, &output));
          const D3D11_INPUT_ELEMENT_DESC element{"DATA", 0, f.id, 0, padding, D3D11_INPUT_PER_VERTEX_DATA, 0};
          ComPtr<ID3D11InputLayout> layout;
          CHECK(device->CreateInputLayout(&element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout));
          for (bool listed : {false, true}) {
            step(
                "FL%#x %s %s, stride %u, offsets %u, %s", level, f.name.c_str(), profiles[profile], stride, padding,
                listed ? "deferred" : "immediate"
            );
            auto context = listed ? deferred.Get() : immediate.Get();
            context->ClearState();
            context->UpdateSubresource(output.Get(), 0, nullptr, poison.data(), 0, 0);
            context->IASetInputLayout(layout.Get());
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            context->IASetVertexBuffers(0, 1, vertices.GetAddressOf(), &stride, &offset);
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->GSSetShader(stream.Get(), nullptr, 0);
            const UINT start = 0;
            context->SOSetTargets(1, output.GetAddressOf(), &start);
            context->Draw(count + 1, 0);
            context->ClearState();
            if (listed) {
              ComPtr<ID3D11CommandList> commands;
              CHECK(deferred->FinishCommandList(FALSE, &commands));
              immediate->ExecuteCommandList(commands.Get(), FALSE);
            }
            auto got = read(device.Get(), immediate.Get(), output.Get());
            if (!expect(got.size() * sizeof(uint32_t) == output_bytes, "vertex readback incomplete"))
              return verdict();
            for (UINT i = 0; i <= count; i++) {
              Pixel value;
              memcpy(value.data(), got.data() + i * value.size(), sizeof(value));
              if (!expect(
                      loaded(f, i == count ? zero.data() : bytes.data() + i * f.bytes, value),
                      "vertex %u conversion or buffer-end default differs", i
                  ))
                return verdict();
            }
            if (!expect(
                    !memcmp(got.data() + (count + 1) * std::tuple_size_v<Pixel>, poison.back().data(), sizeof(Pixel)),
                    "stream output wrote beyond the draw"
                ))
              return verdict();
            tested++;
          }
        }
      }
    }
  }
  expect(tested != 0, "no vertex formats were exercised");
  return verdict();
}
