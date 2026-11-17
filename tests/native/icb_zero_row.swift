// Native control for the constant zero-row validation report on Apple M2 / macOS 27.2.1.
// Default: both controls must return the expected values. With --reproduce and MTL_SHADER_VALIDATION=1,
// enabling ICB support on the unguarded access reports an invalid load despite correct output.
// The dispatch is direct; no Wine, AIR conversion or LLVM is involved.
import Metal

let device = MTLCreateSystemDefaultDevice()!
let options = MTLCompileOptions()
options.languageVersion = .version4_0
let queue = device.makeCommandQueue()!
let input = device.makeBuffer(length: 256, options: .storageModeShared)!
let output = device.makeBuffer(length: 128, options: .storageModeShared)!
let arguments = device.makeBuffer(length: MemoryLayout<UInt64>.size, options: .storageModeShared)!
for i in 0..<64 { input.contents().storeBytes(of: UInt32(i + 1), toByteOffset: i * 4, as: UInt32.self) }
arguments.contents().storeBytes(of: input.gpuAddress, as: UInt64.self)

func run(icb: Bool, guarded: Bool) throws {
    let access = guarded ? "out[tid] = tid < 16 ? args.rows[4 * tid] : 0;"
        : "constant uint *p = tid < 16 ? args.rows + 4 * tid : zero_row; out[tid] = *p;"
    let library = try device.makeLibrary(source: """
        #include <metal_stdlib>
        using namespace metal;
        constant uint zero_row[4] = {0, 0, 0, 0};
        struct Args { constant uint *rows; };
        kernel void zero_test(constant Args &args [[buffer(0)]], device uint *out [[buffer(1)]],
                              uint tid [[thread_position_in_grid]]) { \(access) }
        """, options: options)
    let descriptor = MTLComputePipelineDescriptor()
    descriptor.computeFunction = library.makeFunction(name: "zero_test")
    descriptor.supportIndirectCommandBuffers = icb
    let state = try device.makeComputePipelineState(descriptor: descriptor, options: [], reflection: nil)
    for i in 0..<32 { output.contents().storeBytes(of: UInt32.max, toByteOffset: i * 4, as: UInt32.self) }
    let command = queue.makeCommandBuffer()!
    let encoder = command.makeComputeCommandEncoder()!
    encoder.setComputePipelineState(state)
    encoder.useResource(input, usage: .read)
    encoder.setBuffer(arguments, offset: 0, index: 0)
    encoder.setBuffer(output, offset: 0, index: 1)
    encoder.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1),
                                threadsPerThreadgroup: MTLSize(width: 32, height: 1, depth: 1))
    encoder.endEncoding()
    command.commit()
    command.waitUntilCompleted()
    guard command.status == .completed else { fatalError("GPU command failed: \(String(describing: command.error))") }
    let wrong = (0..<32).filter {
        output.contents().load(fromByteOffset: $0 * 4, as: UInt32.self) != ($0 < 16 ? UInt32(4 * $0 + 1) : 0)
    }.count
    print("ICB=\(icb) guarded=\(guarded): \(wrong) wrong results")
    guard wrong == 0 else { fatalError("incorrect GPU output") }
}

switch Array(CommandLine.arguments.dropFirst()) {
case []:
    try run(icb: false, guarded: false)
    try run(icb: true, guarded: true)
case ["--reproduce"]:
    try run(icb: true, guarded: false)
default:
    fatalError("usage: icb_zero_row [--reproduce]")
}
