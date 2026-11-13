// contract: a retained-reference command buffer keeps a shader's view alive after CPU encoding and before execution.
// "maintains strong references to resource instances that its commands need" (Apple, MTLCommandBufferDescriptor.
// retainedReferences, https://developer.apple.com/documentation/metal/mtlcommandbufferdescriptor/retainedreferences).
// useResource: "Call the method before encoding draw calls that may access resource through an argument buffer."
// https://developer.apple.com/documentation/metal/mtlrendercommandencoder/useresource(_:usage:)
// direct argument-buffer encoding writes the texture's gpuResourceID into its member (Apple, Improving CPU
// performance by using argument buffers, "Encode resources into argument buffers"):
// https://developer.apple.com/documentation/metal/improving-cpu-performance-by-using-argument-buffers
// run with NSZombieEnabled=YES, MTL_DEBUG_LAYER=1 and MTL_SHADER_VALIDATION=1. no argument encoder retains the view.
import Metal

let device = MTLCreateSystemDefaultDevice()!
precondition(device.argumentBuffersSupport == .tier2)
let library = try device.makeLibrary(source: """
  #include <metal_stdlib>
  using namespace metal;
  struct Arguments { texture2d<uint> source; };
  vertex float4 vs(uint id [[vertex_id]]) {
    return float4(float((id << 1) & 2) * 2 - 1, 1 - float(id & 2) * 2, 0, 1);
  }
  fragment uint fs(float4 at [[position]], constant Arguments &args [[buffer(0)]]) {
    return args.source.read(uint2(at.xy)).x;
  }
  """, options: nil)
let pipelineDescriptor = MTLRenderPipelineDescriptor()
pipelineDescriptor.vertexFunction = library.makeFunction(name: "vs")
pipelineDescriptor.fragmentFunction = library.makeFunction(name: "fs")
pipelineDescriptor.colorAttachments[0].pixelFormat = .r32Uint
let pipeline = try device.makeRenderPipelineState(descriptor: pipelineDescriptor)
let side = 4
let sourceDescriptor = MTLTextureDescriptor.texture2DDescriptor(
  pixelFormat: .r32Uint, width: side, height: side, mipmapped: false)
sourceDescriptor.textureType = .type2DArray
sourceDescriptor.arrayLength = 2
sourceDescriptor.storageMode = .shared
sourceDescriptor.usage = [.shaderRead, .pixelFormatView]
let source = device.makeTexture(descriptor: sourceDescriptor)!
let values = (0 ..< side * side).map { UInt32($0 + 1) }
values.withUnsafeBytes {
  source.replace(region: MTLRegionMake2D(0, 0, side, side), mipmapLevel: 0, slice: 1,
    withBytes: $0.baseAddress!, bytesPerRow: side * MemoryLayout<UInt32>.stride,
    bytesPerImage: values.count * MemoryLayout<UInt32>.stride)
}
let targetDescriptor = MTLTextureDescriptor.texture2DDescriptor(
  pixelFormat: .r32Uint, width: side, height: side, mipmapped: false)
targetDescriptor.storageMode = .shared
targetDescriptor.usage = .renderTarget
let target = device.makeTexture(descriptor: targetDescriptor)!
let arguments = device.makeBuffer(length: MemoryLayout<MTLResourceID>.size, options: .storageModeShared)!
let queue = device.makeCommandQueue()!
let descriptor = MTLCommandBufferDescriptor()
descriptor.errorOptions = .encoderExecutionStatus
let commands = queue.makeCommandBuffer(descriptor: descriptor)!
precondition(commands.retainedReferences)
weak var unused: AnyObject?
autoreleasepool {
  let view = source.makeTextureView(pixelFormat: .r32Uint, textureType: .type2D,
    levels: 0 ..< 1, slices: 1 ..< 2)!
  unused = view
}
precondition(unused == nil, "the parent retains unused views; the ownership control is inconclusive")
print("unused view released: true")
weak var alive: AnyObject?
autoreleasepool {
  let view = source.makeTextureView(pixelFormat: .r32Uint, textureType: .type2D,
    levels: 0 ..< 1, slices: 1 ..< 2)!
  alive = view
  arguments.contents().storeBytes(of: view.gpuResourceID, as: MTLResourceID.self)
  let pass = MTLRenderPassDescriptor()
  pass.colorAttachments[0].texture = target
  pass.colorAttachments[0].loadAction = .clear
  pass.colorAttachments[0].storeAction = .store
  let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
  encoder.setRenderPipelineState(pipeline)
  encoder.setFragmentBuffer(arguments, offset: 0, index: 0)
  encoder.useResource(view, usage: .read, stages: .fragment)
  encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
  encoder.endEncoding()
}
precondition(alive != nil, "useResource did not retain the view before execution")
print("view retained after encoding: true")
commands.commit()
commands.waitUntilCompleted()
precondition(commands.status == .completed, "\(String(describing: commands.error))")
var got = Array(repeating: UInt32.max, count: values.count)
got.withUnsafeMutableBytes {
  target.getBytes($0.baseAddress!, bytesPerRow: side * MemoryLayout<UInt32>.stride,
    from: MTLRegionMake2D(0, 0, side, side), mipmapLevel: 0)
}
precondition(got == values, "resource-id pixels: \(got), want \(values)")
print("resource-id pixels: correct")
