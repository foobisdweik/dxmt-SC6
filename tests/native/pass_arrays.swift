// how many scissors and viewports one render pass takes (src/mullion/mullion_context.hpp kPassRectangles). Apple documents
// no limit; the driver ends the process inside the draw that follows one set too many, in a pass that has several
// viewports: every set of viewports counts as many as it sets, and every set of scissors, even of one, as many as the
// pass has viewports. a pass of `each` viewports sets its viewports, its scissors or one scissor before every one of
// its draws: this finds the first number of sets a pass does not survive, each pass in a process of its own.
import Metal

let arguments = CommandLine.arguments
if arguments.count == 4 {
  let kind = arguments[1], each = Int(arguments[2])!, sets = Int(arguments[3])!
  let device = MTLCreateSystemDefaultDevice()!
  let library = try! device.makeLibrary(source: """
    #include <metal_stdlib>
    using namespace metal;
    struct V { float4 p [[position]]; };
    vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); return {float4(uv * float2(2, -2) + float2(-1, 1), 0, 1)}; }
    fragment float4 ps() { return 1; }
    """, options: nil)
  let pipeline = MTLRenderPipelineDescriptor()
  pipeline.vertexFunction = library.makeFunction(name: "vs")
  pipeline.fragmentFunction = library.makeFunction(name: "ps")
  pipeline.colorAttachments[0].pixelFormat = .rgba8Unorm
  let state = try! device.makeRenderPipelineState(descriptor: pipeline)
  let target = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 8, height: 4, mipmapped: false)
  target.usage = .renderTarget
  let pass = MTLRenderPassDescriptor()
  pass.colorAttachments[0].texture = device.makeTexture(descriptor: target)!
  let commands = device.makeCommandQueue()!.makeCommandBuffer()!
  let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
  encoder.setRenderPipelineState(state)
  encoder.setViewports([MTLViewport](repeating: MTLViewport(originX: 0, originY: 0, width: 8, height: 4, znear: 0, zfar: 1), count: each))
  for set in 0..<sets {
    // each set differs from the one before
    let scissor = MTLScissorRect(x: 0, y: 0, width: set % 4 + 1, height: 1)
    if kind == "viewports" {
      encoder.setViewports([MTLViewport](repeating: MTLViewport(originX: 0, originY: 0, width: 8, height: Double(set % 4 + 1), znear: 0, zfar: 1), count: each))
    } else if kind == "scissors" {
      encoder.setScissorRects([MTLScissorRect](repeating: scissor, count: each))
    } else {
      encoder.setScissorRect(scissor)
    }
    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
  }
  encoder.endEncoding()
  commands.commit()
  commands.waitUntilCompleted()
  exit(commands.status == .completed ? 0 : 1)
}

FileHandle.standardError.write("\(MTLCreateSystemDefaultDevice()!.name)\n".data(using: .utf8)!)
func survives(_ kind: String, _ each: Int, _ sets: Int) -> Bool {
  let child = Process()
  child.executableURL = URL(fileURLWithPath: arguments[0])
  child.arguments = [kind, String(each), String(sets)]
  child.standardError = FileHandle.nullDevice
  try! child.run()
  child.waitUntilExit()
  return child.terminationReason == .exit && child.terminationStatus == 0
}
let most = 1 << 17
for kind in ["viewports", "scissors", "one scissor"] {
  for each in [1, 2, 3, 16] {
    if survives(kind, each, most) {
      print("\(kind), \(each) viewports: a pass takes \(most) sets")
      continue
    }
    var takes = 0, ends = most
    while ends - takes > 1 {
      let middle = (takes + ends) / 2
      if survives(kind, each, middle) { takes = middle } else { ends = middle }
    }
    print("\(kind), \(each) viewports: the process ends in set \(ends), \(ends * each) by the viewports")
  }
}
