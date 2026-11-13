// contract: sin and cos obey every entry of D3D11.3 22.10.20, including outside the accuracy interval.
// "Theta values can be any IEEE 32-bit floating point values."
// "The maximum absolute error is 0.0008 in the interval from -100*Pi to +100*Pi."
// TableSinCos quotes, in source order:
// src:     -inf, -F,         -denorm, -0, +0, +denorm, +F,         +inf, NaN
// DestSin: NaN,  [-1 to +1], -0,      -0, +0, +0,      [-1 to +1], NaN,  NaN
// DestCos: NaN,  [-1 to +1], +1,      +1, +1, +1,      [-1 to +1], NaN,  NaN
// tan is built from sincos by FXC; DXIL.rst, Tan gives "[-inf to +inf]" for finite sources and the sine table's
// other entries (https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst#tan).
#include "d3d11_test.hpp"
#undef min
#undef max
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

int
main() {
  constexpr double accuracy_limit = 100 * std::numbers::pi;
  constexpr double absolute_error = 0.0008;
  constexpr UINT threads = 64;
  const char *operations[] = {"sin", "cos", "tan"};
  const UINT components = std::size(operations);
  std::vector<float> values;
  for (float a : {0.0f, 1.0f, std::nextafter(float(accuracy_limit), 0.0f), float(accuracy_limit),
                  std::nextafter(float(accuracy_limit), std::numeric_limits<float>::infinity()),
                  std::numeric_limits<float>::denorm_min(), std::nextafter(std::numeric_limits<float>::min(), 0.0f),
                  std::numeric_limits<float>::min(), std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    values.insert(values.end(), {a, -a});
  for (int exponent = std::numeric_limits<float>::min_exponent; exponent <= std::numeric_limits<float>::max_exponent;
       exponent++) {
    float a = std::ldexp(1.0f, exponent - 1);
    values.insert(values.end(), {a, -a});
  }
  const UINT count = values.size(), stride = components * sizeof(float), bytes = count * stride;
  static const char hlsl[] = R"hlsl(
StructuredBuffer<uint> input : register(t0);
RWStructuredBuffer<uint3> output : register(u0);
[numthreads(THREADS, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {
  if (id.x < COUNT) {
    float a = asfloat(input[id.x]);
    output[id.x] = asuint(float3(sin(a), cos(a), tan(a)));
  }
}
)hlsl";
  auto code = compile(hlsl, "cs", "cs", {"THREADS=" + std::to_string(threads), "COUNT=" + std::to_string(count)});
  if (!expect(bool(code), "the HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  ComPtr<ID3D11ComputeShader> shader;
  CHECK(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
  D3D11_BUFFER_DESC input_desc{UINT(values.size() * sizeof(float)), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                              D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(float)};
  D3D11_SUBRESOURCE_DATA input_data{values.data()};
  ComPtr<ID3D11Buffer> input, output;
  CHECK(device->CreateBuffer(&input_desc, &input_data, &input));
  D3D11_BUFFER_DESC output_desc{bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                               D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, stride};
  CHECK(device->CreateBuffer(&output_desc, nullptr, &output));
  ComPtr<ID3D11ShaderResourceView> srv;
  ComPtr<ID3D11UnorderedAccessView> uav;
  CHECK(device->CreateShaderResourceView(input.Get(), nullptr, &srv));
  CHECK(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
  step("the sincos table and tangent over %u inputs", count);
  context->CSSetShader(shader.Get(), nullptr, 0);
  context->CSSetShaderResources(0, 1, srv.GetAddressOf());
  context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
  context->Dispatch((count + threads - 1) / threads, 1, 1);
  auto got = read(device.Get(), context.Get(), output.Get());
  if (!expect(got.size() * sizeof(uint32_t) == bytes, "readback size"))
    return verdict();
  for (UINT i = 0; i < count; i++)
    for (UINT op = 0; op < components; op++) {
      float a = values[i], g = std::bit_cast<float>(got[i * components + op]);
      bool ok;
      if (!std::isfinite(a)) {
        ok = std::isnan(g);
      } else if (a == 0 || std::fpclassify(a) == FP_SUBNORMAL) {
        float want = op == 1 ? 1.0f : std::copysign(0.0f, a);
        ok = got[i * components + op] == std::bit_cast<uint32_t>(want);
      } else {
        double want = op == 0 ? std::sin(double(a)) : op == 1 ? std::cos(double(a)) : std::tan(double(a));
        ok = op == 2 ? !std::isnan(g) : g >= -1 && g <= 1;
        if (std::abs(double(a)) <= accuracy_limit)
          ok = ok && std::abs(g - want) <= absolute_error * (op == 2 ? 1 + want * want : 1);
      }
      expect(ok, "%s(%#x) returned %#x", operations[op], std::bit_cast<uint32_t>(a), got[i * components + op]);
    }
  return verdict();
}
