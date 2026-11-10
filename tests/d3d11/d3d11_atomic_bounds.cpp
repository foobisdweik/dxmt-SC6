// contract: an atomic past a UAV's end writes nothing. "Out of bounds addressing on u# causes nothing to be written to
// memory" (D3D11.3 22.17, atomic_* and imm_atomic_*; the value returned is undefined, so only memory is checked). the
// views are windows inside larger buffers, so a write past one lands in words the test reads back; unbound UAVs take
// the same atomics, and an unbound constant buffer and SRV read after them must still hold nothing (D3D11.3 7.5,
// 22.4.10)
#include "d3d11_test.hpp"

static constexpr UINT threads = 64, first = 4, inside = 37; // a SIMD group and a part, a view of an odd length
static constexpr UINT words = first + threads + 4, initial = 0x1000;
static const char hlsl[] = R"hlsl(
RWByteAddressBuffer raw : register(u0);
RWStructuredBuffer<uint> structured : register(u1);
RWByteAddressBuffer unbound_raw : register(u2);
RWStructuredBuffer<uint> unbound_structured : register(u3);
cbuffer Unbound : register(b0) { uint4 constants[THREADS / 4]; };
ByteAddressBuffer unbound_srv : register(t0);
RWStructuredBuffer<uint> results : register(u4);
[numthreads(THREADS, 1, 1)] void atomics(uint id : SV_GroupIndex) {
  uint ignored, before = INITIAL + FIRST + id;
  // each kind of atomic alone changes a word it reaches past the end
  raw.InterlockedCompareExchange(id * 4, before, before ^ 0xc0de0000, ignored);
  raw.InterlockedAdd(id * 4, 1);
  InterlockedCompareExchange(structured[id], before, before ^ 0xc0de0000, ignored);
  InterlockedAdd(structured[id], 1);
  unbound_raw.InterlockedCompareExchange(id * 4, 0, 7, ignored);
  unbound_raw.InterlockedAdd(id * 4, 1);
  InterlockedCompareExchange(unbound_structured[id], 0, 7, ignored);
  InterlockedAdd(unbound_structured[id], 1);
}
[numthreads(THREADS, 1, 1)] void reads(uint id : SV_GroupIndex) {
  results[id] = constants[id / 4][id % 4] | unbound_srv.Load(id * 4);
}
)hlsl";

int
main() {
  const std::vector<std::string> defines = {"THREADS=" + std::to_string(threads), "FIRST=" + std::to_string(first),
                                            "INITIAL=" + std::to_string(initial)};
  auto atomics_code = compile(hlsl, "atomics", "cs", defines), reads_code = compile(hlsl, "reads", "cs", defines);
  if (!atomics_code || !reads_code) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  ComPtr<ID3D11ComputeShader> atomics, reads;
  CHECK(device->CreateComputeShader(atomics_code->GetBufferPointer(), atomics_code->GetBufferSize(), nullptr, &atomics));
  CHECK(device->CreateComputeShader(reads_code->GetBufferPointer(), reads_code->GetBufferSize(), nullptr, &reads));
  std::vector<uint32_t> data(words);
  for (UINT i = 0; i < words; i++)
    data[i] = initial + i;
  ComPtr<ID3D11Buffer> targets[2], results;
  ComPtr<ID3D11UnorderedAccessView> views[3];
  for (UINT kind = 0; kind < 2; kind++) {
    const bool structured = kind;
    D3D11_BUFFER_DESC desc{words * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                           structured ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED : D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
                           structured ? 4u : 0u};
    D3D11_SUBRESOURCE_DATA initial_data{data.data()};
    CHECK(device->CreateBuffer(&desc, &initial_data, &targets[kind]));
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS,
                                          D3D11_UAV_DIMENSION_BUFFER};
    view.Buffer = {first, inside, structured ? 0u : D3D11_BUFFER_UAV_FLAG_RAW};
    CHECK(device->CreateUnorderedAccessView(targets[kind].Get(), &view, &views[kind]));
  }
  D3D11_BUFFER_DESC results_desc{threads * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                 D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
  CHECK(device->CreateBuffer(&results_desc, nullptr, &results));
  CHECK(device->CreateUnorderedAccessView(results.Get(), nullptr, &views[2]));

  step("atomics inside, at and past the end of a raw and a structured view, and on unbound views");
  ID3D11UnorderedAccessView *bound[] = {views[0].Get(), views[1].Get(), nullptr, nullptr, nullptr};
  context->CSSetShader(atomics.Get(), nullptr, 0);
  context->CSSetUnorderedAccessViews(0, std::size(bound), bound, nullptr);
  context->Dispatch(1, 1, 1);
  for (UINT kind = 0; kind < 2; kind++) {
    auto got = read(device.Get(), context.Get(), targets[kind].Get());
    if (!expect(got.size() == words, "readback size"))
      continue;
    for (UINT i = 0; i < words; i++) {
      const uint32_t want = i >= first && i < first + inside ? (data[i] ^ 0xc0de0000) + 1 : data[i];
      expect(got[i] == want, "%s word %u: %08x, expected %08x", kind ? "structured" : "raw", i, got[i], want);
    }
  }

  step("an unbound constant buffer and SRV after the atomics");
  ID3D11UnorderedAccessView *output[] = {nullptr, nullptr, nullptr, nullptr, views[2].Get()};
  ID3D11Buffer *no_constants = nullptr;
  ID3D11ShaderResourceView *no_srv = nullptr;
  context->CSSetShader(reads.Get(), nullptr, 0);
  context->CSSetUnorderedAccessViews(0, std::size(output), output, nullptr);
  context->CSSetConstantBuffers(0, 1, &no_constants);
  context->CSSetShaderResources(0, 1, &no_srv);
  context->Dispatch(1, 1, 1);
  auto got = read(device.Get(), context.Get(), results.Get());
  if (expect(got.size() == threads, "readback size"))
    for (UINT i = 0; i < threads; i++)
      expect(!got[i], "unbound word %u: %08x", i, got[i]);
  return verdict();
}
