// contract: DirectX-Specs Conservative Rasterization 3.2: "Conservative Rasterization implementations must never
// produce false negatives with respect to the floating-point vertex coordinates for non-degenerate post-snap
// primitives". tier1 permits a half-pixel outer uncertainty region; tier2 permits1/256 and requires degenerates.
// InnerCoverage: "pixels entirely within the inner boundary of the inner uncertainty region must be marked as
// fully covered." the inner uncertainty is at most1/256; false positives against floating vertices are forbidden.
// source: https://microsoft.github.io/DirectX-Specs/d3d/ConservativeRasterization.html
#pragma once
#include "raster.hpp"

namespace raster_conservative {
using namespace raster;
static const char hlsl[] = R"hlsl(
float4 ps_inner(float4 p : SV_Position, uint inner : SV_InnerCoverage) : SV_Target {
  return float4(inner == 0 ? 0 : (inner & 1) ? 1 : 2,p.zw,1);
}
)hlsl";
inline std::vector<Case> cases(unsigned tier) {
  std::vector<Case> out;
  if (!tier)
    return out;
  for (unsigned offset = 0; offset < sample_grid; offset++)
    for (unsigned shape = 0; shape < 3; shape++)
      for (bool reverse : {false, true})
        for (bool ccw : {false, true})
          for (auto cull : {NoCull, CullFront, CullBack}) {
            auto c = base("conservative floating coverage and tier uncertainty bounds");
            c.conservative = true;
            c.ccw = ccw;
            c.cull = cull;
            const float x = width / 2 + float(offset) / sample_grid;
            if (shape == 0)
              c.vertices = {vertex(1.25f + float(offset) / sample_grid, 1.25f), vertex(width - 1.25f, 1.25f),
                            vertex(1.25f, height - 1.25f)};
            else {
              const float delta = shape == 1 ? 1.0f / (1u << (subpixel_bits + 2)) : 0;
              c.vertices = {vertex(x - delta, 2.25f), vertex(x + delta, height / 2 + 0.25f),
                            vertex(x - delta, height - 2.25f)};
            }
            if (reverse)
              std::swap(c.vertices[1], c.vertices[2]);
            out.push_back(c);
            if (tier >= 3) {
              c.inner = true;
              out.push_back(c);
            }
          }
  return out;
}
inline std::array<Screen, Triangle> positions(const Case &c, bool snapped) {
  std::array<Screen, Triangle> out;
  for (unsigned i = 0; i < Triangle; i++) {
    out[i] = project(c.vertices[i], c.viewport);
    if (!snapped) {
      const auto &p = c.vertices[i].position;
      out[i].x = (p[0] / p[3] + 1) * c.viewport.w * 0.5f + c.viewport.x;
      out[i].y = (1 - p[1] / p[3]) * c.viewport.h * 0.5f + c.viewport.y;
    }
  }
  return out;
}
inline bool overlaps(const std::array<Screen, Triangle> &triangle, unsigned x, unsigned y, double grow, bool strict) {
  std::array<std::array<double, 2>, Triangle + 2> axes{{{1, 0}, {0, 1}}};
  for (unsigned i = 0; i < Triangle; i++)
    axes[i + 2] = {triangle[(i + 1) % Triangle].y - triangle[i].y, triangle[i].x - triangle[(i + 1) % Triangle].x};
  for (auto [dx, dy] : axes) {
    if (!dx && !dy)
      continue;
    double low = std::numeric_limits<double>::infinity(), high = -low;
    for (auto p : triangle) {
      double dot = p.x * dx + p.y * dy;
      low = std::min(low, dot);
      high = std::max(high, dot);
    }
    double center = (x + 0.5) * dx + (y + 0.5) * dy;
    double radius = (0.5 + grow) * (std::abs(dx) + std::abs(dy));
    if (strict ? high <= center - radius || low >= center + radius : high < center - radius || low > center + radius)
      return false;
  }
  return true;
}
inline bool contains(const std::array<Screen, Triangle> &triangle, unsigned x, unsigned y, double grow,
                     bool strict = false) {
  double area = edge(triangle[0], triangle[1], triangle[2].x, triangle[2].y);
  if (!area)
    return false;
  for (unsigned i = 0; i < Triangle; i++)
    for (double px : {x - grow, x + 1 + grow})
      for (double py : {y - grow, y + 1 + grow})
        if (double value = edge(triangle[i], triangle[(i + 1) % Triangle], px, py) * (area > 0 ? 1 : -1);
            strict ? value <= 0 : value < 0)
          return false;
  return true;
}
inline void compare(const Case &c, unsigned tier, const char *rows, size_t pitch) {
  auto floating = positions(c, false), snapped = positions(c, true);
  double area = edge(snapped[0], snapped[1], snapped[2].x, snapped[2].y);
  bool front = area != 0 && (area < 0) == c.ccw;
  bool culled = (c.cull == CullFront && front) || (c.cull == CullBack && !front);
  double uncertainty = tier == 1 ? 0.5 : 1.0 / (1u << subpixel_bits);
  unsigned wrong = 0, hits = 0;
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++) {
      const float *got = reinterpret_cast<const float *>(rows + y * pitch) + x * lanes;
      bool required = !culled && (area != 0 || tier >= 2) && overlaps(floating, x, y, 0, true);
      bool allowed = !culled && overlaps(snapped, x, y, uncertainty, false);
      bool hit = got[3] != 0;
      hits += hit;
      bool valid = (!required || hit) && (!hit || allowed);
      if (hit) {
        valid &= got[3] == 1;
        if (c.inner) {
          bool inner = got[0] == 1;
          valid &= (!inner || contains(floating, x, y, 0)) && (!contains(snapped, x, y, uncertainty, true) || inner) &&
                   (inner || got[0] == 0);
          valid &= got[1] == 0.5f && got[2] == 1;
        } else
          valid &= got[0] == (front ? 1 : 2) && got[1] == 0.5f && got[2] == 1;
      } else
        for (unsigned lane = 0; lane < lanes; lane++)
          valid &= got[lane] == 0;
      if (!valid && wrong++ < 4)
        expect(false, "conservative pixel %u,%u = {%.9g,%.9g,%.9g,%.9g}, required %u allowed %u", x, y, got[0], got[1], got[2],
               got[3], required, allowed);
    }
  if (!culled && tier >= 2)
    expect(hits != 0, "conservative tier%u discarded an interior degenerate primitive", tier);
}
inline bool reference_contract() {
  std::array<Screen, Triangle> triangle{{{0, 0, 0.5, 1}, {4, 0, 0.5, 1}, {0, 4, 0.5, 1}}};
  bool ok = expect(overlaps(triangle, 0, 0, 0, true), "conservative reference rejected interior coverage");
  ok &= expect(!overlaps(triangle, 3, 3, 0, false), "conservative reference accepted disjoint boxes");
  ok &= expect(overlaps(triangle, 4, 0, 0, false) && !overlaps(triangle, 4, 0, 0, true),
               "conservative reference did not distinguish touching from interior coverage");
  ok &= expect(contains(triangle, 0, 0, 0) && !contains(triangle, 0, 0, 1.0 / (1u << subpixel_bits), true),
               "conservative reference did not erode the inner coverage region");
  return ok;
}
} // namespace raster_conservative
