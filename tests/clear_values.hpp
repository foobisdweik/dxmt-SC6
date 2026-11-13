// what a clear of a float color stores in each channel of a format, by the rules d3d11_clear_values.cpp quotes:
// FLOAT to UNORM/SNORM maps NaN to zero and clamps, within "0.6f ULP" (D3D11.3 3.2.3.4/3.2.3.6); sRGB uses
// 3.2.3.8's transfer function except for alpha (19.1.4); integer inputs have "fractions rounded to zero, out of
// range/INF values clamped to target range, NaN to 0" (5.2.3.3); "Round-to-zero must be used during conversion to
// another float format", finite overflow giving "the maximum representable (appropriately signed) value" (3.2.2);
// 16/11/10-bit floats have five exponent bits and keep denorms, unsigned negatives clamping to 0 (3.1.5/6).
// a test includes its trace (expect) and copy_formats.hpp first.
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace clear_values {
struct Channel {
  UINT component, bits;
};

struct Kind {
  bool snorm, unorm, sint, uint, floating, srgb;
};

inline Kind
kind(const CopyFormat &format) {
  return {!!strstr(format.name, "_SNORM"), !!strstr(format.name, "_UNORM"), !!strstr(format.name, "_SINT"),
          !!strstr(format.name, "_UINT"),  !!strstr(format.name, "_FLOAT"), !!strstr(format.name, "_SRGB")};
}

// the format's channels in the order they are stored, from its SDK name; empty for an unexpected layout
inline std::vector<Channel>
channels(const CopyFormat &format) {
  constexpr char components[] = "RGBA";
  std::vector<Channel> channels;
  for (const char *at = format.family; *at;) {
    const char *component = strchr(components, *at++);
    char *end;
    const UINT bits = UINT(strtoul(at, &end, 10));
    if (!expect(component && at != end && bits <= sizeof(UINT) * CHAR_BIT, "unexpected SDK layout %s", format.family))
      return {};
    channels.push_back({UINT(component - components), bits});
    at = end;
  }
  return channels;
}

// the inputs a clear is tried with: every class of float, and the edges of each narrower float channel
inline std::vector<float>
values(Kind kind, const std::vector<Channel> &channels) {
  const float infinity = std::numeric_limits<float>::infinity();
  std::vector<float> values{-infinity, std::numeric_limits<float>::lowest(), -1.5f, -1, -0.5f,
                            -std::numeric_limits<float>::denorm_min(), -0.0f, 0.0f,
                            std::numeric_limits<float>::denorm_min(), 0.25f, 0.5f, 0.75f, 1, 1.5f,
                            std::numeric_limits<float>::max(), infinity, std::numeric_limits<float>::quiet_NaN()};
  if (kind.floating)
    for (auto channel : channels)
      if (channel.bits < sizeof(float) * CHAR_BIT) {
        constexpr UINT exponent_bits = 5; // D3D11.3 3.1.5/6
        constexpr int bias = (1 << (exponent_bits - 1)) - 1;
        const UINT fraction_bits = channel.bits - exponent_bits - (channel.bits == 16);
        const float minimum = std::ldexp(1.0f, 1 - bias - int(fraction_bits));
        const float maximum = std::ldexp(2.0f - std::ldexp(1.0f, -int(fraction_bits)), bias);
        for (float edge : {minimum, std::ldexp(1.0f, 1 - bias), maximum, 1.0f + std::ldexp(3.0f, -int(fraction_bits) - 2)})
          for (float sign : {-1.0f, 1.0f}) {
            values.push_back(sign * std::nextafter(edge, 0.0f));
            values.push_back(sign * edge);
            values.push_back(sign * std::nextafter(edge, infinity));
          }
      }
  return values;
}

// checks one stored texel against `color`, reporting each wrong channel
inline bool
check(const unsigned char *pixel, Kind kind, const std::vector<Channel> &channels, const std::array<float, 4> &color,
      UINT x, UINT y) {
  constexpr double norm_tolerance = 0.6; // D3D11.3 3.2.3.4/6/8, integer ULP
  bool ok = true;
  UINT at = 0;
  for (auto channel : channels) {
    UINT64 encoded = 0;
    for (UINT bit = 0; bit < channel.bits; bit++, at++)
      encoded |= UINT64((pixel[at / CHAR_BIT] >> (at % CHAR_BIT)) & 1) << bit;
    const UINT64 mask = (UINT64(1) << channel.bits) - 1;
    const double value = color[channel.component];
    if (kind.floating) {
      if (channel.bits == sizeof(float) * CHAR_BIT) {
        const float got = std::bit_cast<float>(UINT(encoded));
        const float input = color[channel.component];
        ok &= expect(std::isnan(value) ? std::isnan(got)
                                       : UINT(encoded) == std::bit_cast<UINT>(input) ||
                                             (std::fpclassify(input) == FP_SUBNORMAL &&
                                              UINT(encoded) == std::bit_cast<UINT>(std::copysign(0.0f, input))),
                     "pixel (%u,%u) float channel %u: %#llx, input %g", x, y, channel.component, encoded, value);
      } else {
        constexpr UINT exponent_bits = 5; // D3D11.3 3.1.5/6
        constexpr int bias = (1 << (exponent_bits - 1)) - 1;
        const bool signed_float = channel.bits == 16;
        const UINT fraction_bits = channel.bits - exponent_bits - signed_float;
        const UINT fraction_mask = (1u << fraction_bits) - 1;
        const UINT exponent_mask = (1u << exponent_bits) - 1;
        const UINT sign = signed_float && std::signbit(value) ? 1u << (channel.bits - 1) : 0;
        UINT want = sign;
        if (std::isnan(value)) {
          ok &= expect(((encoded >> fraction_bits) & exponent_mask) == exponent_mask && (encoded & fraction_mask),
                       "pixel (%u,%u) channel %u: NaN became %#llx", x, y, channel.component, encoded);
          continue;
        }
        const double magnitude = !signed_float && value < 0 ? 0 : std::abs(value);
        if (std::isinf(magnitude))
          want |= exponent_mask << fraction_bits;
        else if (magnitude) {
          int exponent;
          const double fraction = std::frexp(magnitude, &exponent);
          const int biased = exponent - 1 + bias;
          if (biased >= int(exponent_mask))
            want |= ((exponent_mask - 1) << fraction_bits) | fraction_mask;
          else if (biased <= 0)
            want |= UINT(std::floor(std::ldexp(magnitude, bias - 1 + fraction_bits)));
          else
            want |= (UINT(biased) << fraction_bits) | UINT(std::floor(std::ldexp(2 * fraction - 1, fraction_bits)));
        }
        ok &= expect(encoded == want, "pixel (%u,%u) channel %u: %#llx, want %#x for %g", x, y, channel.component,
                     encoded, want, value);
      }
    } else {
      const bool signed_value = kind.snorm || kind.sint;
      const UINT64 sign = UINT64(1) << (channel.bits - 1);
      const double got = signed_value ? double(INT64(encoded ^ sign) - INT64(sign)) : double(encoded);
      const double maximum = signed_value ? double(sign - 1) : double(mask);
      const double minimum = kind.snorm ? -maximum : kind.sint ? -double(sign) : 0;
      double ideal = std::isnan(value) ? 0 : std::clamp(value, kind.snorm ? -1.0 : 0.0, 1.0);
      if (kind.unorm && kind.srgb && channel.component != 3)
        ideal = ideal <= 0.0031308 ? 12.92 * ideal : 1.055 * std::pow(ideal, 1.0 / 2.4) - 0.055;
      ideal *= maximum;
      if (kind.sint || kind.uint)
        ideal = std::isnan(value) ? 0 : std::trunc(std::clamp(value, minimum, maximum));
      ok &= expect((kind.sint || kind.uint) ? got == ideal : std::abs(got - ideal) <= norm_tolerance,
                   "pixel (%u,%u) channel %u: %g, ideal %g", x, y, channel.component, got, ideal);
    }
  }
  return ok;
}
} // namespace clear_values
