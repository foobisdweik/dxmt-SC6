// contract: every error returned by the bridge belongs to its Reference and survives the pool around the call.
// "You must not relinquish ownership of an object you do not own" (Apple, Advanced Memory Management Programming
// Guide, "Memory Management Policy", "You Don't Own Objects Returned by Reference":
// https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/MemoryMgmt/Articles/mmRules.html).
// the inventory is every ret_error assignment in winemetal_unix.c, matched to its Metal.hpp wrapper: library data
// and source, function constants, compute, render, mesh, tile and linked compute pipelines, and binary archive
// creation. each has a fault patch that removes its retain. not here: residency sets (no input Metal must refuse),
// and archive serialization, whose one refusal (an empty archive) is an error Metal itself keeps a count on, so its
// retain cannot be told from none.
// an empty archive must miss: "Metal returns an error instead of compiling a missing function" (Apple,
// MTLPipelineOption.failOnBinaryArchiveMiss). the float fragment output also cannot match an RGBA32Uint target, and
// a linked function that is an entry point gives the pipeline two.
// descriptions are copied before pool drainage and must be identical afterward, then their References are dropped.
#include "d3d11_test.hpp"
#include "../../src/winemetal/Metal.hpp"
#include <iterator>
#include <thread>

static const char source[] = R"metal(
#include <metal_stdlib>
using namespace metal;
vertex float4 vs() { return float4(0, 0, 0, 1); }
fragment float4 fs() { return float4(1); }
kernel void cs(device uint *result [[buffer(0)]]) { result[0] = 0; }
struct Vertex { float4 position [[position]]; };
constexpr constant uint vertex_count = 1, primitive_count = 1;
[[mesh]] void ms(mesh<Vertex, void, vertex_count, primitive_count, topology::point> result) {
  result.set_vertex(0, Vertex{float4(0, 0, 0, 1)});
  result.set_index(0, 0);
  result.set_primitive_count(primitive_count);
}
struct Color { float4 value [[color(0)]]; };
kernel void ts(imageblock<Color, imageblock_layout_implicit> colors,
               ushort2 tid [[thread_position_in_threadgroup]]) { colors.write(Color{float4(0)}, tid); }
)metal";

template <typename F>
static void
rejected(const char *name, F call) {
  step("%s on thread %lu", name, GetCurrentThreadId());
  WMT::Reference<WMT::Error> error;
  std::string before;
  {
    auto pool = WMT::MakeAutoreleasePool();
    expect(call(error), "Metal accepted the invalid input");
    if (expect(error != nullptr, "Metal returned no error"))
      before = error.description().getUTF8String();
    printf("Metal: %s\n", before.c_str());
  }
  {
    auto pool = WMT::MakeAutoreleasePool();
    if (error) {
      auto after = error.description().getUTF8String();
      expect(!before.empty() && after == before, "diagnostic changed after pool drainage: %s", after.c_str());
    }
    error = nullptr;
  }
}

int
main() {
  auto pool = WMT::MakeAutoreleasePool();
  auto devices = WMT::CopyAllDevices();
  CHECK(devices && devices.count() ? S_OK : E_FAIL);
  auto device = devices.object(0);
  WMT::Reference<WMT::Error> error;
  step("compile Metal functions");
  auto library = device.newLibraryWithSource(source, error);
  if (error)
    printf("Metal compiler: %s\n", error.description().getUTF8String().c_str());
  CHECK(library ? S_OK : E_FAIL);
  auto vs = library.newFunction("vs"), fs = library.newFunction("fs"), cs = library.newFunction("cs");
  auto ms = library.newFunction("ms"), ts = library.newFunction("ts");
  CHECK(vs && fs && cs && ms && ts ? S_OK : E_FAIL);
  auto archive = device.newBinaryArchive(nullptr, error);
  CHECK(archive && !error ? S_OK : E_FAIL);
  obj_handle_t archives[] = {archive};
  WMTComputePipelineInfo compute{};
  WMT::InitializeComputePipelineInfo(compute);
  compute.compute_function = cs;
  compute.binary_archives_for_lookup.set(archives);
  compute.num_binary_archives_for_lookup = std::size(archives);
  compute.fail_on_binary_archive_miss = true;
  WMTRenderPipelineInfo render{};
  WMT::InitializeRenderPipelineInfo(render);
  render.vertex_function = vs;
  render.fragment_function = fs;
  render.colors[0].pixel_format = WMTPixelFormatRGBA32Uint;
  WMTMeshRenderPipelineInfo mesh{};
  WMT::InitializeMeshRenderPipelineInfo(mesh);
  mesh.mesh_function = ms;
  mesh.fragment_function = fs;
  mesh.colors[0].pixel_format = WMTPixelFormatRGBA8Unorm;
  mesh.binary_archives_for_lookup.set(archives);
  mesh.num_binary_archives_for_lookup = std::size(archives);
  mesh.fail_on_binary_archive_miss = true;
  WMTTileRenderPipelineInfo tile{};
  WMT::InitializeTileRenderPipelineInfo(tile);
  tile.tile_function = ts;
  tile.color_formats[0] = WMTPixelFormatRGBA8Unorm;
  tile.binary_archives_for_lookup.set(archives);
  tile.num_binary_archives_for_lookup = std::size(archives);
  tile.fail_on_binary_archive_miss = true;
  WMTLinkedComputePipelineInfo link{};
  obj_handle_t entries[] = {fs};
  link.compute_function = cs;
  link.functions.set(entries);
  link.function_count = std::size(entries);
  link.max_call_stack_depth = 1; // MTLComputePipelineDescriptor.maxCallStackDepth's default
  auto run = [&] {
    auto pool = WMT::MakeAutoreleasePool();
    rejected("library data", [&](auto &e) {
      const char data[] = "not a Metal library";
      return !device.newLibrary(data, sizeof(data), e);
    });
    rejected("library source", [&](auto &e) {
      return !device.newLibraryWithSource("#error bridge_error_source\n", e);
    });
    rejected("function constants", [&](auto &e) {
      return !library.newFunctionWithConstants("bridge_error_missing", nullptr, 0, e);
    });
    rejected("compute archive miss", [&](auto &e) { return !device.newComputePipelineState(compute, e); });
    rejected("render output mismatch", [&](auto &e) {
      return !device.newRenderPipelineState(render, e);
    });
    rejected("mesh archive miss", [&](auto &e) { return !device.newRenderPipelineState(mesh, e); });
    rejected("tile archive miss", [&](auto &e) { return !device.newRenderPipelineState(tile, e); });
    rejected("linked compute entry point", [&](auto &e) {
      return !device.newLinkedComputePipelineState(link, e);
    });
    rejected("binary archive load", [&](auto &e) { return !device.newBinaryArchive("/dev/null", e); });
  };
  run();
  std::thread(run).join();
  return verdict();
}
