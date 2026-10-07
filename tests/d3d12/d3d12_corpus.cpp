// contract: the shaders an application ships make pipelines. MULLION_CORPUS names a directory of DXIL containers
// (*.dxbc, each named by its hash, as an application's shaders are dumped); without it the test is skipped, since
// no shipped shader is in this tree. every shader goes into pipelines made as an application makes them:
// - a compute shader into a compute pipeline;
// - a vertex shader into a pipeline of its own that writes depth, and into one with a pixel shader whose inputs it
//   has; a pixel shader with a vertex shader that has its inputs; a geometry shader between a vertex shader that
//   has its inputs and a pixel shader whose inputs it has. with MULLION_CORPUS_PAIRS set, every such pair of a
//   vertex and a pixel shader, not one a shader;
// - the root signature has a descriptor table a stage of the ranges the stage's shader binds, which its container
//   lists (the PSV0 part: DirectXShaderCompiler, DxilPipelineStateValidation.h, PSVResourceBindInfo0); the input
//   layout has the vertex shader's input signature (ISG1: DxilContainer.h, DxilProgramSignatureElement), an
//   element a register, of the signature's component type; the targets have the types the pixel shader's output
//   signature (OSG1) gives its SV_Target outputs, and depth when it or no pixel shader writes it.
// a stage has another's inputs when each input of the later stage that an earlier stage must write (all but the
// rasterizer's own: SV_IsFrontFace, SV_SampleIndex, SV_Coverage, SV_InnerCoverage, and SV_PrimitiveID) is an output
// of the earlier one with the name, index, register and type, and no component the earlier does not have: what
// the runtime's linkage check accepts.
// every such pipeline is created (S_OK), but one whose shader asks for waves the device does not have, which
// "D3D12 runtime validation will fail" (DirectX-Specs, HLSL Shader Model 6.6 WaveSize; PSVRuntimeInfo0's expected
// wave lane counts against D3D12_FEATURE_DATA_D3D12_OPTIONS1).
// a failure names the shaders' hashes; the library's own line about it follows the step in the log.
#include "d3d12_test.hpp"
#include <algorithm>

// DXIL's shader kinds (DxilContainer.h, GetVersionShaderType; DXIL::ShaderKind)
enum Kind { Pixel, Vertex, Geometry, Hull, Domain, Compute };
// DxilProgramSigSemantic and DxilProgramSigCompType
enum { PrimitiveID = 7, IsFrontFace = 9, SampleIndex = 10, Target = 64, Depth = 65, Coverage = 66, DepthGE = 67, DepthLE = 68, InnerCoverage = 70 };
enum { UInt32 = 1, SInt32 = 2, Float32 = 3 };

struct Element {
  const char *name;
  UINT index, system, type, reg;
  UINT8 mask;
};

struct Shader {
  std::string hash, code;
  UINT kind = ~0u, lanes[2] = {0, ~0u}, primitive = 0;
  std::vector<Element> in, out;
  std::vector<D3D12_DESCRIPTOR_RANGE> views, samplers;

  // reads the container's parts (DxilContainerHeader, DxilPartHeader)
  bool parse() {
    auto word = [&](size_t at) {
      UINT value = 0;
      if (at + sizeof(value) <= code.size())
        memcpy(&value, code.data() + at, sizeof(value));
      return value;
    };
    if (code.compare(0, 4, "DXBC"))
      return false;
    for (UINT i = 0; i < word(28); i++) {
      size_t part = word(32 + 4 * i), body = part + 8;
      std::string tag = code.substr(part, 4);
      if (tag == "DXIL") {
        kind = word(body) >> 16;
      } else if (tag == "ISG1" || tag == "OSG1") {
        // DxilProgramSignature, then its elements of 32 bytes
        for (UINT e = 0; e < word(body); e++) {
          size_t at = body + word(body + 4) + 32 * e;
          (tag == "ISG1" ? in : out).push_back({code.data() + body + word(at + 4), word(at + 8), word(at + 12), word(at + 16),
                                                word(at + 20), (UINT8)code[at + 24]});
        }
      } else if (tag == "PSV0") {
        // the runtime info's size, the info (a union of 16 bytes, then the wave lane counts), the bindings' count
        // and size, the bindings
        size_t info = body + 4, bindings = info + word(body);
        primitive = word(info);
        lanes[0] = word(info + 16), lanes[1] = word(info + 20);
        for (UINT b = 0; b < word(bindings); b++) {
          size_t at = bindings + 8 + word(bindings + 4) * b;
          // PSVResourceType: a sampler, a constant buffer, three kinds of shader resource, four of unordered access
          const D3D12_DESCRIPTOR_RANGE_TYPE types[] = {{}, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
                                                       D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                                                       D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                                                       D3D12_DESCRIPTOR_RANGE_TYPE_UAV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                                                       D3D12_DESCRIPTOR_RANGE_TYPE_UAV};
          UINT type = word(at), lower = word(at + 8), upper = word(at + 12);
          if (!type || type >= std::size(types))
            return false;
          D3D12_DESCRIPTOR_RANGE range{types[type], upper == ~0u ? ~0u : upper - lower + 1, lower, word(at + 4),
                                       D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
          (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER ? samplers : views).push_back(range);
        }
      }
    }
    return kind != ~0u;
  }

  // whether this stage writes what `later` reads
  bool feeds(const Shader &later) const {
    return std::all_of(later.in.begin(), later.in.end(), [&](const Element &read) {
      if (later.kind == Pixel && (read.system == IsFrontFace || read.system == SampleIndex || read.system == Coverage ||
                                  read.system == InnerCoverage || read.system == PrimitiveID))
        return true;
      return std::any_of(out.begin(), out.end(), [&](const Element &written) {
        return !_stricmp(written.name, read.name) && written.index == read.index && written.reg == read.reg &&
               written.type == read.type && !(read.mask & ~written.mask);
      });
    });
  }
};

int
main(int argc, char **argv) {
  char named[MAX_PATH];
  if (!GetEnvironmentVariableA("MULLION_CORPUS", named, sizeof(named)) || (argc > 1 && !strcmp(argv[1], "dxbc"))) {
    printf("skipped: no directory of DXIL shaders named\n");
    return 77;
  }
  bool pairs = GetEnvironmentVariableA("MULLION_CORPUS_PAIRS", nullptr, 0);
  std::vector<Shader> shaders;
  WIN32_FIND_DATAA found;
  std::string directory = named;
  HANDLE search = FindFirstFileA((directory + "\\*.dxbc").c_str(), &found);
  for (BOOL more = search != INVALID_HANDLE_VALUE; more; more = FindNextFileA(search, &found)) {
    Shader shader;
    shader.hash = std::string(found.cFileName).substr(0, strlen(found.cFileName) - strlen(".dxbc"));
    if (FILE *file = fopen((directory + "\\" + found.cFileName).c_str(), "rb")) {
      shader.code.resize(found.nFileSizeLow);
      shader.code.resize(fread(shader.code.data(), 1, shader.code.size(), file));
      fclose(file);
    }
    step("the container of %s", shader.hash.c_str());
    if (expect(shader.parse(), "%s is no DXIL container this test reads", shader.hash.c_str()))
      shaders.push_back(std::move(shader));
  }
  if (shaders.empty()) {
    printf("failed: no shader in %s\n", named);
    return 1;
  }
  // signature elements point into the containers, which stay where they are from here on
  std::vector<const Shader *> of[Compute + 1];
  for (auto &shader : shaders)
    if (shader.kind <= Compute)
      of[shader.kind].push_back(&shader);

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)));

  unsigned made = 0, refused = 0;
  // a pipeline of the stages given, in pipeline order, some of them none
  auto pipeline = [&](std::initializer_list<const Shader *> given) {
    std::vector<const Shader *> stages;
    for (auto stage : given)
      if (stage)
        stages.push_back(stage);
    std::string names;
    bool waves = true;
    std::vector<D3D12_ROOT_PARAMETER> parameters;
    for (auto stage : stages) {
      const char *kinds[] = {"ps", "vs", "gs", "hs", "ds", "cs"};
      names += std::string(names.empty() ? "" : ", ") + kinds[stage->kind] + " " + stage->hash;
      waves &= stage->lanes[0] <= options1.WaveLaneCountMax && stage->lanes[1] >= options1.WaveLaneCountMin;
      const D3D12_SHADER_VISIBILITY visible[] = {D3D12_SHADER_VISIBILITY_PIXEL, D3D12_SHADER_VISIBILITY_VERTEX,
                                                 D3D12_SHADER_VISIBILITY_GEOMETRY, D3D12_SHADER_VISIBILITY_HULL,
                                                 D3D12_SHADER_VISIBILITY_DOMAIN, D3D12_SHADER_VISIBILITY_ALL};
      for (auto ranges : {&stage->views, &stage->samplers})
        if (!ranges->empty()) {
          D3D12_ROOT_PARAMETER table{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
          table.DescriptorTable = {(UINT)ranges->size(), ranges->data()};
          table.ShaderVisibility = visible[stage->kind];
          parameters.push_back(table);
        }
    }
    step("a pipeline of %s", names.c_str());
    auto rs = root_signature(device.Get(), {(UINT)parameters.size(), parameters.data(), 0, nullptr,
                                            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
    if (!expect(rs, "no root signature of the ranges of %s", names.c_str()))
      return;
    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr;
    auto stage = [&](Kind kind) {
      auto at = std::find_if(stages.begin(), stages.end(), [&](auto s) { return s->kind == kind; });
      return at == stages.end() ? nullptr : *at;
    };
    if (auto cs = stage(Compute)) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(cs->code)};
      hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    } else {
      auto vs = stage(Vertex), gs = stage(Geometry), ps = stage(Pixel);
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs->code)};
      // an element a register of the input signature that is no system value, in formats of 32 bits a component
      const DXGI_FORMAT formats[][4] = {
          {},
          {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32A32_UINT},
          {DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32A32_SINT},
          {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT}};
      auto format = [&](const Element &element, UINT components) {
        return element.type < std::size(formats) ? formats[element.type][components - 1] : DXGI_FORMAT_UNKNOWN;
      };
      std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
      for (auto &element : vs->in)
        if (!element.system)
          layout.push_back({element.name, element.index, format(element, __builtin_popcount(element.mask)), 0,
                            D3D12_APPEND_ALIGNED_ELEMENT});
      desc.InputLayout = {layout.data(), (UINT)layout.size()};
      if (gs)
        desc.GS = bytecode(gs->code);
      bool depth = !ps;
      for (auto &element : ps ? ps->out : std::vector<Element>{}) {
        depth |= element.system == Depth || element.system == DepthGE || element.system == DepthLE;
        if (element.system == Target && element.index < std::size(desc.RTVFormats)) {
          desc.RTVFormats[element.index] = format(element, 4);
          desc.NumRenderTargets = std::max(desc.NumRenderTargets, element.index + 1);
        }
      }
      if (ps)
        desc.PS = bytecode(ps->code);
      if (depth) {
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.DepthStencilState = {TRUE, D3D12_DEPTH_WRITE_MASK_ALL, D3D12_COMPARISON_FUNC_LESS_EQUAL};
      }
      for (auto &target : desc.BlendState.RenderTarget)
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc.SampleMask = ~0u;
      desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      // a geometry shader's input primitive (DXIL::InputPrimitive: 1 a point, 2 a line, 3 a triangle, with adjacency
      // after) is the topology's type; without one, triangles
      desc.PrimitiveTopologyType = !gs || gs->primitive == 3 || gs->primitive == 7 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
                                   : gs->primitive == 1                            ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                                                                                   : D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      desc.SampleDesc = {1, 0};
      hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    }
    if (waves) {
      made += expect(hr == S_OK, "the pipeline of %s: %08lx", names.c_str(), hr);
    } else {
      refused += expect(FAILED(hr), "the pipeline of %s, which needs waves of %u to %u lanes on a device of %u to %u: %08lx",
                        names.c_str(), stages[0]->lanes[0], stages[0]->lanes[1], options1.WaveLaneCountMin,
                        options1.WaveLaneCountMax, hr);
    }
  };

  for (auto cs : of[Compute])
    pipeline({cs});
  std::vector<const Shader *> paired;
  for (auto vs : of[Vertex]) {
    pipeline({vs});
    for (auto ps : of[Pixel])
      if (vs->feeds(*ps)) {
        pipeline({vs, ps});
        paired.push_back(ps);
        if (!pairs)
          break;
      }
  }
  // a pixel shader that had no pipeline above: with the first vertex shader that has its inputs
  for (auto ps : of[Pixel]) {
    auto first = std::find_if(of[Vertex].begin(), of[Vertex].end(), [&](auto vs) { return vs->feeds(*ps); });
    if (first != of[Vertex].end() && std::find(paired.begin(), paired.end(), ps) == paired.end())
      pipeline({*first, ps});
  }
  for (auto gs : of[Geometry])
    for (auto vs : of[Vertex])
      if (vs->feeds(*gs)) {
        for (auto ps : of[Pixel])
          if (gs->feeds(*ps))
            pipeline({vs, gs, ps});
        pipeline({vs, gs});
        break;
      }
  printf("%u pipelines made of %u shaders, %u refused for their waves\n", made, (unsigned)shaders.size(), refused);
  return verdict();
}
