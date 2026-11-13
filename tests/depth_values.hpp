// depth families follow SDK component spelling and D3D11.3 19.1.3/.4; plane defaults follow 19.1.3.3.
#pragma once
#include "format_values.hpp"

namespace depth_values {
struct Format {
  DXGI_FORMAT id, parent, planes[2];
  std::string name;
  unsigned depth_bits, stencil_bits, unused_bits;
  bool floating;
  unsigned
  bytes() const {
    return (depth_bits + stencil_bits + unused_bits) / CHAR_BIT;
  }
  format_values::Format
  depth() const {
    return {planes[0], name, floating ? format_values::Floating : format_values::Unorm, {{0, depth_bits, 0}}, bytes()};
  }
};
inline std::vector<Format>
formats() {
  const struct {
    DXGI_FORMAT id;
    const char *name;
  } sdk[] = {DXGI_FORMATS}, depths[] = {DXGI_DEPTH_FORMATS};
  auto lookup = [&](const std::string &name) {
    for (auto item : sdk)
      if (name == item.name)
        return item.id;
    return DXGI_FORMAT_UNKNOWN;
  };
  std::vector<Format> result;
  for (auto [id, name] : depths) {
    Format f{id, DXGI_FORMAT_UNKNOWN, {}, name};
    sscanf(name, "DXGI_FORMAT_D%u", &f.depth_bits);
    f.floating = strstr(name, "FLOAT") != nullptr;
    if (auto at = strstr(name, "_S"))
      sscanf(at, "_S%uX%u", &f.stencil_bits, &f.unused_bits);
    const auto d = std::to_string(f.depth_bits), s = std::to_string(f.stencil_bits);
    const auto tail = f.unused_bits ? "X" + std::to_string(f.unused_bits) : "";
    f.parent = lookup("DXGI_FORMAT_R" + d + (f.stencil_bits ? "G" + s + tail : "") + "_TYPELESS");
    f.planes[0] = lookup(
        "DXGI_FORMAT_R" + d + (f.floating ? "_FLOAT" : "_UNORM") + (f.stencil_bits ? "_X" + s + tail + "_TYPELESS" : "")
    );
    f.planes[1] =
        f.stencil_bits ? lookup("DXGI_FORMAT_X" + d + "_TYPELESS_G" + s + tail + "_UINT") : DXGI_FORMAT_UNKNOWN;
    result.push_back(f);
  }
  return result;
}
} // namespace depth_values
