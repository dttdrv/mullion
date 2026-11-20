// why a texture keeps PixelFormatView whenever a view of it might cast across layouts, and Mullion does not drop the usage
// on a guess (a per-game profile of the views a game made): an RGBA8 texture compressed without it, read through an R32Uint
// view, reads every texel wrong (M6, macOS 27.3, validation off): the view sees the compressed bits. a texture whose
// guess was wrong would need a new allocation and a decompressing copy before its first such view.
import Metal
let device = MTLCreateSystemDefaultDevice()!
let n = 1024
func texture(_ usage: MTLTextureUsage) -> MTLTexture {
  let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: n, height: n, mipmapped: false)
  d.usage = usage; d.storageMode = .private
  return device.makeTexture(descriptor: d)!
}
let lib = try! device.makeLibrary(source: """
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; };
vertex V vs(uint i [[vertex_id]]) { float2 p[3] = {{-1,-1},{3,-1},{-1,3}}; return { float4(p[i], 0, 1) }; }
fragment float4 fill(V v [[stage_in]]) { return float4(v.p.x / 1024, v.p.y / 1024, 0.25, 1); }
kernel void asuint(texture2d<uint> t [[texture(0)]], device uint *out [[buffer(0)]], uint2 i [[thread_position_in_grid]]) {
  out[i.y * 1024 + i.x] = t.read(i).x;
}
kernel void asunorm(texture2d<float> t [[texture(0)]], device uint *out [[buffer(0)]], uint2 i [[thread_position_in_grid]]) {
  float4 c = t.read(i); out[i.y * 1024 + i.x] = uint(round(c.x * 255)) | uint(round(c.y * 255)) << 8 | uint(round(c.z * 255)) << 16 | uint(round(c.w * 255)) << 24;
}
""", options: nil)
let rd = MTLRenderPipelineDescriptor(); rd.vertexFunction = lib.makeFunction(name: "vs"); rd.fragmentFunction = lib.makeFunction(name: "fill")
rd.colorAttachments[0].pixelFormat = .rgba8Unorm
let fill = try! device.makeRenderPipelineState(descriptor: rd)
let asUint = try! device.makeComputePipelineState(function: lib.makeFunction(name: "asuint")!)
let asUnorm = try! device.makeComputePipelineState(function: lib.makeFunction(name: "asunorm")!)
let q = device.makeCommandQueue()!
func run(_ usage: MTLTextureUsage, _ label: String) {
  let t = texture(usage)
  let cb = q.makeCommandBuffer()!
  let rp = MTLRenderPassDescriptor(); rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = .clear; rp.colorAttachments[0].storeAction = .store
  let re = cb.makeRenderCommandEncoder(descriptor: rp)!; re.setRenderPipelineState(fill); re.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3); re.endEncoding()
  let truth = device.makeBuffer(length: n * n * 4, options: .storageModeShared)!
  let viewed = device.makeBuffer(length: n * n * 4, options: .storageModeShared)!
  let ce = cb.makeComputeCommandEncoder()!
  ce.setComputePipelineState(asUnorm); ce.setTexture(t, index: 0); ce.setBuffer(truth, offset: 0, index: 0)
  ce.dispatchThreads(MTLSize(width: n, height: n, depth: 1), threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
  let view = t.makeTextureView(pixelFormat: .r32Uint)!
  ce.setComputePipelineState(asUint); ce.setTexture(view, index: 0); ce.setBuffer(viewed, offset: 0, index: 0)
  ce.dispatchThreads(MTLSize(width: n, height: n, depth: 1), threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
  ce.endEncoding(); cb.commit(); cb.waitUntilCompleted()
  let a = truth.contents().bindMemory(to: UInt32.self, capacity: n * n), b = viewed.contents().bindMemory(to: UInt32.self, capacity: n * n)
  var wrong = 0; for i in 0..<(n * n) where a[i] != b[i] { wrong += 1 }
  print(label, ": R32Uint view read", wrong, "of", n * n, "texels otherwise than the RGBA8 texture")
}
run([.renderTarget, .shaderRead, .pixelFormatView], "with PixelFormatView   ")
run([.renderTarget, .shaderRead], "without PixelFormatView")
