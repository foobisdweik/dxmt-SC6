// contract: each SO stream counts independently, including when another stream cannot fit a complete primitive.
// "each stream has its own independent NumPrimitivesWritten and PrimitivesStorageNeeded counters" (D3D11.3 14.5).
// SO_STATISTICS "is a synonym for D3D11_QUERY_SO_STATISTICS_STREAM0" (20.4.9); the unnumbered overflow
// predicate includes "any of the streams" (20.4.10). every written record and partial-stride tail is read back.
// GSInvocations counts input primitives "multiplied by the instance count per primitive" (13.2.1.1);
// GSPrimitives counts "Number of primitives GS generated" (20.4.7). a separate single-stream GS emits once.
// "the driver will be asked to calculate the difference between two requests (one request for Issue( BEGIN ),
// and one request for Issue( END ))." (D3D11.3 20.4.7). "However, the Issue command is not able to be predicated."
// (20.2). recording contexts can bracket the same query independently; statistics count only the draws executed
// by each list, including nested lists and immediate queries enclosing their execution.
// contract: finishing a deferred list ends every open query in that list (D3D11.3 6.3.6): "In addition, any Queries
// that have been Begun in the Deferred Contexts but not Ended, are implicitly Ended by the invocation to Finalize."
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#6.3.6%20Query%20Interactions
// visible single-sample draws count width * height (20.4.6); scissored-out draws count zero. predicates return
// "FALSE. Otherwise, the OCCLUSION Predicate would return TRUE" (20.4.8).
// contract: query reads require an ended range on the immediate context, and copy data only when ready and sized.
// "If DataSize is 0, GetData is only used to check status." (Microsoft Learn, ID3D11DeviceContext::GetData).
// "Returning S_OK indicates the Query is \"signaled\""; "A second D3DISSUE_BEGIN will result in the range being
// reset" (D3D11.3 20.3.5, 20.3.4).
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata
// Wine's test_occlusion_query, test_timestamp_query and test_so_statistics_query record Windows returning
// DXGI_ERROR_INVALID_CALL before End and leaving the destination intact; test_deferred_context_queries records
// that error on a deferred context even after execution. the driver-state description in 20.3.2 is not the
// application's GetData contract. an empty range counts zero; EVENT returns TRUE (20.4.2).
// predicates are the BOOL query types (20.2, 20.4.8, 20.4.10). CreatePredicate accepts
// "D3D11_QUERY_SO_OVERFLOW_PREDICATE or D3D11_QUERY_OCCLUSION_PREDICATE" (Microsoft Learn).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createpredicate
// query types come from the contiguous D3D11_QUERY enumeration; the seed varies restart and reuse traces.
// the restart draw covers a single-sample target with depth and stencil disabled: 20.4.6 counts width * height.
// pipeline statistics count that draw too: "Number of vertices IA generated (not subtracting any caching)" and
// "Number of times PS is executed" (20.4.7). The restarted range excludes its discarded draw on both contexts.
// "The value of PSInvocations may include or exclude helper pixels for 2x2 stamps" (20.4.7). The triangle covers
// the viewport's quad; clipping can split it into two triangles, each needing all lanes of its target's stamps.
// "OCCLUSION measures multi-samples (not pixels)"; "If the Pixel Shader outputs depth, then PSInvocations
// must increment as expected, even if the output depth fails"; "when a NULL pixel shader is bound to the
// pipeline, PSInvocations does not increment" (D3D11.3 20.4.7). sample frequency is not requested here.
// "the data associated with the Query is expected to be returned/ copied out through the pData parameter."
// (D3D11.3 20.3.5). repeated reads of one ended range must return that same data.
// a cut means "the strip defined up to the previous index is to be completed, and the next index is a new strip"
// (8.12). the expected primitive count is the sum over the complete strips authored below.
// D3D10_QUERY has eight kinds, ending at SO_OVERFLOW_PREDICATE; D3D11 stream-specific queries are not D3D10 kinds.
// "Specifies the type of query." (D3D10_QUERY_DESC::Query); only those enum values may create a D3D10 query.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/ns-d3d10-d3d10_query_desc
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/ne-d3d10-d3d10_query
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <d3d11_1.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <random>

static const char stream_shader[] = R"hlsl(
struct Input { uint id : ID; uint instance : INSTANCE; };
Input vs_stats(uint id : SV_VertexID, uint instance : SV_InstanceID) {
  Input v; v.id = id; v.instance = instance; return v;
}
struct Output { float4 position : SV_Position; uint4 value : VALUE; };
Output record(Input v, uint gi, uint stream_id) {
  Output o; o.position = float4(0, 0, 0, 1); o.value = uint4(v.id, v.instance, gi, stream_id); return o;
}
#if STREAMS == 1
[maxvertexcount(1)] [instance(GS_INSTANCES)]
void gs_stats(point Input v[1], uint gi : SV_GSInstanceID, inout PointStream<Output> s0) {
  s0.Append(record(v[0], gi, 0));
}
#else
[maxvertexcount(4)]
void gs_stats(point Input v[1], inout PointStream<Output> s0, inout PointStream<Output> s1,
              inout PointStream<Output> s2, inout PointStream<Output> s3) {
  s0.Append(record(v[0], 0, 0));
  s1.Append(record(v[0], 0, 1));
  s2.Append(record(v[0], 0, 2));
  s3.Append(record(v[0], 0, 3));
}
#endif
)hlsl";

int
main(int argc, char **argv) {
  const UINT seed = argc > 1 ? strtoul(argv[1], nullptr, 0) : 18;
  std::minstd_rand next(seed);
  const char shader[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 p = float2((id << 1) & 2, id & 2);
  return float4(p * 2 - 1, 0, 1);
}
uint ps() : SV_Target { return 1; }
uint ps_depth(out float depth : SV_Depth) : SV_Target { depth = 0.5; return 1; }
float4 vs_strip(uint id : SV_VertexID) : SV_Position {
  float2 corners[] = {float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, -1)};
  return float4(corners[id % 4], 0.5, 1);
}
)";
  step("seed %u: compile VS and PS", seed);
  auto vs_code = compile(shader, "vs", "vs"), ps_code = compile(shader, "ps", "ps"),
       depth_code = compile(shader, "ps_depth", "ps"), strip_code = compile(shader, "vs_strip", "vs");
  if (!vs_code || !ps_code || !depth_code || !strip_code)
    return 1;
  constexpr UINT max_gs_instances = 32; // D3D11.3 22.3.7, D3D11_GS_MAX_INSTANCE_COUNT
  const UINT geometry_instances[] = {1, max_gs_instances, 1};
  const UINT stream_counts[] = {1, 1, D3D11_SO_STREAM_COUNT};
  auto stats_vs_code = compile(stream_shader, "vs_stats", "vs");
  ComPtr<ID3DBlob> stats_gs_code[std::size(stream_counts)];
  for (UINT mode = 0; mode < std::size(stream_counts); mode++) {
    stats_gs_code[mode] = compile(stream_shader, "gs_stats", "gs",
        {"STREAMS=" + std::to_string(stream_counts[mode]),
         "GS_INSTANCES=" + std::to_string(geometry_instances[mode])});
    if (!stats_gs_code[mode])
      return 1;
  }
  if (!stats_vs_code)
    return 1;
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")
  );
  if (!expect(create10 != nullptr, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  for (auto feature : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    ComPtr<ID3D10Device1> legacy;
    CHECK(create10(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, feature, D3D10_1_SDK_VERSION, &legacy));
    const D3D10_QUERY_DESC event_desc{D3D10_QUERY_EVENT}, predicate_desc{D3D10_QUERY_OCCLUSION_PREDICATE};
    ComPtr<ID3D10Query> control;
    ComPtr<ID3D10Predicate> control_predicate;
    CHECK(legacy->CreateQuery(&event_desc, &control));
    CHECK(legacy->CreatePredicate(&predicate_desc, &control_predicate));
    std::vector<UINT> kinds;
    for (UINT kind = D3D10_QUERY_EVENT; kind <= D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3 + 1; kind++)
      kinds.push_back(kind);
    kinds.push_back(std::numeric_limits<UINT>::max());
    for (UINT kind : kinds) {
      step("D3D10 level=%#x query kind=%u", feature, kind);
      const bool valid = kind <= D3D10_QUERY_SO_OVERFLOW_PREDICATE;
      D3D10_QUERY_DESC desc{D3D10_QUERY(kind)};
      ComPtr<ID3D10Query> query;
      ID3D10Query *created = control.Get();
      auto hr = legacy->CreateQuery(&desc, &created);
      if (SUCCEEDED(hr) && created)
        query.Attach(created);
      expect(valid ? hr == S_OK && query : FAILED(hr) && !created, "D3D10 query kind=%u returned %#lx", kind, hr);
      if (valid && query) {
        D3D10_QUERY_DESC got{};
        query->GetDesc(&got);
        expect(got.Query == desc.Query && got.MiscFlags == 0, "D3D10 query description changed");
        const UINT bytes = kind == D3D10_QUERY_EVENT || kind == D3D10_QUERY_OCCLUSION_PREDICATE ||
                                   kind == D3D10_QUERY_SO_OVERFLOW_PREDICATE
                               ? sizeof(BOOL)
                           : kind == D3D10_QUERY_TIMESTAMP_DISJOINT  ? sizeof(D3D10_QUERY_DATA_TIMESTAMP_DISJOINT)
                           : kind == D3D10_QUERY_PIPELINE_STATISTICS ? sizeof(D3D10_QUERY_DATA_PIPELINE_STATISTICS)
                           : kind == D3D10_QUERY_SO_STATISTICS       ? sizeof(D3D10_QUERY_DATA_SO_STATISTICS)
                                                                     : sizeof(UINT64);
        expect(
            query->GetDataSize() == bytes, "D3D10 query kind=%u data size=%u expected=%u", kind, query->GetDataSize(),
            bytes
        );
      }
      ComPtr<ID3D10Predicate> predicate;
      ID3D10Predicate *created_predicate = control_predicate.Get();
      hr = legacy->CreatePredicate(&desc, &created_predicate);
      if (SUCCEEDED(hr) && created_predicate)
        predicate.Attach(created_predicate);
      const bool predicatable = kind == D3D10_QUERY_OCCLUSION_PREDICATE || kind == D3D10_QUERY_SO_OVERFLOW_PREDICATE;
      expect(
          predicatable ? hr == S_OK && predicate : hr == E_INVALIDARG && !created_predicate,
          "D3D10 predicate kind=%u returned %#lx", kind, hr
      );
    }
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate
  ));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11Device1> device1;
  ComPtr<ID3DDeviceContextState> state;
  CHECK(device.As(&device1));
  CHECK(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D10Device), nullptr, &state));
  ComPtr<ID3D10Device> device10;
  CHECK(device.As(&device10));
  alignas(D3D11_QUERY_DATA_PIPELINE_STATISTICS)
      std::array<unsigned char, sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS) + sizeof(UINT64)> bytes;
  const unsigned char sentinel = 0xa5;
  auto intact = [&] { return std::all_of(bytes.begin(), bytes.end(), [&](auto byte) { return byte == sentinel; }); };
  auto ready = [&](ID3D11Query *query) {
    HRESULT hr = S_FALSE;
    do {
      hr = immediate->GetData(query, nullptr, 0, 0);
    } while (hr == S_FALSE);
    return expect(hr == S_OK, "query never ready: %#lx", hr);
  };

  auto read_statistics = [&](ID3D11Query *query, D3D11_QUERY_DATA_PIPELINE_STATISTICS &data) {
    HRESULT hr = immediate->GetData(query, &data, sizeof(data), 0);
    if (hr == S_OK) {
      D3D11_QUERY_DATA_PIPELINE_STATISTICS repeated{};
      HRESULT again = immediate->GetData(query, &repeated, sizeof(repeated), 0);
      expect(again == S_OK && !memcmp(&data, &repeated, sizeof(data)), "repeated statistics read changed the result");
    }
    return hr;
  };

  auto source = buffer(device.Get(), sizeof(UINT), 0, seed);
  auto destination = buffer(device.Get(), sizeof(UINT), 0);
  if (!expect(source && destination, "copy buffers were not created"))
    return verdict();
  for (UINT kind = D3D11_QUERY_EVENT; kind <= D3D11_QUERY_TIMESTAMP_DISJOINT; kind++) {
    if (kind != D3D11_QUERY_EVENT && kind != D3D11_QUERY_TIMESTAMP_DISJOINT)
      continue;
    step("seed %u, query %u, pending output", seed, kind);
    D3D11_QUERY_DESC desc{D3D11_QUERY(kind)};
    ComPtr<ID3D11Query> query;
    CHECK(device->CreateQuery(&desc, &query));
    if (kind == D3D11_QUERY_TIMESTAMP_DISJOINT)
      immediate->Begin(query.Get());
    immediate->CopyResource(destination.Get(), source.Get());
    immediate->End(query.Get());
    bytes.fill(sentinel);
    HRESULT hr = immediate->GetData(query.Get(), bytes.data(), query->GetDataSize(), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    expect(hr == S_OK || hr == S_FALSE, "issued query returned %#lx", hr);
    if (hr == S_FALSE)
      expect(intact(), "pending result changed destination");
    if (!ready(query.Get()))
      return verdict();
    auto words = read(device.Get(), immediate.Get(), destination.Get());
    expect(words.size() == 1 && words[0] == seed, "copy result does not match seed");
  }

  for (UINT kind = D3D11_QUERY_EVENT; kind <= D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3; kind++) {
    const auto type = D3D11_QUERY(kind);
    const bool range = type != D3D11_QUERY_EVENT && type != D3D11_QUERY_TIMESTAMP;
    const bool predicate = type == D3D11_QUERY_OCCLUSION_PREDICATE || type == D3D11_QUERY_SO_OVERFLOW_PREDICATE ||
                           type == D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0 ||
                           type == D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1 ||
                           type == D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2 ||
                           type == D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3;
    step("seed %u, query %u, creation and interfaces", seed, kind);
    D3D11_QUERY_DESC desc{type};
    ComPtr<ID3D11Query> query;
    CHECK(device->CreateQuery(&desc, &query));
    ComPtr<ID3D11Predicate> as_predicate;
    HRESULT hr = query.As(&as_predicate);
    expect(hr == (predicate ? S_OK : E_NOINTERFACE), "predicate interface: %#lx", hr);
    as_predicate.Reset();
    const bool creatable = type == D3D11_QUERY_OCCLUSION_PREDICATE || type == D3D11_QUERY_SO_OVERFLOW_PREDICATE;
    hr = device->CreatePredicate(&desc, nullptr);
    expect(hr == (creatable ? S_FALSE : E_INVALIDARG), "predicate validation: %#lx", hr);
    hr = device->CreatePredicate(&desc, &as_predicate);
    expect(hr == (creatable ? S_OK : E_INVALIDARG), "predicate creation: %#lx", hr);
    as_predicate.Reset();
    ComPtr<ID3D10Query> query10;
    if (kind <= D3D10_QUERY_SO_OVERFLOW_PREDICATE) {
      CHECK(query.As(&query10));
      ComPtr<ID3D10Predicate> predicate10;
      hr = query10.As(&predicate10);
      expect(hr == (predicate ? S_OK : E_NOINTERFACE), "D3D10 predicate interface: %#lx", hr);
    }
    const UINT data_size = predicate || type == D3D11_QUERY_EVENT ? sizeof(BOOL)
                           : type == D3D11_QUERY_TIMESTAMP_DISJOINT ? sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT)
                           : type == D3D11_QUERY_PIPELINE_STATISTICS ? sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS)
                           : type >= D3D11_QUERY_SO_STATISTICS ? sizeof(D3D11_QUERY_DATA_SO_STATISTICS)
                                                              : sizeof(UINT64);
    if (!expect(query->GetDataSize() == data_size, "data size %u, want %u", query->GetDataSize(), data_size))
      return verdict();
    auto read_data = [&](ID3D11DeviceContext *context, bool ten, bool available) {
      const UINT size = ten && type == D3D11_QUERY_PIPELINE_STATISTICS
                            ? sizeof(D3D10_QUERY_DATA_PIPELINE_STATISTICS) : data_size;
      if (ten)
        expect(query10->GetDataSize() == size, "D3D10 data size %u, want %u", query10->GetDataSize(), size);
      auto get = [&](void *out, UINT count, UINT flags) {
        return ten ? query10->GetData(out, count, flags) : context->GetData(query.Get(), out, count, flags);
      };
      for (UINT flags : {0u, UINT(D3D11_ASYNC_GETDATA_DONOTFLUSH)}) {
        for (UINT count : {0u, size - 1, size, size + 1}) {
          for (bool output : {false, true}) {
            bytes.fill(sentinel);
            HRESULT got = get(output ? bytes.data() : nullptr, count, flags);
            const bool valid = (!count || count == size) && (!count || output);
            const HRESULT want = !valid ? E_INVALIDARG : available ? S_OK : DXGI_ERROR_INVALID_CALL;
            expect(got == want, "D3D%u size %u output %u flags %u: %#lx, want %#lx", ten ? 10 : 11,
                   count, output, flags, got, want);
            if (want != S_OK || !count)
              expect(intact(), "status or rejected read changed destination");
            else
              expect(std::all_of(bytes.begin() + size, bytes.end(), [&](auto byte) { return byte == sentinel; }),
                     "read wrote beyond data size");
          }
        }
      }
    };
    step("seed %u, query %u, before End", seed, kind);
    read_data(immediate.Get(), false, false);
    if (query10)
      read_data(immediate.Get(), true, false);
    for (UINT count : {0u, data_size - 1, data_size, data_size + 1})
      for (bool output : {false, true}) {
        bytes.fill(sentinel);
        hr = deferred->GetData(query.Get(), output ? bytes.data() : nullptr, count, 0);
        expect(hr == DXGI_ERROR_INVALID_CALL && intact(), "deferred read: %#lx or changed bytes", hr);
      }
    for (UINT variant = 0; variant < 3; variant++) {
      step("seed %u, query %u, empty range variant %u", seed, kind, variant);
      if (range && variant) {
        const UINT begins = 1 + next() % 3;
        for (UINT i = 0; i < begins; i++)
          immediate->Begin(query.Get());
        read_data(immediate.Get(), false, false);
        if (query10)
          read_data(immediate.Get(), true, false);
      }
      immediate->End(query.Get());
      if (variant == 2)
        immediate->End(query.Get());
      if (!ready(query.Get()))
        return verdict();
      read_data(immediate.Get(), false, true);
      if (query10)
        read_data(immediate.Get(), true, true);
      bytes.fill(sentinel);
      CHECK(immediate->GetData(query.Get(), bytes.data(), data_size, 0));
      if (type == D3D11_QUERY_EVENT) {
        BOOL result;
        memcpy(&result, bytes.data(), sizeof(result));
        expect(result == TRUE, "event result %d", result);
      } else if (type != D3D11_QUERY_TIMESTAMP && type != D3D11_QUERY_TIMESTAMP_DISJOINT) {
        expect(std::all_of(bytes.begin(), bytes.begin() + data_size, [](auto byte) { return byte == 0; }),
               "empty range returned nonzero data");
      }
    }
    step("seed %u, query %u, deferred recording and replay", seed, kind);
    ComPtr<ID3D11Query> recorded;
    CHECK(device->CreateQuery(&desc, &recorded));
    if (range) {
      deferred->Begin(recorded.Get());
      deferred->Begin(recorded.Get());
    }
    deferred->End(recorded.Get());
    bytes.fill(sentinel);
    hr = immediate->GetData(recorded.Get(), bytes.data(), data_size, 0);
    expect(hr == DXGI_ERROR_INVALID_CALL && intact(), "unexecuted query: %#lx or changed bytes", hr);
    ComPtr<ID3D11CommandList> list;
    CHECK(deferred->FinishCommandList(FALSE, &list));
    for (UINT replay = 0; replay < 2; replay++) {
      immediate->ExecuteCommandList(list.Get(), TRUE);
      if (!ready(recorded.Get()))
        return verdict();
      bytes.fill(sentinel);
      hr = deferred->GetData(recorded.Get(), bytes.data(), data_size, 0);
      expect(hr == DXGI_ERROR_INVALID_CALL && intact(), "executed deferred read: %#lx or changed bytes", hr);
    }
  }
  step("seed %u, empty deferred occlusion End without Begin", seed);
  D3D11_QUERY_DESC empty_desc{D3D11_QUERY_OCCLUSION};
  ComPtr<ID3D11Query> empty;
  CHECK(device->CreateQuery(&empty_desc, &empty));
  deferred->End(empty.Get());
  deferred->End(empty.Get());
  ComPtr<ID3D11CommandList> empty_list;
  CHECK(deferred->FinishCommandList(FALSE, &empty_list));
  immediate->ExecuteCommandList(empty_list.Get(), TRUE);
  if (!ready(empty.Get()))
    return verdict();
  UINT64 empty_count = ~UINT64(0);
  CHECK(immediate->GetData(empty.Get(), &empty_count, sizeof(empty_count), 0));
  expect(empty_count == 0, "empty deferred End returned %llu", empty_count);
  for (auto kind : {D3D11_QUERY_OCCLUSION, D3D11_QUERY_OCCLUSION_PREDICATE, D3D11_QUERY_PIPELINE_STATISTICS}) {
    const D3D11_QUERY_DESC desc{kind};
    ComPtr<ID3D11Query> query;
    CHECK(device->CreateQuery(&desc, &query));
    for (UINT flushes : {0u, 1u, 3u}) {
      step("empty query %u across %u flushes, reused", kind, flushes);
      immediate->Begin(query.Get());
      for (UINT i = 0; i < flushes; i++) {
        immediate->CopyResource(destination.Get(), source.Get());
        immediate->Flush();
      }
      immediate->End(query.Get());
      if (!ready(query.Get()))
        return verdict();
      bytes.fill(sentinel);
      CHECK(immediate->GetData(query.Get(), bytes.data(), query->GetDataSize(), 0));
      expect(std::all_of(bytes.begin(), bytes.begin() + query->GetDataSize(), [](auto value) { return value == 0; }),
             "empty query %u returned nonzero data", kind);
      expect(std::all_of(bytes.begin() + query->GetDataSize(), bytes.end(),
                         [&](auto value) { return value == sentinel; }), "empty query wrote past its data");
    }
  }
  step("seed %u, occlusion restart discards the first draw on both contexts", seed);
  const UINT width = 17, height = 13;
  D3D11_TEXTURE2D_DESC target_desc{width, height, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  target_desc.Usage = D3D11_USAGE_STAGING;
  target_desc.BindFlags = 0;
  target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &staging));
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11PixelShader> pixel;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vertex));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &pixel));
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));
  const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
  for (auto *context : {immediate.Get(), deferred.Get()}) {
    step("seed %u, nonempty restart on %s context", seed, context == immediate.Get() ? "immediate" : "deferred");
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->RSSetState(raster.Get());
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION};
    ComPtr<ID3D11Query> query, statistics;
    CHECK(device->CreateQuery(&desc, &query));
    D3D10_QUERY_DESC statistics_desc{D3D10_QUERY_PIPELINE_STATISTICS};
    ComPtr<ID3D10Query> statistics10;
    CHECK(device10->CreateQuery(&statistics_desc, &statistics10));
    CHECK(statistics10.As(&statistics));
    context->Begin(query.Get());
    context->Begin(statistics.Get());
    context->Draw(3, 0);
    context->Begin(query.Get());
    context->Begin(statistics.Get());
    const UINT draws = 1 + next() % 3;
    for (UINT i = 0; i < draws; i++)
      context->Draw(3, 0);
    context->End(query.Get());
    context->End(statistics.Get());
    if (context == deferred.Get()) {
      ComPtr<ID3D11CommandList> list;
      CHECK(deferred->FinishCommandList(FALSE, &list));
      immediate->ExecuteCommandList(list.Get(), TRUE);
    }
    if (!ready(query.Get()))
      return verdict();
    UINT64 count = ~UINT64(0);
    CHECK(immediate->GetData(query.Get(), &count, sizeof(count), 0));
    const UINT64 expected = UINT64(width) * height * draws;
    expect(count == expected, "%u retained draws: count %llu, want %llu", draws, count, expected);
    if (!ready(statistics.Get()))
      return verdict();
    D3D11_QUERY_DATA_PIPELINE_STATISTICS data{};
    if (!expect(read_statistics(statistics.Get(), data) == S_OK, "pipeline data is not ready"))
      return verdict();
    expect(data.IAVertices == UINT64(3) * draws, "pipeline IA vertices %llu for %u retained draws",
           data.IAVertices, draws);
    expect(data.IAPrimitives == draws, "pipeline IA primitives %llu for %u retained draws", data.IAPrimitives, draws);
    expect(data.VSInvocations == UINT64(3) * draws, "pipeline VS invocations %llu for %u retained draws",
           data.VSInvocations, draws);
    expect(data.CInvocations == draws && data.CPrimitives >= draws,
           "pipeline clipping counts differ for %u retained draws", draws);
    const UINT stamp_width = 2, stamp_height = 2, clipped_triangles = 2;
    const UINT64 maximum_pixels = UINT64((width + stamp_width - 1) / stamp_width) *
                                  ((height + stamp_height - 1) / stamp_height) * stamp_width * stamp_height *
                                  clipped_triangles * draws;
    expect(data.PSInvocations >= expected && data.PSInvocations <= maximum_pixels,
           "pipeline PS count %llu, want [%llu, %llu]", data.PSInvocations, expected, maximum_pixels);
    expect(!data.GSPrimitives && !data.HSInvocations && !data.DSInvocations && !data.CSInvocations,
           "pipeline counted an unused stage");
    struct {
      D3D10_QUERY_DATA_PIPELINE_STATISTICS data;
      UINT64 guard;
    } ten{};
    ten.guard = ~UINT64(0);
    if (!expect(statistics10->GetData(&ten.data, sizeof(ten.data), D3D10_ASYNC_GETDATA_DONOTFLUSH) == S_OK,
                "D3D10 pipeline data is not ready"))
      return verdict();
    expect(!memcmp(&ten.data, &data, sizeof(ten.data)) && ten.guard == ~UINT64(0),
           "D3D10 pipeline statistics differ or changed bytes beyond their size");
    immediate->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (UINT y = 0; y < height; y++) {
      auto row = reinterpret_cast<const UINT *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
      for (UINT x = 0; x < width; x++)
        expect(row[x] == 1, "pixel %u,%u: %u, want 1", x, y, row[x]);
    }
    immediate->Unmap(staging.Get(), 0);
  }
  constexpr std::array kinds{D3D11_QUERY_OCCLUSION, D3D11_QUERY_OCCLUSION_PREDICATE};
  std::array<ComPtr<ID3D11Query>, 2 * kinds.size()> unfinished;
  for (UINT i = 0; i < unfinished.size(); i++) {
    D3D11_QUERY_DESC desc{kinds[i % kinds.size()]};
    CHECK(device->CreateQuery(&desc, &unfinished[i]));
  }
  raster_desc.ScissorEnable = TRUE;
  CHECK(device->CreateRasterizerState(&raster_desc, raster.ReleaseAndGetAddressOf()));
  const D3D11_RECT full{0, 0, LONG(width), LONG(height)}, clipped{};
  for (BOOL restore : {FALSE, TRUE}) {
    for (UINT active : {0u, 1u, UINT(kinds.size()), UINT(unfinished.size())}) {
      step("seed %u, implicit End, restore %d, active %u: record", seed, restore, active);
      deferred->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      deferred->RSSetState(raster.Get());
      deferred->RSSetViewports(1, &viewport);
      deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      deferred->VSSetShader(vertex.Get(), nullptr, 0);
      deferred->PSSetShader(pixel.Get(), nullptr, 0);
      for (UINT i = 0; i < std::min(active, UINT(kinds.size())); i++)
        deferred->Begin(unfinished[i].Get());
      deferred->RSSetScissorRects(1, &full);
      deferred->Draw(3, 0);
      for (UINT i = kinds.size(); i < active; i++)
        deferred->Begin(unfinished[i].Get());
      deferred->RSSetScissorRects(1, &clipped);
      deferred->Draw(3, 0);
      ComPtr<ID3D11CommandList> list;
      CHECK(deferred->FinishCommandList(restore, &list));
      for (UINT replay = 0; replay < 2; replay++) {
        immediate->ExecuteCommandList(list.Get(), TRUE);
        for (UINT i = 0; i < active; i++) {
          step("seed %u, implicit End, restore %d, active %u, replay %u, query %u", seed, restore, active, replay, i);
          if (!ready(unfinished[i].Get()))
            return verdict();
          const UINT64 expected = i < kinds.size() ? UINT64(width) * height : 0;
          if (kinds[i % kinds.size()] == D3D11_QUERY_OCCLUSION_PREDICATE) {
            BOOL visible = -1;
            CHECK(immediate->GetData(unfinished[i].Get(), &visible, sizeof(visible), 0));
            expect(visible == (expected != 0), "implicit predicate %d, want %d", visible, expected != 0);
          } else {
            UINT64 count = ~UINT64(0);
            CHECK(immediate->GetData(unfinished[i].Get(), &count, sizeof(count), 0));
            expect(count == expected, "implicit count %llu, want %llu", count, expected);
          }
        }
      }
    }
  }
  constexpr UINT triangle_vertices = 3;
  const UINT indices[] = {0, 1, 2};
  D3D11_BUFFER_DESC index_desc{sizeof(indices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER};
  D3D11_SUBRESOURCE_DATA index_data{indices};
  ComPtr<ID3D11Buffer> index_buffer;
  CHECK(device->CreateBuffer(&index_desc, &index_data, &index_buffer));
  ComPtr<ID3D11DeviceContext> other;
  CHECK(device->CreateDeferredContext(0, &other));
  const std::array contexts{deferred.Get(), other.Get()};
  auto setup = [&](ID3D11DeviceContext *context) {
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->RSSetState(raster.Get());
    context->RSSetScissorRects(1, &full);
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetIndexBuffer(index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
  };
  auto counts = [&](ID3D11Query *query, UINT64 primitives) {
    if (!ready(query))
      return false;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS data{};
    HRESULT hr = read_statistics(query, data);
    bool valid = expect(hr == S_OK && data.IAVertices == primitives * triangle_vertices && data.IAPrimitives == primitives,
                        "statistics %#lx: IA vertices %llu, primitives %llu; want %llu, %llu", hr, data.IAVertices,
                        data.IAPrimitives, primitives * triangle_vertices, primitives);
    expect(data.VSInvocations == primitives * triangle_vertices && data.CInvocations == primitives &&
               data.CPrimitives >= primitives,
           "VS %llu, clipper %llu/%llu for %llu primitives", data.VSInvocations, data.CInvocations,
           data.CPrimitives, primitives);
    const UINT64 pixels = UINT64(width) * height * primitives;
    const UINT64 maximum = UINT64(2) * ((width + 1) / 2) * ((height + 1) / 2) * 4 * primitives;
    valid &= expect(data.PSInvocations >= pixels && data.PSInvocations <= maximum,
                    "PS invocations %llu, want [%llu, %llu]", data.PSInvocations, pixels, maximum);
    return valid;
  };
  const D3D11_QUERY_DESC query_desc{D3D11_QUERY_PIPELINE_STATISTICS};
  ComPtr<ID3D11Query> shared, outer, overlap;
  CHECK(device->CreateQuery(&query_desc, &shared));
  CHECK(device->CreateQuery(&query_desc, &outer));
  CHECK(device->CreateQuery(&query_desc, &overlap));
  std::array<ComPtr<ID3D11CommandList>, 2> lists;
  step("immediate statistics include a deferred list with no query of its own");
  setup(deferred.Get());
  const UINT list_draws = width * height;
  for (UINT i = 0; i < list_draws; i++)
    deferred->DrawInstanced(triangle_vertices, i ? 1 : 2, 0, 0);
  ComPtr<ID3D11CommandList> unqueried;
  CHECK(deferred->FinishCommandList(FALSE, &unqueried));
  for (UINT replay = 0; replay < 2; replay++) {
    immediate->Begin(outer.Get());
    immediate->ExecuteCommandList(unqueried.Get(), TRUE);
    immediate->End(outer.Get());
    counts(outer.Get(), list_draws + 1);
  }

  step("stream output and its rasterization count the input once");
  ComPtr<ID3D11GeometryShader> streamed;
  const D3D11_SO_DECLARATION_ENTRY position{0, "SV_Position", 0, 0, 4, 0};
  const UINT position_stride = 4 * sizeof(float);
  CHECK(device->CreateGeometryShaderWithStreamOutput(
      vs_code->GetBufferPointer(), vs_code->GetBufferSize(), &position, 1, &position_stride, 1, 0, nullptr, &streamed));
  auto positions = buffer(device.Get(), position_stride * triangle_vertices, D3D11_BIND_STREAM_OUTPUT);
  if (!expect(positions, "stream output buffer was not created"))
    return verdict();
  setup(immediate.Get());
  immediate->GSSetShader(streamed.Get(), nullptr, 0);
  const UINT output_offset = 0;
  immediate->SOSetTargets(1, positions.GetAddressOf(), &output_offset);
  immediate->Begin(outer.Get());
  immediate->Draw(triangle_vertices, 0);
  immediate->End(outer.Get());
  counts(outer.Get(), 1);
  immediate->GSSetShader(nullptr, nullptr, 0);
  immediate->SOSetTargets(0, nullptr, nullptr);

  step("two contexts interleave one query's recording");
  for (UINT i = 0; i < contexts.size(); i++) {
    setup(contexts[i]);
    contexts[i]->DrawInstanced(triangle_vertices, i + 1, 0, 0);
    contexts[i]->Begin(shared.Get());
    contexts[i]->DrawInstanced(triangle_vertices, i + 1, 0, 0);
  }
  for (UINT i = 0; i < contexts.size(); i++) {
    contexts[i]->End(shared.Get());
    CHECK(contexts[i]->FinishCommandList(FALSE, &lists[i]));
  }
  for (UINT replay = 0; replay < 3; replay++)
    for (UINT i : {1u, 0u}) {
      step("interleaved context %u replay %u", i, replay);
      immediate->Begin(outer.Get());
      immediate->ExecuteCommandList(lists[i].Get(), TRUE);
      immediate->End(outer.Get());
      counts(shared.Get(), i + 1);
      counts(outer.Get(), 2 * (i + 1));
    }

  step("seed %u, recording must not change an immediate range", seed);
  setup(immediate.Get());
  immediate->Begin(shared.Get());
  immediate->Draw(triangle_vertices, 0);
  setup(deferred.Get());
  deferred->Begin(shared.Get());
  deferred->DrawInstanced(triangle_vertices, 2, 0, 0);
  deferred->End(shared.Get());
  ComPtr<ID3D11CommandList> recorded_range;
  CHECK(deferred->FinishCommandList(FALSE, &recorded_range));
  immediate->Draw(triangle_vertices, 0);
  immediate->End(shared.Get());
  counts(shared.Get(), 2);
  immediate->ExecuteCommandList(recorded_range.Get(), TRUE);
  counts(shared.Get(), 2);

  const D3D11_QUERY_DESC predicate_desc{D3D11_QUERY_OCCLUSION_PREDICATE};
  ComPtr<ID3D11Predicate> predicate;
  CHECK(device->CreatePredicate(&predicate_desc, &predicate));
  for (bool seen : {false, true}) {
    setup(immediate.Get());
    immediate->Begin(predicate.Get());
    if (seen)
      immediate->Draw(triangle_vertices, 0);
    immediate->End(predicate.Get());
    if (!ready(predicate.Get()))
      return verdict();
    for (BOOL value : {FALSE, TRUE})
      for (bool recorded : {false, true}) {
        auto *context = recorded ? contexts[0] : immediate.Get();
        setup(context);
        context->Begin(shared.Get());
        context->Draw(triangle_vertices, 0);
        context->SetPredication(predicate.Get(), value);
        context->Begin(overlap.Get());
        context->Draw(triangle_vertices, 0);
        context->DrawIndexed(triangle_vertices, 0, 0);
        context->DrawInstanced(triangle_vertices, 2, 0, 0);
        context->DrawIndexedInstanced(triangle_vertices, 2, 0, 0, 0);
        context->End(overlap.Get());
        context->SetPredication(nullptr, FALSE);
        context->Draw(triangle_vertices, 0);
        context->End(shared.Get());
        ComPtr<ID3D11CommandList> list;
        if (recorded)
          CHECK(context->FinishCommandList(FALSE, &list));
        const UINT retained = BOOL(seen) != value ? 6 : 0;
        for (UINT replay = 0; replay < (recorded ? 3u : 1u); replay++) {
          step("seed %u, predicate %u value %d recorded %u replay %u", seed, seen, value, recorded, replay);
          if (recorded) {
            immediate->Begin(outer.Get());
            immediate->ExecuteCommandList(list.Get(), TRUE);
            immediate->End(outer.Get());
            counts(outer.Get(), retained + 2);
          }
          counts(shared.Get(), retained + 2);
          counts(overlap.Get(), retained);
        }
        if (recorded) {
          step("seed %u, nested list", seed);
          contexts[1]->Begin(outer.Get());
          contexts[1]->ExecuteCommandList(list.Get(), TRUE);
          contexts[1]->End(outer.Get());
          ComPtr<ID3D11CommandList> nested;
          CHECK(contexts[1]->FinishCommandList(FALSE, &nested));
          immediate->ExecuteCommandList(nested.Get(), TRUE);
          counts(outer.Get(), retained + 2);
          counts(shared.Get(), retained + 2);
        }
      }
  }
  for (BOOL restore : {FALSE, TRUE})
    for (UINT instances : {0u, 1u, 2u}) {
      step("seed %u, implicit statistics End, restore %d, instances %u", seed, restore, instances);
      setup(deferred.Get());
      deferred->Begin(shared.Get());
      deferred->DrawInstanced(triangle_vertices, instances, 0, 0);
      ComPtr<ID3D11CommandList> list;
      CHECK(deferred->FinishCommandList(restore, &list));
      immediate->ExecuteCommandList(list.Get(), TRUE);
      counts(shared.Get(), instances);
    }
  ComPtr<ID3D11PixelShader> depth_pixel;
  ComPtr<ID3D11VertexShader> strip_vertex;
  CHECK(device->CreatePixelShader(depth_code->GetBufferPointer(), depth_code->GetBufferSize(), nullptr, &depth_pixel));
  CHECK(
      device->CreateVertexShader(strip_code->GetBufferPointer(), strip_code->GetBufferSize(), nullptr, &strip_vertex));
  D3D11_DEPTH_STENCIL_DESC depth_state_desc{};
  depth_state_desc.DepthEnable = TRUE;
  depth_state_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  depth_state_desc.DepthFunc = D3D11_COMPARISON_LESS;
  ComPtr<ID3D11DepthStencilState> depth_state;
  CHECK(device->CreateDepthStencilState(&depth_state_desc, &depth_state));
  for (auto *context : {immediate.Get(), deferred.Get()}) {
    auto finish = [&] {
      if (context == deferred.Get()) {
        ComPtr<ID3D11CommandList> list;
        HRESULT hr = deferred->FinishCommandList(FALSE, &list);
        if (FAILED(hr))
          return expect(false, "FinishCommandList returned %#lx", hr);
        immediate->ExecuteCommandList(list.Get(), TRUE);
      }
      return ready(shared.Get());
    };
    for (UINT samples : {1u, 4u}) {
      D3D11_TEXTURE2D_DESC desc{
          width, height, 1, 1, DXGI_FORMAT_R32_UINT, {samples, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
      ComPtr<ID3D11Texture2D> color, depth;
      ComPtr<ID3D11RenderTargetView> color_view;
      ComPtr<ID3D11DepthStencilView> depth_view;
      CHECK(device->CreateTexture2D(&desc, nullptr, &color));
      CHECK(device->CreateRenderTargetView(color.Get(), nullptr, &color_view));
      desc.Format = DXGI_FORMAT_D32_FLOAT;
      desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
      CHECK(device->CreateTexture2D(&desc, nullptr, &depth));
      CHECK(device->CreateDepthStencilView(depth.Get(), nullptr, &depth_view));
      for (UINT shader_kind = 0; shader_kind < 3; shader_kind++)
        for (bool passes : {false, true}) {
          step("%s, %u samples, shader %u, depth passes %u", context == immediate.Get() ? "immediate" : "deferred",
               samples, shader_kind, passes);
          setup(context);
          context->OMSetRenderTargets(1, color_view.GetAddressOf(), depth_view.Get());
          context->OMSetDepthStencilState(depth_state.Get(), 0);
          context->ClearDepthStencilView(depth_view.Get(), D3D11_CLEAR_DEPTH, passes ? 1.0f : 0.0f, 0);
          context->PSSetShader(shader_kind == 0   ? nullptr
                               : shader_kind == 1 ? pixel.Get()
                                                  : depth_pixel.Get(),
                               nullptr, 0);
          context->Begin(shared.Get());
          context->Draw(triangle_vertices, 0);
          context->End(shared.Get());
          if (!finish())
            return verdict();
          D3D11_QUERY_DATA_PIPELINE_STATISTICS got{};
          CHECK(read_statistics(shared.Get(), got));
          const UINT64 covered = UINT64(width) * height;
          const UINT64 minimum = shader_kind && (passes || shader_kind == 2) ? covered : 0;
          const UINT64 maximum = shader_kind ? 2 * ((width + 1) / 2) * ((height + 1) / 2) * 4 : 0;
          expect(got.PSInvocations >= minimum && got.PSInvocations <= maximum, "PS invocations %llu, want [%llu, %llu]",
                 got.PSInvocations, minimum, maximum);
        }
    }
    for (DXGI_FORMAT format : {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R32_UINT})
      for (auto topology : {D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP})
        for (UINT strips : {1u, 2u, 19u})
          for (UINT edge_cuts : {0u, 1u, 2u, 3u}) {
            const UINT cut =
                format == DXGI_FORMAT_R16_UINT ? D3D11_16BIT_INDEX_STRIP_CUT_VALUE : D3D11_32BIT_INDEX_STRIP_CUT_VALUE;
            const UINT buffer_offset_indices = 2, start_index = 3,
                       leading_indices = buffer_offset_indices + start_index;
            std::vector<UINT> index(leading_indices, cut);
            UINT64 primitives = 0;
            if (edge_cuts & 1)
              index.push_back(cut);
            for (UINT strip = 0; strip < strips; strip++) {
              if (strip) {
                index.push_back(cut);
                if (strip % 2)
                  index.push_back(cut);
              }
              const UINT vertices = 3 + (strip ? strip % 3 : width * height);
              for (UINT v = 0; v < vertices; v++)
                index.push_back(v % 4);
              primitives += vertices - (topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP ? 1 : 2);
            }
            if (edge_cuts & 2)
              index.push_back(cut);
            std::vector<UINT16> narrow(index.begin(), index.end());
            const UINT stride = format == DXGI_FORMAT_R16_UINT ? sizeof(UINT16) : sizeof(UINT);
            D3D11_BUFFER_DESC desc{UINT(index.size()) * stride, D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER};
            D3D11_SUBRESOURCE_DATA data{format == DXGI_FORMAT_R16_UINT ? (void *)narrow.data() : (void *)index.data()};
            ComPtr<ID3D11Buffer> ib;
            CHECK(device->CreateBuffer(&desc, &data, &ib));
            for (UINT instances : {1u, 3u})
              for (bool indirect : {false, true}) {
                step("%s, format %u, topology %u, strips %u, edge cuts %u, instances %u, indirect %u",
                     context == immediate.Get() ? "immediate" : "deferred", format, topology, strips, edge_cuts,
                     instances, indirect);
                const D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS args{UINT(index.size()) - leading_indices, instances,
                                                                      start_index};
                D3D11_BUFFER_DESC args_desc{sizeof(args), D3D11_USAGE_DEFAULT};
                args_desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
                D3D11_SUBRESOURCE_DATA args_data{&args};
                ComPtr<ID3D11Buffer> arguments;
                CHECK(device->CreateBuffer(&args_desc, &args_data, &arguments));
                setup(context);
                context->OMSetDepthStencilState(nullptr, 0);
                context->IASetIndexBuffer(ib.Get(), format, buffer_offset_indices * stride);
                context->IASetPrimitiveTopology(topology);
                context->VSSetShader(strip_vertex.Get(), nullptr, 0);
                context->Begin(shared.Get());
                if (indirect)
                  context->DrawIndexedInstancedIndirect(arguments.Get(), 0);
                else if (instances == 1)
                  context->DrawIndexed(index.size() - leading_indices, start_index, 0);
                else
                  context->DrawIndexedInstanced(index.size() - leading_indices, instances, start_index, 0, 0);
                const D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS empty{};
                context->UpdateSubresource(arguments.Get(), 0, nullptr, &empty, 0, 0);
                context->End(shared.Get());
                if (!finish())
                  return verdict();
                D3D11_QUERY_DATA_PIPELINE_STATISTICS got{};
                CHECK(read_statistics(shared.Get(), got));
                const UINT64 expected = primitives * instances;
                expect(got.IAPrimitives == expected && got.CInvocations == expected,
                       "strip primitives IA %llu clipper %llu, want %llu", got.IAPrimitives, got.CInvocations,
                       expected);
                if (topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
                  expect(got.PSInvocations >= UINT64(width / 2) * (height / 2) * strips * instances,
                         "strip draw lost pixel invocations: %llu", got.PSInvocations);
              }
          }
  }
  ComPtr<ID3D11VertexShader> stats_vertex;
  CHECK(device->CreateVertexShader(stats_vs_code->GetBufferPointer(), stats_vs_code->GetBufferSize(), nullptr,
                                    &stats_vertex));
  constexpr UINT stream_stride = 4 * sizeof(UINT), sentinel_word = 0xa5a5a5a5, prefix_words = 1;
  for (UINT mode = 0; mode < std::size(stream_counts); mode++) {
    const UINT streams = stream_counts[mode], gs_instances = geometry_instances[mode];
    std::vector<D3D11_SO_DECLARATION_ENTRY> entries;
    std::vector<UINT> strides(streams, stream_stride);
    for (UINT stream = 0; stream < streams; stream++)
      entries.push_back({stream, "VALUE", 0, 0, 4, BYTE(stream)});
    ComPtr<ID3D11GeometryShader> geometry;
    CHECK(device->CreateGeometryShaderWithStreamOutput(
        stats_gs_code[mode]->GetBufferPointer(), stats_gs_code[mode]->GetBufferSize(), entries.data(), entries.size(),
        strides.data(), strides.size(), D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &geometry));
    for (auto context : {immediate.Get(), deferred.Get()})
      for (UINT points : {0u, 1u, max_gs_instances - 1, max_gs_instances, max_gs_instances + 1})
        for (UINT instances : {1u, 3u})
          for (UINT short_stream = 0; short_stream <= streams; short_stream++) {
            const UINT generated = points * instances * gs_instances;
            step("%s GS instances %u, streams %u, points %u, draw instances %u, short stream %u",
                 context == immediate.Get() ? "immediate" : "deferred", gs_instances, streams, points,
                 instances, short_stream);
            ComPtr<ID3D11Buffer> output[D3D11_SO_STREAM_COUNT];
            ID3D11Buffer *targets[D3D11_SO_STREAM_COUNT]{};
            UINT offsets[D3D11_SO_STREAM_COUNT]{};
            UINT capacity[D3D11_SO_STREAM_COUNT]{};
            for (UINT stream = 0; stream < streams; stream++) {
              capacity[stream] = stream == short_stream ? generated / 2 : generated;
              const UINT words = prefix_words + capacity[stream] * stream_stride / sizeof(UINT) + 1;
              output[stream] = buffer(device.Get(), words * sizeof(UINT), D3D11_BIND_STREAM_OUTPUT, sentinel_word);
              if (!expect(!!output[stream], "SO buffer %u was not created", stream))
                return verdict();
              targets[stream] = output[stream].Get();
              offsets[stream] = prefix_words * sizeof(UINT);
            }
            std::array<ComPtr<ID3D11Query>, 2 * D3D11_SO_STREAM_COUNT + 2> so_queries;
            for (UINT q = 0; q < so_queries.size(); q++) {
              const D3D11_QUERY type = q < 2 * D3D11_SO_STREAM_COUNT
                  ? D3D11_QUERY(D3D11_QUERY_SO_STATISTICS_STREAM0 + q)
                  : D3D11_QUERY(D3D11_QUERY_SO_STATISTICS + q - 2 * D3D11_SO_STREAM_COUNT);
              const D3D11_QUERY_DESC desc{type};
              CHECK(device->CreateQuery(&desc, &so_queries[q]));
            }
            context->ClearState();
            context->VSSetShader(stats_vertex.Get(), nullptr, 0);
            context->GSSetShader(geometry.Get(), nullptr, 0);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            context->SOSetTargets(streams, targets, offsets);
            context->Begin(shared.Get());
            for (auto &query : so_queries)
              context->Begin(query.Get());
            context->DrawInstanced(points, instances, 0, 0);
            for (auto &query : so_queries)
              context->End(query.Get());
            context->End(shared.Get());
            context->SOSetTargets(0, nullptr, nullptr);
            if (context == deferred.Get()) {
              ComPtr<ID3D11CommandList> commands;
              CHECK(deferred->FinishCommandList(FALSE, &commands));
              immediate->ExecuteCommandList(commands.Get(), FALSE);
            }
            if (!ready(shared.Get()))
              return verdict();
            D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
            CHECK(read_statistics(shared.Get(), stats));
            expect(stats.IAVertices == UINT64(points) * instances && stats.IAPrimitives == UINT64(points) * instances,
                   "point IA counts %llu/%llu", stats.IAVertices, stats.IAPrimitives);
            expect(stats.GSInvocations == generated, "GSInvocations %llu, want %u", stats.GSInvocations, generated);
            if (streams == 1)
              expect(stats.GSPrimitives == generated, "GSPrimitives %llu, want %u", stats.GSPrimitives, generated);
            expect(!stats.CInvocations && !stats.CPrimitives && !stats.PSInvocations && !stats.HSInvocations &&
                       !stats.DSInvocations && !stats.CSInvocations, "rasterization or an unused stage counted");
            for (UINT q = 0; q < so_queries.size(); q++) {
              if (!ready(so_queries[q].Get()))
                return verdict();
              const UINT stream = q < 2 * D3D11_SO_STREAM_COUNT ? q / 2 : 0;
              const UINT needed = stream < streams ? generated : 0;
              const UINT written = stream < streams ? capacity[stream] : 0;
              if (!(q % 2)) {
                D3D11_QUERY_DATA_SO_STATISTICS got{};
                CHECK(immediate->GetData(so_queries[q].Get(), &got, sizeof(got), 0));
                expect(got.NumPrimitivesWritten == written && got.PrimitivesStorageNeeded == needed,
                       "SO query %u written/needed %llu/%llu, want %u/%u", q, got.NumPrimitivesWritten,
                       got.PrimitivesStorageNeeded, written, needed);
              } else {
                BOOL got = FALSE;
                CHECK(immediate->GetData(so_queries[q].Get(), &got, sizeof(got), 0));
                const bool aggregate = q == so_queries.size() - 1;
                const bool want = generated && short_stream < streams && (aggregate || stream == short_stream);
                expect(got == BOOL(want), "SO predicate %u: %u, want %u", q, got, want);
              }
            }
            for (UINT stream = 0; stream < streams; stream++) {
              auto got = read(device.Get(), immediate.Get(), output[stream].Get());
              const UINT words = prefix_words + capacity[stream] * stream_stride / sizeof(UINT) + 1;
              if (!expect(got.size() == words, "SO readback size for stream %u", stream))
                return verdict();
              for (UINT word = 0; word < got.size(); word++) {
                UINT want = sentinel_word;
                if (word >= prefix_words && word < words - 1) {
                  const UINT ordinal = (word - prefix_words) / 4, component = (word - prefix_words) % 4;
                  const UINT record[] = {ordinal / gs_instances % points,
                                         ordinal / (gs_instances * points),
                                         ordinal % gs_instances, stream};
                  want = record[component];
                }
                if (!expect(got[word] == want, "stream %u word %u: %#x, want %#x", stream, word, got[word], want))
                  break;
              }
            }
          }
  }
  return verdict();
}
