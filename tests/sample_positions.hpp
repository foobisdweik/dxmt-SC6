// contract: samplepos must return zero for an invalid sample or an unbound view: "If sampleIndex is out of bounds
// a zero vector is returned." "If there is no resource bound to the specified slot, 0 is returned."
// (D3D11.3 22.4.22). query the count, then test its first two invalid indices and UINT_MAX. the index and count
// travel beside the result so an unwritten output or an incorrect sampleinfo result cannot look like success.
#pragma once
#include <cstdint>

namespace sample_positions
{
constexpr unsigned records = 3, components = 4, bytes = records * components * sizeof(uint32_t);
static const char hlsl[] = R"hlsl(
Texture2DMS<float> texels : register(t0);
RWStructuredBuffer<uint4> results : register(u0);
[numthreads(1, 1, 1)] void cs() {
  uint width, height, count;
  texels.GetDimensions(width, height, count);
  uint indices[3] = {count, count + 1, ~0u};
  for (uint i = 0; i < 3; i++)
    results[i] = uint4(asuint(texels.GetSamplePosition(indices[i])), indices[i], count);
}
)hlsl";

inline void
check(const uint32_t *got, uint32_t count)
{
  const uint32_t indices[] = {count, count + 1, UINT32_MAX};
  for (unsigned i = 0; i < records; i++) {
    expect(got[i * components] == 0 && got[i * components + 1] == 0,
           "sample %u of %u: position bits (%#x, %#x), want zero", indices[i], count, got[i * components],
           got[i * components + 1]);
    expect(got[i * components + 2] == indices[i] && got[i * components + 3] == count,
           "sample %u of %u: index %#x, count %u", indices[i], count, got[i * components + 2], got[i * components + 3]);
  }
}
} // namespace sample_positions
