// not a test: tests/fuzz.py's harness. each argument (or each line of a file "@list" names) is an HLSL file of a cs_5_0 compute shader "cs" of 64 threads
// that reads its inputs from Buffer<uint> t0 and writes uints to RWStructuredBuffer<uint> u0; beside it, the same
// name with ".in" holds the input words (hex, one a line) and its first line "// outputs: N" says how many words it
// writes. every shader runs on one device, and its output is printed as "<file>: <hex words>", or "<file>: error
// <what>" when it did not compile or run. the inputs come from a buffer so that the compiler cannot fold what the
// shader computes, and the GPU, through Mullion's converter, is what computes it.
#include "d3d11_test.hpp"
#include <fstream>
#include <sstream>

static std::string
read_text(const char *path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

int
main(int argc, char **argv) {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  // "@list" names a file of the files, one a line: more than Windows' command line holds
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++)
    if (argv[i][0] == '@') {
      std::ifstream list(argv[i] + 1);
      for (std::string line; std::getline(list, line);)
        if (!line.empty())
          files.push_back(line.back() == '\r' ? line.substr(0, line.size() - 1) : line);
    } else
      files.push_back(argv[i]);
  std::vector<char *> names;
  for (auto &file : files)
    names.push_back(file.data());
  argc = int(names.size()) + 1;
  names.insert(names.begin(), argv[0]);
  argv = names.data();
  for (int i = 1; i < argc; i++) {
    std::string hlsl = read_text(argv[i]);
    unsigned outputs = 0;
    if (sscanf(hlsl.c_str(), "// outputs: %u", &outputs) != 1 || !outputs) {
      printf("%s: error no outputs line\n", argv[i]);
      continue;
    }
    std::vector<uint32_t> inputs;
    {
      std::ifstream in(std::string(argv[i]) + ".in");
      std::string word;
      while (in >> word)
        inputs.push_back(uint32_t(strtoul(word.c_str(), nullptr, 16)));
    }
    if (inputs.empty()) {
      printf("%s: error no inputs\n", argv[i]);
      continue;
    }
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(hlsl.data(), hlsl.size(), argv[i], nullptr, nullptr, "cs", "cs_5_0", 0, 0, &code, &errors))) {
      printf("%s: error compile %.*s\n", argv[i], errors ? int(errors->GetBufferSize()) : 0,
             errors ? (const char *)errors->GetBufferPointer() : "");
      continue;
    }
    // MULLION_FUZZ_DISASSEMBLE=1: the compiled shader's assembly, on stderr, to tell the compiler's work from the GPU's,
    // and the shader itself beside its file (".dxbc"), for airconv's own command (build/src/airconv/darwin/airconv -S)
    if (getenv("MULLION_FUZZ_DISASSEMBLE")) {
      std::ofstream(std::string(argv[i]) + ".dxbc", std::ios::binary)
          .write((const char *)code->GetBufferPointer(), std::streamsize(code->GetBufferSize()));
      ComPtr<ID3DBlob> text;
      if (SUCCEEDED(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &text)))
        fprintf(stderr, "%s:\n%.*s\n", argv[i], int(text->GetBufferSize()), (const char *)text->GetBufferPointer());
    }
    ComPtr<ID3D11ComputeShader> cs;
    if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs))) {
      printf("%s: error CreateComputeShader\n", argv[i]);
      continue;
    }
    D3D11_SUBRESOURCE_DATA init{inputs.data()};
    D3D11_BUFFER_DESC in_desc{UINT(inputs.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Buffer> in;
    CHECK(device->CreateBuffer(&in_desc, &init, &in));
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{DXGI_FORMAT_R32_UINT, D3D11_SRV_DIMENSION_BUFFER};
    srv_desc.Buffer = {0, UINT(inputs.size())};
    ComPtr<ID3D11ShaderResourceView> srv;
    CHECK(device->CreateShaderResourceView(in.Get(), &srv_desc, &srv));
    D3D11_BUFFER_DESC out_desc{outputs * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                               D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
    std::vector<uint32_t> poison(outputs, 0xdeadbeef);
    D3D11_SUBRESOURCE_DATA out_init{poison.data()};
    ComPtr<ID3D11Buffer> out;
    CHECK(device->CreateBuffer(&out_desc, &out_init, &out));
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{DXGI_FORMAT_UNKNOWN, D3D11_UAV_DIMENSION_BUFFER};
    uav_desc.Buffer = {0, outputs, 0};
    ComPtr<ID3D11UnorderedAccessView> uav;
    CHECK(device->CreateUnorderedAccessView(out.Get(), &uav_desc, &uav));
    context->CSSetShader(cs.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, srv.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    context->Dispatch(1, 1, 1);
    auto words = read(device.Get(), context.Get(), out.Get());
    printf("%s:", argv[i]);
    if (words.size() != outputs)
      printf(" error readback");
    for (auto word : words)
      printf(" %08x", word);
    printf("\n");
    fflush(stdout);
  }
  return 0;
}
