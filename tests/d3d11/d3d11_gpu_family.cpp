// contract: what the bridge answers of a device is what Apple's Metal feature set tables give its GPU family
// (https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf, "Metal feature availability by GPU family", "GPU
// implementation limits by family" and "Texture capabilities by pixel format"): the family of the GPU, or the one
// MULLION_GPU_FAMILY has the bridge answer as. each row below is a row of those tables: the Apple family an answer
// is given from, and the macOS that has it. so an emulated family answers as its GPUs would, and a run on such a GPU
// shows where the tables and the GPU differ.
// three answers the tables leave to the device before a family ("some GPU devices in the Apple7 and Apple8 families"
// have BC formats and the filtering and resolve of 32-bit float formats, some in Apple7 placement sparse resources)
// are held from that family on, and printed before it.
// what a stage reaches of samplers through argument buffers is 1024 before Apple9 as an M1 Pro reports it
// (winemetal_unix.c); the tables have 996 there.
#include "d3d11_test.hpp"
#include "../../src/winemetal/Metal.hpp"
#include <map>
#include <sstream>

struct Row {
  const char *name;
  unsigned first; // the Apple family the answer is 1 from
  unsigned macos; // and the macOS it needs
  bool some_before;
};

static const Row rows[] = {
    {"supportsFamily.apple7", 7},
    {"supportsFamily.apple8", 8},
    {"supportsFamily.apple9", 9},
    {"supportsFamily.apple10", 10},
    {"supportsFamily.apple11", 11},
    {"supportsFamily.metal3", 7},
    {"supportsFamily.metal4", 7, 26},
    // Maximum sample count in render passes with MSAA: 4, and 8 from Apple10
    {"supportsTextureSampleCount.1", 7},
    {"supportsTextureSampleCount.2", 7},
    {"supportsTextureSampleCount.4", 7},
    {"supportsTextureSampleCount.8", 10},
    {"supportsTextureSampleCount.16", ~0u},
    // Sampler minimum and maximum reduction
    {"supportsSamplerReduction", 10, 26},
    // Access to pre-raster per-vertex values, which the bridge is asked for with the barycentric coordinates
    {"supportsShaderBarycentricCoordinates", 10, 26},
    // MetalFX spatial, temporal and denoised upscaling
    {"supportsFXSpatialScaler", 7},
    {"supportsFXTemporalScaler", 7},
    {"supportsFXDenoisedScaler.metal3", 9, 26},
    {"supportsFXDenoisedScaler.metal4", 9, 26},
    // BC pixel formats, 32-bit float formats' Filter and Resolve, placement sparse textures
    {"supportsBCTextureCompression", 9, 0, true},
    {"supports32BitFloatFiltering", 9, 0, true},
    {"supports32BitMSAA", 9, 0, true},
    {"supportsPlacementSparse", 8, 26, true},
};

int
main() {
  auto pool = WMT::MakeAutoreleasePool();
  auto devices = WMT::CopyAllDevices();
  CHECK(devices && devices.count() ? S_OK : E_FAIL);
  auto device = devices.object(0);
  std::string text(WMTDeviceReport(device.handle, nullptr, 0), 0);
  CHECK(text.size() && WMTDeviceReport(device.handle, text.data(), text.size()) == text.size() ? S_OK : E_FAIL);
  printf("%s", text.c_str());
  std::map<std::string, uint64_t> answers;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);)
    if (auto at = line.find('='); expect(at != line.npos, "a line without a value: %s", line.c_str()))
      answers[line.substr(0, at)] = strtoull(line.c_str() + at + 1, nullptr, 10);
  uint64_t macos = 0, minor = 0;
  WMTGetOSVersion(&macos, &minor, nullptr);
  auto answer = [&](const std::string &name) {
    auto found = answers.find(name);
    return expect(found != answers.end(), "no answer %s", name.c_str()) ? found->second : ~0ull;
  };

  // the family answered as: the one asked for, which no answer may pass, or the GPU's own, the last it says it is
  unsigned family = answer("family"), last = 0;
  for (unsigned n = 7; n <= 11; n++)
    if (answer("supportsFamily.apple" + std::to_string(n)) == 1)
      last = n;
  if (!expect(last >= 7, "the device is of no Apple family from 7 on, which the tables do not have"))
    return verdict();
  if (family)
    expect(last == family, "answers as Apple%u, asked to answer as Apple%u", last, family);
  family = last;
  printf("held to Apple%u on macOS %llu.%llu\n", family, macos, minor);

  for (auto &row : rows) {
    uint64_t have = answer(row.name);
    // placement sparse resources are macOS 26.4's
    bool os = macos >= row.macos && (std::string(row.name) != "supportsPlacementSparse" || macos > 26 || minor >= 4);
    bool want = family >= row.first && os;
    if (row.some_before && family < row.first)
      printf("%s=%llu is the device's: the tables have it from Apple%u\n", row.name, have, row.first);
    else
      expect(have == want, "%s is %llu, Apple%u on macOS %llu has %d (from Apple%u, macOS %u)", row.name, have, family,
             macos, want, row.first, row.macos);
  }
  // Query texture LOD: "some GPU devices in the Apple7 family", every one from Apple8. converted shaders query on
  // every GPU, so a GPU that says no is one they do not compile for
  expect(answer("device.supportsQueryTextureLOD") == 1, "the GPU does not query a texture's LOD");
  // Maximum total threadgroup memory allocation and Maximum threads per threadgroup
  expect(answer("maxThreadgroupMemoryLength") == 32 * 1024, "threadgroup memory is not 32 KB");
  expect(answer("maxThreadsPerThreadgroup") == 1024, "a threadgroup is not 1024 threads");
  // Maximum number of samplers you can access, per stage, from an argument buffer
  uint64_t samplers = answer("maxArgumentBufferSamplerCount");
  expect(family >= 9 ? samplers >= 500000 : samplers == 1024, "%llu samplers through argument buffers on Apple%u",
         samplers, family);
  return verdict();
}
