// shared by d3d11_viewports and d3d12_viewports: the cases and what they must draw.
// contract: viewports and scissors are each set all at once, and what a call does not set is not there: a viewport of
// no size, and an empty scissor (D3D11.3 15.6, 15.7: "the default Scissor Rectangle is an empty Scissor Rectangle").
// a primitive is drawn through the viewport and the scissor its SV_ViewportArrayIndex names, the first ones without
// a geometry shader (15.8.1), so through neither when one of the two is not set. this holds from draw to draw
// in one pass: a call that sets one kind leaves the other kind as it was set.
// 15.8.1: "Values outside [0..n-1] are treated as 0 for indexing viewports." the shader still receives the
// unmodified index; an integer target retains all 32 bits separately from the draw tag.
// each case is draws of a triangle over the whole clip space, each with a value of its own: a pixel holds the value
// of the last draw whose viewport and scissor both have it, or nothing.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace viewports {

static const char hlsl[] = R"hlsl(
cbuffer Draw : register(b0) { uint index; uint value; uint conflicting; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
struct G { float4 position : SV_Position; uint viewport : SV_ViewportArrayIndex; };
[maxvertexcount(3)]
void gs(triangle float4 corners[3] : SV_Position, inout TriangleStream<G> stream) {
  for (int i = 0; i < 3; i++) {
    G g;
    g.position = corners[i];
    g.viewport = conflicting && i != 0 ? (index + 1) % MAX_VIEWPORTS : index;
    stream.Append(g);
  }
}
uint4 ps(G input) : SV_Target { return uint4(value, input.viewport, uint2(input.position.xy)); }
uint4 ps_plain(float4 p : SV_Position) : SV_Target { return uint4(value, 0, uint2(p.xy)); }
)hlsl";

const uint32_t width = 8, height = 4;
const uint32_t max_viewports = 16; // D3D11.3 15.8.1, checked against each API header by the callers
using Pixel = std::array<uint32_t, 4>;

// parts of the target, as left, top, right, bottom
enum Part { Whole, Left, Right, Lower, Nothing };
struct Rectangle {
  int left, top, right, bottom;
};
inline Rectangle
rectangle(Part part) {
  const int w = width, h = height;
  const Rectangle parts[] = {{0, 0, w, h}, {0, 0, w / 2, h}, {w / 2, 0, w, h}, {0, h / 2, w, h}, {0, 0, 0, 0}};
  return parts[part];
}

// a draw: the viewports and scissors set before it, if any are, and the viewport its primitive names. `plain`
// draws without a geometry shader
const uint32_t plain = ~0u - 1;
struct Draw {
  std::optional<std::vector<Part>> viewports, scissors;
  uint32_t index;
  bool conflicting = false;
};
using Case = std::vector<Draw>;

inline std::vector<Case>
cases() {
  using Parts = std::vector<Part>;
  std::vector<Case> all = {
      // one of each, without and with the geometry shader
      {{Parts{Whole}, Parts{Whole}, plain}},
      {{Parts{Whole}, Parts{Whole}, 0}},
      {{Parts{Right}, Parts{Lower}, plain}},
      // none of one kind
      {{Parts{}, Parts{Whole}, plain}},
      {{Parts{Whole}, Parts{}, plain}},
      {{Parts{}, Parts{}, 0}},
      // the second of each
      {{Parts{Whole, Right}, Parts{Whole, Whole}, 1}},
      {{Parts{Whole, Whole}, Parts{Whole, Left}, 1}},
      {{Parts{Left, Right}, Parts{Nothing, Lower}, 1}},
      {{Parts{Left, Right}, Parts{Nothing, Lower}, 0}},
      // a second that only one kind has
      {{Parts{Whole, Whole}, Parts{Whole}, 1}},
      {{Parts{Whole}, Parts{Whole, Whole}, 1}},
      // more viewports than before, with the scissors that were set for them earlier: an empty one, a part
      {{Parts{Whole}, Parts{Whole, Nothing}, 0}, {Parts{Whole, Whole}, std::nullopt, 1}},
      {{Parts{Whole}, Parts{Whole, Lower}, 0}, {Parts{Whole, Whole}, std::nullopt, 1}},
      // fewer of one kind than before
      {{Parts{Whole, Whole}, Parts{Whole, Whole}, 1}, {Parts{Whole}, std::nullopt, 1}},
      {{Parts{Whole, Whole}, Parts{Whole, Whole}, 1}, {std::nullopt, Parts{Whole}, 1}},
      {{Parts{Whole}, Parts{Whole}, plain}, {Parts{}, std::nullopt, plain}},
      {{Parts{Whole}, Parts{Whole}, plain}, {std::nullopt, Parts{}, plain}},
      // and more again
      {{Parts{}, Parts{Whole}, plain},
       {Parts{Left}, std::nullopt, plain},
       {Parts{Left, Right}, Parts{Whole, Lower}, 1}},
  };
  for (uint32_t index = 0; index < max_viewports; index++) {
    Parts v(max_viewports, Nothing), s(max_viewports, Nothing);
    v[index] = Whole;
    s[index] = Whole;
    all.push_back({{v, s, index}});
    all.push_back({{v, s, index, true}});
    Parts short_v(index + 1, Nothing), short_s(index + 1, Nothing);
    short_v[index] = short_s[index] = Whole;
    all.push_back({{short_v, short_s, index}});
    all.push_back({{Parts{Whole}, Parts{Whole}, 0}, {v, s, index}});
  }
  for (uint32_t index : {max_viewports, max_viewports + 1, ~0u})
    all.push_back({{Parts{Right}, Parts{Lower}, index}});
  return all;
}

// what the target holds: draw n's tag is n + 1, alongside its raw shader viewport index. `scissoring`: whether the
// scissor test is on, which Direct3D 12's always is
inline std::vector<Pixel> expected(const Case &draws, bool scissoring = true) {
  std::vector<Pixel> pixels(width * height);
  std::vector<Part> viewports, scissors;
  for (size_t n = 0; n < draws.size(); n++) {
    auto &draw = draws[n];
    if (draw.viewports)
      viewports = *draw.viewports;
    if (draw.scissors)
      scissors = *draw.scissors;
    uint32_t index = draw.index == plain ? 0 : draw.index;
    if (index >= max_viewports)
      index = 0;
    Rectangle through = rectangle(index < viewports.size() ? viewports[index] : Nothing),
              scissor = rectangle(!scissoring ? Whole : index < scissors.size() ? scissors[index] : Nothing);
    for (int y = std::max(through.top, scissor.top); y < std::min(through.bottom, scissor.bottom); y++)
      for (int x = std::max(through.left, scissor.left); x < std::min(through.right, scissor.right); x++)
        pixels[y * width + x] = {uint32_t(n + 1), draw.index == plain ? 0 : draw.index, uint32_t(x), uint32_t(y)};
  }
  return pixels;
}

// compares a case's pixels, `pitch` bytes a row; returns the failures
inline unsigned check(const char *what, size_t number, const std::vector<Pixel> &want, const char *rows, size_t pitch) {
  unsigned wrong = 0;
  for (uint32_t y = 0; y < height; y++)
    for (uint32_t x = 0; x < width; x++) {
      auto got = *reinterpret_cast<const Pixel *>(rows + y * pitch + x * sizeof(Pixel));
      if (got != want[y * width + x] && wrong++ < 2)
        expect(false, "%scase %zu: pixel %u,%u holds (%u,%u,%u,%u), want (%u,%u,%u,%u)", what, number, x, y, got[0],
               got[1], got[2], got[3], want[y * width + x][0], want[y * width + x][1], want[y * width + x][2],
               want[y * width + x][3]);
    }
  return wrong;
}

} // namespace viewports
