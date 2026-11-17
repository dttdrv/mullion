// Native control for the zero-row shader-validation report, independent of Wine and AIR conversion.
// Default: the original access without ICB support and guarded reads with ICB support both return the expected data.
// --reproduce: enable ICB support on the original access; run with MTL_SHADER_VALIDATION=1 to see the report.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <string>
#include <string_view>

static int run(bool icb, bool guarded) {
  @autoreleasepool {
    auto device = MTLCreateSystemDefaultDevice();
    if (!device || ![device supportsFamily:MTLGPUFamilyMetal4]) {
      puts("skipped: a Metal 4 device is required");
      return 77;
    }
    std::string source = R"(
      #include <metal_stdlib>
      using namespace metal;
      constant uint zero_row[4] = {0, 0, 0, 0};
      struct Args { constant uint *rows; };
      kernel void zero_test(constant Args &args [[buffer(0)]], device uint *out [[buffer(1)]],
                            uint tid [[thread_position_in_grid]]) {
    )";
    source += guarded ? "out[tid] = tid < 16 ? args.rows[4 * tid] : 0; }"
                      : "constant uint *p = tid < 16 ? args.rows + 4 * tid : zero_row; out[tid] = *p; }";
    NSError *error = nil;
    auto options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion4_0;
    auto library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                       options:options error:&error];
    if (!library) { NSLog(@"library: %@", error); return 1; }
    auto descriptor = [MTLComputePipelineDescriptor new];
    descriptor.computeFunction = [library newFunctionWithName:@"zero_test"];
    descriptor.supportIndirectCommandBuffers = icb;
    auto pipeline = [device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionNone
                                                     reflection:nil error:&error];
    if (!pipeline) { NSLog(@"pipeline: %@", error); return 1; }
    auto input = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    auto output = [device newBufferWithLength:128 options:MTLResourceStorageModeShared];
    auto arguments = [device newBufferWithLength:sizeof(uint64_t) options:MTLResourceStorageModeShared];
    if (!input || !output || !arguments) return 1;
    for (unsigned i = 0; i < 64; ++i) static_cast<unsigned *>(input.contents)[i] = i + 1;
    for (unsigned i = 0; i < 32; ++i) static_cast<unsigned *>(output.contents)[i] = ~0u;
    *static_cast<uint64_t *>(arguments.contents) = input.gpuAddress;
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder useResource:input usage:MTLResourceUsageRead];
    [encoder setBuffer:arguments offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) { NSLog(@"command: %@", command.error); return 1; }
    unsigned wrong = 0;
    for (unsigned i = 0; i < 32; ++i)
      wrong += static_cast<unsigned *>(output.contents)[i] != (i < 16 ? 4 * i + 1 : 0);
    printf("ICB=%d guarded=%d: %u wrong results\n", icb, guarded, wrong);
    return wrong != 0;
  }
}

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--reproduce") {
    puts("reproducer: inspect Metal shader-validation output; correct pixels do not exclude a validation report");
    return run(true, false);
  }
  if (argc != 1) return 2;
  if (auto result = run(false, false)) return result;
  if (auto result = run(true, true)) return result;
  puts("passed: native zero-row control and guarded ICB reads");
  return 0;
}
