// contract: UAV atomics act once per thread and return the value before the operation: "Returns value in memory
// before the AND." (D3D11.3 22.17.19); "Returns value in memory before the max operation." (22.17.25).
// the operations are and, xor, signed and unsigned min and max, compare-store and exchange, and
// Consume of a consume buffer takes each element once (D3D11.3 7.14.7). 64 threads use each one on one word, so the
// word ends the same in any order: and clears every bit, xor of the first 40 threads leaves the top 24 bits, min and
// max end at the extremes, compare-store succeeds once, and the exchanged values and the last one are 0 to 64.
#include "d3d11_test.hpp"
#include <algorithm>
#include <climits>

static const char hlsl[] = R"hlsl(
RWByteAddressBuffer words : register(u0);
RWStructuredBuffer<uint> originals : register(u1);
ConsumeStructuredBuffer<uint> queue : register(u2);
RWStructuredBuffer<uint> consumed : register(u3);
RWStructuredBuffer<int> signed_words : register(u4);

[numthreads(64, 1, 1)]
void cs(uint i : SV_GroupIndex) {
  uint bit = 1u << (i % 32);
  // without the original value
  words.InterlockedAnd(0, ~bit);
  if (i < 40)
    words.InterlockedXor(4, bit);
  words.InterlockedMin(8, i + 5);
  words.InterlockedMax(12, i + 5);
  words.InterlockedCompareStore(16, i, i + 100);
  InterlockedMin(signed_words[0], int(i) - 32);
  InterlockedMax(signed_words[1], int(i) - 32);
  // with the original value
  uint o0, o1, o2, o3, o4;
  int o5, o6;
  words.InterlockedAnd(20, ~bit, o0);
  o1 = 0;
  if (i < 40)
    words.InterlockedXor(24, bit, o1);
  words.InterlockedMin(28, i + 5, o2);
  words.InterlockedMax(32, i + 5, o3);
  words.InterlockedExchange(36, i + 1, o4);
  InterlockedMin(signed_words[2], int(i) - 32, o5);
  InterlockedMax(signed_words[3], int(i) - 32, o6);
  originals[i * 8 + 0] = o0;
  originals[i * 8 + 1] = o1;
  originals[i * 8 + 2] = o2;
  originals[i * 8 + 3] = o3;
  originals[i * 8 + 4] = o4;
  originals[i * 8 + 5] = asuint(o5);
  originals[i * 8 + 6] = asuint(o6);
  consumed[i] = queue.Consume();
}
)hlsl";

int
main() {
  auto cs = compile(hlsl, "cs", "cs");
  if (!cs) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  ComPtr<ID3D11ComputeShader> shader;
  CHECK(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &shader));

  const UINT threads = 64;
  // the words' starting values: and, xor, umin, umax, compare-store, and, xor, umin, umax, exchange
  const uint32_t start[] = {~0u, 0, ~0u, 0, 7, ~0u, 0, ~0u, 0, 0};
  const int32_t signed_start[] = {0, -100, 0, -100};
  std::vector<uint32_t> queue_values(threads);
  for (UINT k = 0; k < threads; k++)
    queue_values[k] = 1000 + k;

  // a UAV buffer, or none when it cannot be made, which the view made of it then reports
  auto make = [&](UINT bytes, UINT stride, const void *data, UINT misc) -> ComPtr<ID3D11Buffer> {
    D3D11_BUFFER_DESC desc{bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, misc, stride};
    D3D11_SUBRESOURCE_DATA initial{data};
    ComPtr<ID3D11Buffer> out;
    device->CreateBuffer(&desc, data ? &initial : nullptr, &out);
    return out;
  };
  auto words = make(sizeof(start), 0, start, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
  auto originals = make(threads * 8 * 4, 4, nullptr, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
  auto queue = make(threads * 4, 4, queue_values.data(), D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
  auto consumed = make(threads * 4, 4, nullptr, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
  auto signed_words = make(sizeof(signed_start), 4, signed_start, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);

  ComPtr<ID3D11UnorderedAccessView> uavs[5];
  D3D11_UNORDERED_ACCESS_VIEW_DESC raw{DXGI_FORMAT_R32_TYPELESS, D3D11_UAV_DIMENSION_BUFFER};
  raw.Buffer = {0, UINT(std::size(start)), D3D11_BUFFER_UAV_FLAG_RAW};
  CHECK(device->CreateUnorderedAccessView(words.Get(), &raw, &uavs[0]));
  CHECK(device->CreateUnorderedAccessView(originals.Get(), nullptr, &uavs[1]));
  D3D11_UNORDERED_ACCESS_VIEW_DESC append{DXGI_FORMAT_UNKNOWN, D3D11_UAV_DIMENSION_BUFFER};
  append.Buffer = {0, threads, D3D11_BUFFER_UAV_FLAG_APPEND};
  CHECK(device->CreateUnorderedAccessView(queue.Get(), &append, &uavs[2]));
  CHECK(device->CreateUnorderedAccessView(consumed.Get(), nullptr, &uavs[3]));
  CHECK(device->CreateUnorderedAccessView(signed_words.Get(), nullptr, &uavs[4]));

  step("64 threads of atomics without and with the original value, and Consume");
  ID3D11UnorderedAccessView *views[] = {uavs[0].Get(), uavs[1].Get(), uavs[2].Get(), uavs[3].Get(), uavs[4].Get()};
  // the consume buffer's counter starts at its element count
  const UINT counts[] = {~0u, ~0u, threads, ~0u, ~0u};
  context->CSSetShader(shader.Get(), nullptr, 0);
  context->CSSetUnorderedAccessViews(0, std::size(views), views, counts);
  context->Dispatch(1, 1, 1);

  auto got = read(device.Get(), context.Get(), words.Get());
  auto got_signed = read(device.Get(), context.Get(), signed_words.Get());
  auto got_originals = read(device.Get(), context.Get(), originals.Get());
  auto got_consumed = read(device.Get(), context.Get(), consumed.Get());
  if (!expect(got.size() == std::size(start) && got_signed.size() == 4 && got_originals.size() == threads * 8 &&
                  got_consumed.size() == threads,
              "readback sizes"))
    return verdict();

  const char *names[] = {"and",      "xor",       "umin",      "umax",      "compare-store",
                         "and with original", "xor with original", "umin with original", "umax with original"};
  const uint32_t want[] = {0, 0xffffff00u, 5, 68, 107, 0, 0xffffff00u, 5, 68};
  for (UINT k = 0; k < std::size(want); k++)
    expect(got[k] == want[k], "%s: %#x, want %#x", names[k], got[k], want[k]);
  const char *signed_names[] = {"imin", "imax", "imin with original", "imax with original"};
  const int32_t signed_want[] = {-32, 31, -32, 31};
  for (UINT k = 0; k < 4; k++)
    expect(int32_t(got_signed[k]) == signed_want[k], "%s: %d, want %d", signed_names[k], int32_t(got_signed[k]),
           signed_want[k]);

  // the exchanged originals and the word's last value are 0 to 64, each once
  std::vector<uint32_t> chain{got[9]};
  for (UINT i = 0; i < threads; i++)
    chain.push_back(got_originals[i * 8 + 4]);
  std::sort(chain.begin(), chain.end());
  for (UINT k = 0; k <= threads; k++)
    expect(chain[k] == k, "exchange: value %u of the chain is %u, want %u", k, chain[k], k);
  // each change from an original to its result must add up to the word's total change, in any thread order
  int64_t deltas[6] = {};
  for (UINT i = 0; i < threads; i++) {
    auto original = &got_originals[i * 8];
    uint32_t bit = 1u << (i % (sizeof(uint32_t) * CHAR_BIT));
    deltas[0] += int64_t(original[0] & ~bit) - original[0];
    if (i < 40)
      deltas[1] += int64_t(original[1] ^ bit) - original[1];
    deltas[2] += int64_t(std::min<uint32_t>(original[2], i + 5)) - original[2];
    deltas[3] += int64_t(std::max<uint32_t>(original[3], i + 5)) - original[3];
    deltas[4] += int64_t(std::min<int32_t>(int32_t(original[5]), int32_t(i) - 32)) - int32_t(original[5]);
    deltas[5] += int64_t(std::max<int32_t>(int32_t(original[6]), int32_t(i) - 32)) - int32_t(original[6]);
    expect(got_originals[i * 8 + 2] >= 5, "umin original of thread %u: %u", i, got_originals[i * 8 + 2]);
    expect(got_originals[i * 8 + 3] <= 68, "umax original of thread %u: %u", i, got_originals[i * 8 + 3]);
    expect(int32_t(got_originals[i * 8 + 5]) >= -32, "imin original of thread %u: %d", i, int32_t(got_originals[i * 8 + 5]));
    expect(int32_t(got_originals[i * 8 + 6]) <= 31, "imax original of thread %u: %d", i, int32_t(got_originals[i * 8 + 6]));
  }
  const int64_t want_deltas[] = {int64_t(got[5]) - start[5], int64_t(got[6]) - start[6],
                               int64_t(got[7]) - start[7], int64_t(got[8]) - start[8],
                               int64_t(int32_t(got_signed[2])) - signed_start[2],
                               int64_t(int32_t(got_signed[3])) - signed_start[3]};
  const char *original_names[] = {"and", "xor", "umin", "umax", "imin", "imax"};
  for (UINT k = 0; k < std::size(deltas); k++)
    expect(deltas[k] == want_deltas[k], "%s originals: total change %lld, want %lld", original_names[k],
           (long long)deltas[k], (long long)want_deltas[k]);
  std::vector<uint32_t> taken(got_consumed.begin(), got_consumed.end());
  std::sort(taken.begin(), taken.end());
  for (UINT k = 0; k < threads; k++)
    expect(taken[k] == 1000 + k, "consume: element %u taken is %u, want %u", k, taken[k], 1000 + k);
  return verdict();
}
