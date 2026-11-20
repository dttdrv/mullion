import Metal
// whether a texture's usage costs it lossless compression on this GPU: a full-screen pass writes a smooth image (which
// compresses well) into a 4096x4096 RGBA8 texture, then a pass reads it 8 times per pixel; timed for each usage
let device = MTLCreateSystemDefaultDevice()!
let queue = device.makeCommandQueue()!
let lib = try! device.makeLibrary(source: """
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; };
vertex V vs(uint i [[vertex_id]]) { float2 p[3] = {{-1,-1},{3,-1},{-1,3}}; return { float4(p[i], 0, 1) }; }
fragment float4 fill(V v [[stage_in]]) { return float4(v.p.x / 4096, v.p.y / 4096, 0.5, 1); }
fragment float4 readback(V v [[stage_in]], texture2d<float> t [[texture(0)]]) {
  float4 s = 0; uint2 c = uint2(v.p.xy);
  for (uint i = 0; i < 8; i++) s += t.read((c + uint2(i * 517, i * 131)) % 4096);
  return s;
}
""", options: nil)
func pipeline(_ fs: String) -> MTLRenderPipelineState {
  let d = MTLRenderPipelineDescriptor(); d.vertexFunction = lib.makeFunction(name: "vs"); d.fragmentFunction = lib.makeFunction(name: fs)
  d.colorAttachments[0].pixelFormat = .rgba8Unorm; return try! device.makeRenderPipelineState(descriptor: d)
}
let fill = pipeline("fill"), read = pipeline("readback")
let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 4096, height: 4096, mipmapped: false); td.storageMode = .private
td.usage = [.renderTarget]; let sink = device.makeTexture(descriptor: td)!
func time(_ usage: MTLTextureUsage) -> (Double, Double) {
  td.usage = usage; let t = device.makeTexture(descriptor: td)!
  var w: [Double] = [], r: [Double] = []
  for _ in 0..<30 {
    for (out, pso, list) in [(t, fill, 0), (sink, read, 1)] {
      let cb = queue.makeCommandBuffer()!; let p = MTLRenderPassDescriptor(); p.colorAttachments[0].texture = out
      p.colorAttachments[0].loadAction = .dontCare; p.colorAttachments[0].storeAction = .store
      let e = cb.makeRenderCommandEncoder(descriptor: p)!; e.setRenderPipelineState(pso); e.setFragmentTexture(t, index: 0)
      e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3); e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
      let ms = (cb.gpuEndTime - cb.gpuStartTime) * 1000; if list == 0 { w.append(ms) } else { r.append(ms) }
    }
  }
  return (w.sorted()[15], r.sorted()[15])
}
print("\(device.name), 4096x4096 RGBA8, median of 30 (write pass / 8-read pass, ms):")
for (name, usage) in [("RenderTarget|ShaderRead", MTLTextureUsage([.renderTarget, .shaderRead])),
                      ("+PixelFormatView", MTLTextureUsage([.renderTarget, .shaderRead, .pixelFormatView])),
                      ("+ShaderWrite", MTLTextureUsage([.renderTarget, .shaderRead, .shaderWrite])),
                      ("+ShaderWrite+PixelFormatView", MTLTextureUsage([.renderTarget, .shaderRead, .shaderWrite, .pixelFormatView]))] {
  let (w, r) = time(usage); print(String(format: "  %-30@ %6.3f / %6.3f", name as NSString, w, r))
}
