// how wide a counting segment's count is, which the occlusion queries of both libraries split their draws by
// (d3d11_context_impl.cpp SplitVisibility, d3d12_command_list.cpp PreDraw). Apple documents no width. triangles over
// an 8192x8192 target, 2^26 samples each, count in one segment of a buffer filled with a sentinel, by draws, by
// instances and by triangles of one draw; then in two segments
import Metal

let device = MTLCreateSystemDefaultDevice()!
FileHandle.standardError.write("\(device.name)\n".data(using: .utf8)!)
let library = try! device.makeLibrary(source: """
  #include <metal_stdlib>
  using namespace metal;
  [[vertex]] float4 vs(uint id [[vertex_id]]) { id %= 3; return float4(float((id << 1) & 2) * 2 - 1, 1 - float(id & 2) * 2, 0, 1); }
  [[fragment]] half4 fs() { return half4(1); }
  """, options: nil)
let pipelineDescriptor = MTLRenderPipelineDescriptor()
pipelineDescriptor.vertexFunction = library.makeFunction(name: "vs")
pipelineDescriptor.fragmentFunction = library.makeFunction(name: "fs")
pipelineDescriptor.colorAttachments[0].pixelFormat = .r8Unorm
let pipeline = try! device.makeRenderPipelineState(descriptor: pipelineDescriptor)
let side = 8192
let targetDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: side, height: side, mipmapped: false)
targetDescriptor.usage = .renderTarget
targetDescriptor.storageMode = .private
let target = device.makeTexture(descriptor: targetDescriptor)!
let buffer = device.makeBuffer(length: 64, options: .storageModeShared)!
let word = buffer.contents().bindMemory(to: UInt64.self, capacity: 8)
let queue = device.makeCommandQueue()!

// one pass, a segment for each entry of `segments`: its draws as (vertex count, instance count)
func pass(_ name: String, _ segments: [[(Int, Int)]]) {
  for i in 0 ..< 8 { word[i] = 0xAAAA_AAAA_AAAA_AAAA }
  let descriptor = MTLRenderPassDescriptor()
  descriptor.colorAttachments[0].texture = target
  descriptor.colorAttachments[0].loadAction = .dontCare
  descriptor.colorAttachments[0].storeAction = .dontCare
  descriptor.visibilityResultBuffer = buffer
  let commands = queue.makeCommandBuffer()!
  let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)!
  encoder.setRenderPipelineState(pipeline)
  for (segment, draws) in segments.enumerated() {
    encoder.setVisibilityResultMode(.counting, offset: 8 + segment * 8)
    for (vertices, instances) in draws {
      encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: vertices, instanceCount: instances)
    }
  }
  encoder.endEncoding()
  commands.commit()
  commands.waitUntilCompleted()
  print("  \(name): " + (0 ..< segments.count).map { String(word[1 + $0], radix: 16) }.joined(separator: " "))
}
print("one segment's count, in hex, of triangles of 2^26 samples:")
pass("1 draw", [[(3, 1)]])
pass("63 draws", [Array(repeating: (3, 1), count: 63)])
pass("64 draws", [Array(repeating: (3, 1), count: 64)])
pass("100 draws", [Array(repeating: (3, 1), count: 100)])
pass("1 draw of 100 instances", [[(3, 100)]])
pass("1 draw of 100 triangles", [[(300, 1)]])
print("two segments of 50 draws each:")
pass("50 + 50 draws", [Array(repeating: (3, 1), count: 50), Array(repeating: (3, 1), count: 50)])
