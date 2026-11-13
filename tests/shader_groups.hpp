// contract: "x*y*z <= 1024" (D3D11.3 22.3.40), with x/y up to1024 and z up to64. TGSM declarations share a
// "32kB" limit (22.3.45-46). "All prior g# memory reads/writes by this thread in program order are made visible
// to all threads in the thread group before any subsequent g# memory accesses by this thread." (22.17.7).
// fill every word, exchange the first and last words across threads, and repeat with different data. check every
// word and all four thread/group system values; separate groups must never share storage.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace shader_groups
{
constexpr uint32_t components = 4, rounds = 2;
struct Shape {
  uint32_t x, y, z;
};
inline std::vector<Shape>
shapes(uint32_t xy_limit, uint32_t z_limit, uint32_t thread_limit)
{
  return {{1, 1, 1},
          {xy_limit - 1, 1, 1},
          {xy_limit, 1, 1},
          {1, xy_limit - 1, 1},
          {1, xy_limit, 1},
          {1, 1, z_limit - 1},
          {1, 1, z_limit},
          {thread_limit / z_limit, 1, z_limit},
          {1, thread_limit / z_limit, z_limit},
          {z_limit, thread_limit / z_limit, 1}};
}
constexpr Shape groups{2, 2, 2};
constexpr uint32_t group_count = groups.x * groups.y * groups.z;

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint4> results : register(u0);
groupshared uint shared_words[WORDS];
[numthreads(X, Y, Z)]
void cs(uint3 local_id : SV_GroupThreadID, uint3 group_id : SV_GroupID,
        uint3 global_id : SV_DispatchThreadID, uint flat : SV_GroupIndex) {
  uint g = group_id.x + GX * (group_id.y + GY * group_id.z);
  for (uint turn = 0; turn < ROUNDS; turn++) {
    for (uint k = flat; k < WORDS; k += X * Y * Z)
      shared_words[k] = (g * ROUNDS + turn + 1) * WORDS + k;
    GroupMemoryBarrierWithGroupSync();
    for (uint k = flat; k < WORDS; k += X * Y * Z) {
      uint source = WORDS - 1 - k;
      results[(g * ROUNDS + turn) * WORDS + k] = uint4(shared_words[source], flat,
        local_id.x + X * (local_id.y + Y * local_id.z),
        global_id.x + X * GX * (global_id.y + Y * GY * global_id.z));
    }
    GroupMemoryBarrierWithGroupSync();
  }
}
)hlsl";

// Metal's shader validation keeps a record beside threadgroup memory, so a kernel's doubles under it (measured: 4096
// words take 32768 bytes, 8191 take 65532), and a group past half of the limit, also Metal's maximum, cannot run there
inline bool
runs(uint32_t words, uint32_t limit_words)
{
  auto validation = getenv("MTL_SHADER_VALIDATION");
  return !validation || !strcmp(validation, "0") || 2 * words <= limit_words;
}

// the verdict, or a skip when every group that ran was right and some could not run
inline int
verdict(unsigned skipped)
{
  if (!skipped || trace::wrong)
    return ::verdict();
  printf("skipped: %u groups past half of threadgroup memory, which Metal's shader validation doubles\n", skipped);
  return 77;
}

inline std::vector<std::string>
defines(Shape s, uint32_t words)
{
  return {"X=" + std::to_string(s.x),        "Y=" + std::to_string(s.y),       "Z=" + std::to_string(s.z),
          "GX=" + std::to_string(groups.x),  "GY=" + std::to_string(groups.y), "WORDS=" + std::to_string(words),
          "ROUNDS=" + std::to_string(rounds)};
}

inline void
check(const uint32_t *got, Shape s, uint32_t words)
{
  const uint32_t threads = s.x * s.y * s.z;
  unsigned wrong = 0;
  for (uint32_t g = 0; g < group_count; g++)
    for (uint32_t pass = 0; pass < rounds; pass++)
      for (uint32_t k = 0; k < words; k++) {
        uint32_t flat = k % threads;
        Shape local{flat % s.x, flat / s.x % s.y, flat / (s.x * s.y)};
        Shape group{g % groups.x, g / groups.x % groups.y, g / (groups.x * groups.y)};
        const uint32_t global_flat =
            local.x + s.x * group.x +
            s.x * groups.x * (local.y + s.y * group.y + s.y * groups.y * (local.z + s.z * group.z));
        const uint32_t want[] = {(g * rounds + pass + 1) * words + words - 1 - k, flat, flat, global_flat};
        const uint32_t at = ((g * rounds + pass) * words + k) * components;
        for (uint32_t c = 0; c < components; c++)
          if (got[at + c] != want[c] && wrong++ < components)
            expect(false, "group %u pass %u word %u component %u: %#x, want %#x", g, pass, k, c, got[at + c], want[c]);
      }
  expect(wrong == 0, "%u incorrect shared-memory/system-value results", wrong);
}
} // namespace shader_groups
