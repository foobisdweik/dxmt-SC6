// observation: passes that reach one heap's memory through a placement sparse buffer and through a buffer placed on
// that heap run in no order, in one command buffer or in command buffers committed back to back on one queue. both
// see every earlier write only once the earlier command buffer has completed. Metal's headers say the mapping makes
// the buffer "alias specific tiles from a Metal heap" (MTL4CommandQueue.h, updateBufferMappings) and name no order
// between the two views. each case fills the heap through the placed buffer, writes its head through the sparse one
// in a kernel, and reads both back: in order, the head has the kernel's value and the rest the fill's
import Metal

let device = MTLCreateSystemDefaultDevice()!
let tile = device.sparseTileSizeInBytes(sparsePageSize: .size64), tiles = 4, length = tiles * tile, head = tile + 16
let heapDescriptor = MTLHeapDescriptor()
heapDescriptor.type = .placement
heapDescriptor.size = length
heapDescriptor.maxCompatiblePlacementSparsePageSize = .size64
let heap = device.makeHeap(descriptor: heapDescriptor)!
let sparse = device.makeBuffer(length: length, options: .storageModePrivate, placementSparsePageSize: .size64)!
let placed = heap.makeBuffer(length: length, options: .storageModePrivate, offset: 0)!
let queue = device.makeCommandQueue()!, mapper = device.makeMTL4CommandQueue()!
let residency = try device.makeResidencySet(descriptor: MTLResidencySetDescriptor())
residency.addAllocation(heap)
residency.addAllocation(sparse)
residency.commit()
residency.requestResidency()
queue.addResidencySet(residency)
mapper.addResidencySet(residency)
let mapped = device.makeSharedEvent()!
mapper.updateMappings(buffer: sparse, heap: heap, operations: [
  MTL4UpdateSparseBufferMappingOperation(mode: .map, bufferRange: NSRange(location: 0, length: tiles), heapOffset: 0)
])
mapper.signalEvent(mapped, value: 1)
while mapped.signaledValue < 1 { usleep(100) }
let kernel = try device.makeComputePipelineState(function: device.makeLibrary(source: """
  #include <metal_stdlib>
  using namespace metal;
  kernel void write(device uchar *bytes [[buffer(0)]], constant uint &value [[buffer(1)]],
                    uint at [[thread_position_in_grid]]) { bytes[at] = uchar(value); }
  """, options: nil).makeFunction(name: "write")!)
let fill = device.makeBuffer(length: length, options: .storageModeShared)!
let views = [placed, sparse].map { ($0, device.makeBuffer(length: length, options: .storageModeShared)!) }
let filled: UInt8 = 0x11
var written: UInt32 = 0x33
memset(fill.contents(), Int32(filled), length)

enum Between: String, CaseIterable {
  case nothing = "one command buffer", commit = "command buffers back to back",
       completion = "each command buffer completed before the next"
}
for between in Between.allCases {
  var ordered = true
  for _ in 0 ..< 16 {
    var commands = queue.makeCommandBuffer()!
    func next() {
      guard between != .nothing else { return }
      commands.commit()
      if between == .completion { commands.waitUntilCompleted() }
      commands = queue.makeCommandBuffer()!
    }
    var blit = commands.makeBlitCommandEncoder()!
    blit.copy(from: fill, sourceOffset: 0, to: placed, destinationOffset: 0, size: length)
    blit.endEncoding()
    next()
    let compute = commands.makeComputeCommandEncoder()!
    compute.setComputePipelineState(kernel)
    compute.setBuffer(sparse, offset: 0, index: 0)
    compute.setBytes(&written, length: MemoryLayout.size(ofValue: written), index: 1)
    compute.dispatchThreads(MTLSizeMake(head, 1, 1),
                            threadsPerThreadgroup: MTLSizeMake(kernel.threadExecutionWidth, 1, 1))
    compute.endEncoding()
    next()
    blit = commands.makeBlitCommandEncoder()!
    for (view, copy) in views { blit.copy(from: view, sourceOffset: 0, to: copy, destinationOffset: 0, size: length) }
    blit.endEncoding()
    commands.commit()
    commands.waitUntilCompleted()
    precondition(commands.error == nil)
    for (_, copy) in views {
      let bytes = UnsafeBufferPointer(start: copy.contents().assumingMemoryBound(to: UInt8.self), count: length)
      ordered = ordered && bytes[..<head].allSatisfy { $0 == UInt8(written) } && bytes[head...].allSatisfy { $0 == filled }
    }
  }
  print("\(between.rawValue): both views in order every time: \(ordered)")
}
