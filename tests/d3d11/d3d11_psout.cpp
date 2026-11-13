// alpha-to-coverage endpoints: "alpha of 0 (or less) must map to no coverage and alpha of 1 (or greater) must
// map to full coverage"; "An alpha value of NaN results in a no coverage (zero) mask." and "If the Pixel Shader
// outputs oMask (output coverage), Alpha-to-Coverage is disabled." (D3D11.3 17.18).
// contract: the view interprets the pixel shader's register bits, then converts to its storage format.
// "There is no data type associated with registers; Shader instructions determine how the data stored in
// registers is interpreted." (D3D11.3 7.8.1). "The application is given control over the data type interpretation for
// Shader outputs (i.e. writing raw integer values vs. writing normalized float values) by simply choosing an
// appropriate format to interpret the output resource's contents as." (7.6).
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm Wine's d3d10core
// test_layered_rtv_mismatch records float bits in R32G32B32A32_UINT. the other pairs keep the same register bits
// under 7.6; 19.1.3.2 and 3.2.3 give the view's conversion. float, UNORM, SNORM and sRGB interpret them as floats, SINT
// as signed integers, UINT as unsigned integers, independently for each target (17.14). "Failure to provide sufficient
// data to the Output Merger for all of the RenderTarget(s)/component(s) enabled with the write masks results in
// undefined values being written out." (17.15): undeclared components are not compared; disabled channels keep the
// clear. BLEND_FACTOR scales the source by one half (17.1, 17.5), dual source multiplies o0 by o1 (17.6); 16.5
// broadcasts at pixel frequency and computes each sample at sample frequency. D3D11 leaves blending off for integer
// views, which have no blend capability. D3D12 refuses that PSO when its output is declared: vkd3d-proton
// tests/d3d12_pso.c:test_integer_blending_pipeline_state records E_INVALIDARG. depth without a DSV is ignored (17.9); a
// bound D32_FLOAT is tested too. all shaders compile before device creation.
#include "d3d11_test.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cfloat>
#include <cmath>
#include <limits>
#include <d3d10.h>

static constexpr UINT TARGETS = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
static constexpr UINT SIDE = 4, SAMPLES = 4;
enum Mode {
  Full,
  Fewer,
  Narrow,
  Mask,
  ZeroMask,
  Blend,
  Dual,
  DepthAbsent,
  DepthPresent,
  PixelFrequency,
  SampleFrequency,
  Geometry,
  Tessellation,
  AlphaToCoverage,
  CoverageOverridesAlpha,
  MODES
};

static const float alphas[] = {-INFINITY, -1, -0.0f, 0, 1, 2, INFINITY, std::numeric_limits<float>::quiet_NaN()};

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
struct Vertex { float4 pos : SV_Position; };
[maxvertexcount(3)]
void gs(triangle Vertex vertices[3], inout TriangleStream<Vertex> stream) {
  for (uint i = 0; i < 3; i++) {
    Vertex v = vertices[i];
    v.pos.x = -v.pos.x;
    stream.Append(v);
  }
}
struct Factors { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Factors factors(InputPatch<Vertex, 3> vertices) {
  Factors f; f.edge[0] = f.edge[1] = f.edge[2] = f.inside = 1; return f;
}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("factors")]
Vertex hs(InputPatch<Vertex, 3> vertices, uint id : SV_OutputControlPointID) { return vertices[id]; }
[domain("tri")]
float4 ds(Factors f, const OutputPatch<Vertex, 3> vertices, float3 bary : SV_DomainLocation) : SV_Position {
  return vertices[0].pos * bary.x + vertices[1].pos * bary.y + vertices[2].pos * bary.z;
}
float4 out_float(float4 v) { return v; }
uint4 out_uint(float4 v) { return asuint(v); }
int4 out_int(float4 v) { return asint(v); }
struct Outputs {
  TYPE0 colour : TARGET;
  TYPE1 other : OTHER_TARGET;
#if COVERAGE_OUTPUT
  uint coverage : SV_Coverage;
#endif
#if DEPTH
  float z : SV_Depth;
#endif
};
Outputs ps(float4 pos : SV_Position
#if RATE
, uint sid : SV_SampleIndex
#endif
) {
#if !RATE
  uint sid = 0;
#endif
  float4 v = float4((1 + ((uint)pos.x + sid) % 3) * 0.25, -0.5, 0.5, 1);
#if ALPHA_COVERAGE
  static const uint bits[] = {ALPHA_VALUES};
  v.a = asfloat(bits[((uint)pos.y * SIDE + (uint)pos.x) % ALPHA_COUNT]);
#endif
  Outputs o;
#if COVERAGE_OUTPUT
  o.coverage = ~0u;
#endif
  o.colour = CONVERT0(v).CHANNELS;
  o.other = CONVERT1(float4(0.5, 0.25, 1, 0.75));
#if DEPTH
  o.z = 0.75;
#endif
  return o;
}
Texture2DMS<READ_TYPE> image : register(t0);
RWStructuredBuffer<uint4> result : register(u0);
[numthreads(1, 1, 1)] void cs(uint2 p : SV_DispatchThreadID) {
  for (uint s = 0; s < SAMPLES; s++)
    result[(p.y * SIDE + p.x) * SAMPLES + s] = asuint(image.Load(p, s));
}
)hlsl";

enum Kind { Float, Unorm, Snorm, Srgb, Sint, Uint };
struct Format {
  DXGI_FORMAT format;
  Kind kind;
  UINT components, bits;
};
static Format
format(Kind kind, UINT components) {
  const DXGI_FORMAT floats[] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
  const DXGI_FORMAT sints[] = {DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32A32_SINT};
  const DXGI_FORMAT uints[] = {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32A32_UINT};
  const DXGI_FORMAT unorms[] = {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
  const DXGI_FORMAT snorms[] = {DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8B8A8_SNORM};
  UINT at = components == 1 ? 0 : components == 2 ? 1 : 2;
  if (kind == Srgb)
    return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kind, 4, CHAR_BIT};
  return {kind == Float   ? floats[at]
          : kind == Sint  ? sints[at]
          : kind == Uint  ? uints[at]
          : kind == Unorm ? unorms[at]
                          : snorms[at],
          kind, components,
          kind == Float || kind == Sint || kind == Uint ? UINT(sizeof(UINT) * CHAR_BIT) : UINT(CHAR_BIT)};
}

// ideal output conversion and its tolerances, D3D11.3 3.2.3.4, 3.2.3.6 and 3.2.3.8
static double
scaled(Format f, float value, UINT channel) {
  if (std::isnan(value)) value = 0;
  if (f.kind == Snorm)
    return std::clamp(double(value), -1.0, 1.0) * INT8_MAX;
  double v = std::clamp(double(value), 0.0, 1.0);
  if (f.kind == Srgb && channel != 3)
    v = v <= D3D10_FLOAT_TO_SRGB_THRESHOLD
            ? D3D10_FLOAT_TO_SRGB_SCALE_1 * v
            : D3D10_FLOAT_TO_SRGB_SCALE_2 *
                      std::pow(v, D3D10_FLOAT_TO_SRGB_EXPONENT_NUMERATOR / D3D10_FLOAT_TO_SRGB_EXPONENT_DENOMINATOR) -
                  D3D10_FLOAT_TO_SRGB_OFFSET;
  return v * UINT8_MAX;
}

static UINT
stored(Format f, float value, UINT channel) {
  return f.bits == sizeof(UINT) * CHAR_BIT ? std::bit_cast<UINT>(value)
                                           : UINT(int(std::round(scaled(f, value, channel)))) & UINT8_MAX;
}

static bool
matches(Format f, UINT got, float value, UINT channel, bool sampled) {
  if (f.bits == sizeof(UINT) * CHAR_BIT)
    return f.kind == Float && std::isnan(value) ? std::isnan(std::bit_cast<float>(got)) : got == stored(f, value, channel);
  double actual = f.kind == Snorm ? int(int8_t(got)) : int(got);
  double tolerance = D3D10_FLOAT32_TO_INTEGER_TOLERANCE_IN_ULP;
  if (sampled) {
    auto number = std::bit_cast<float>(got);
    if (!std::isfinite(number))
      return false;
    actual = scaled(f, number, channel);
    tolerance += f.kind == Srgb && channel != 3 ? D3D10_SRGB_TO_FLOAT_TOLERANCE_IN_ULP : FLT_EPSILON * UINT8_MAX;
  }
  return std::abs(actual - scaled(f, value, channel)) <= tolerance;
}

static std::vector<std::string>
defines(UINT kind, UINT slot, UINT mode) {
  const char *types[] = {"float", "int", "uint"}, *conversions[] = {"out_float", "out_int", "out_uint"};
  UINT components = mode == Fewer ? 1 : mode == Mask ? 2 : 4;
  std::string suffix = components == 1 ? "" : std::to_string(components);
  std::string alpha_values;
  for (auto alpha : alphas) {
    if (!alpha_values.empty()) alpha_values += ",";
    alpha_values += std::to_string(std::bit_cast<UINT>(alpha)) + "u";
  }
  return {"ALPHA_COVERAGE=" + std::to_string(mode == AlphaToCoverage || mode == CoverageOverridesAlpha),
          "COVERAGE_OUTPUT=" + std::to_string(mode == CoverageOverridesAlpha),
          "ALPHA_COUNT=" + std::to_string(std::size(alphas)), "ALPHA_VALUES=" + alpha_values,
          std::string("TYPE0=") + types[kind] + suffix,
          std::string("TYPE1=") + types[(kind + 1) % std::size(types)] + "4",
          std::string("CONVERT0=") + conversions[kind],
          std::string("CONVERT1=") + conversions[(kind + 1) % std::size(types)],
          std::string("CHANNELS=") + (components == 1   ? "x"
                                      : components == 2 ? "xy"
                                                        : "xyzw"),
          "TARGET=SV_Target" + std::to_string(slot),
          "OTHER_TARGET=SV_Target" + std::to_string((slot + 1) % TARGETS),
          "DEPTH=" + std::to_string(mode == DepthAbsent || mode == DepthPresent),
          "RATE=" + std::to_string(mode == SampleFrequency),
          "READ_TYPE=float4",
          "SIDE=" + std::to_string(SIDE),
          "SAMPLES=" + std::to_string(SAMPLES)};
}

int
main() {
  std::array<std::array<std::array<ComPtr<ID3DBlob>, TARGETS>, MODES>, 3> codes;
  auto defaults = defines(0, 0, 0);
  auto vertex = compile(hlsl, "vs", "vs", defaults);
  auto geometry = compile(hlsl, "gs", "gs", defaults);
  auto hull = compile(hlsl, "hs", "hs", defaults);
  auto domain = compile(hlsl, "ds", "ds", defaults);
  if (!expect(vertex && geometry && hull && domain, "rasterizing HLSL did not compile"))
    return verdict();
  for (UINT kind = 0; kind < codes.size(); kind++)
    for (UINT mode = 0; mode < MODES; mode++)
      for (UINT slot = 0; slot < (mode ? 1 : TARGETS); slot++) {
        step("compile output %u, mode %u, slot %u", kind, mode, slot);
        codes[kind][mode][slot] = compile(hlsl, "ps", "ps", defines(kind, slot, mode));
        if (!expect(bool(codes[kind][mode][slot]), "pixel HLSL did not compile"))
          return verdict();
      }
  ComPtr<ID3DBlob> reads[3];
  const char *read_types[] = {"float4", "int4", "uint4"};
  for (UINT i = 0; i < std::size(reads); i++) {
    auto d = defaults;
    for (auto &define : d)
      if (define.rfind("READ_TYPE=", 0) == 0)
        define = std::string("READ_TYPE=") + read_types[i];
    reads[i] = compile(hlsl, "cs", "cs", d);
    if (!expect(bool(reads[i]), "readback HLSL did not compile"))
      return verdict();
  }

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  ComPtr<ID3D11VertexShader> vs;
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs));
  ComPtr<ID3D11GeometryShader> gs;
  ComPtr<ID3D11HullShader> hs;
  ComPtr<ID3D11DomainShader> ds;
  CHECK(device->CreateGeometryShader(geometry->GetBufferPointer(), geometry->GetBufferSize(), nullptr, &gs));
  CHECK(device->CreateHullShader(hull->GetBufferPointer(), hull->GetBufferSize(), nullptr, &hs));
  CHECK(device->CreateDomainShader(domain->GetBufferPointer(), domain->GetBufferSize(), nullptr, &ds));
  std::array<std::array<std::array<ComPtr<ID3D11PixelShader>, TARGETS>, MODES>, 3> shaders;
  for (UINT kind = 0; kind < codes.size(); kind++)
    for (UINT mode = 0; mode < MODES; mode++)
      for (UINT slot = 0; slot < (mode ? 1 : TARGETS); slot++)
        CHECK(device->CreatePixelShader(codes[kind][mode][slot]->GetBufferPointer(),
                                        codes[kind][mode][slot]->GetBufferSize(), nullptr, &shaders[kind][mode][slot]));
  ComPtr<ID3D11ComputeShader> cs[3];
  for (UINT i = 0; i < std::size(cs); i++)
    CHECK(device->CreateComputeShader(reads[i]->GetBufferPointer(), reads[i]->GetBufferSize(), nullptr, &cs[i]));
  const UINT result_bytes = SIDE * SIDE * SAMPLES * sizeof(UINT[4]);
  D3D11_BUFFER_DESC result_desc{
      result_bytes,   D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
      sizeof(UINT[4])};
  ComPtr<ID3D11Buffer> result;
  ComPtr<ID3D11UnorderedAccessView> uav;
  CHECK(device->CreateBuffer(&result_desc, nullptr, &result));
  CHECK(device->CreateUnorderedAccessView(result.Get(), nullptr, &uav));
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster_desc.DepthClipEnable = TRUE;
  raster_desc.MultisampleEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));
  context->RSSetState(raster.Get());
  D3D11_VIEWPORT viewport{0, 0, float(SIDE), float(SIDE), 0, 1};
  context->RSSetViewports(1, &viewport);
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // revisit view types with the same shader objects: a variant for an earlier view cannot serve a later one
  for (UINT cycle = 0; cycle < 2; cycle++)
    for (UINT kind = 0; kind < shaders.size(); kind++)
      for (UINT mode = 0; mode < MODES; mode++)
        for (UINT slot = 0; slot < (mode ? 1 : TARGETS); slot++)
          for (UINT view = 0; view <= Uint; view++) {
            auto target_kind = Kind(cycle ? Uint - view : view);
            bool dual = mode == Dual, multisample = mode == PixelFrequency || mode == SampleFrequency ||
                mode == AlphaToCoverage || mode == CoverageOverridesAlpha;
            if (dual && (target_kind == Sint || target_kind == Uint))
              continue;
            step("cycle %u, output %u, view %u, slot %u, mode %u", cycle, kind, target_kind, slot, mode);
            UINT other_slot = (slot + 1) % TARGETS, samples = multisample ? SAMPLES : 1;
            UINT components = mode == Fewer ? 1 : mode == Mask ? 2 : 4;
            Format formats[] = {format(target_kind, mode == Narrow ? 1 : 4),
                                format(Kind((kind + 1) % shaders.size() == 0   ? Float
                                            : (kind + 1) % shaders.size() == 1 ? Sint
                                                                               : Uint),
                                       4)};
            // the widest target of the kind that the device multisamples: Apple GPUs have no multisampled 128-bit
            // integer format (Metal feature set tables)
            if (multisample)
              for (auto &wide : formats)
                for (UINT fewer = wide.components, quality = 0; fewer; fewer /= 2)
                  if (SUCCEEDED(device->CheckMultisampleQualityLevels(format(wide.kind, fewer).format, samples,
                                                                      &quality)) &&
                      quality) {
                    wide = format(wide.kind, fewer);
                    break;
                  }
            bool layered = !cycle && !kind && !slot && !mode && target_kind == Uint;
            ComPtr<ID3D11Texture2D> textures[2];
            ComPtr<ID3D11RenderTargetView> rtvs[2];
            ID3D11RenderTargetView *bound[TARGETS] = {};
            for (UINT i = 0; i < (dual ? 1u : UINT(std::size(textures))); i++) {
              if (multisample) {
                UINT quality = 0;
                CHECK(device->CheckMultisampleQualityLevels(formats[i].format, samples, &quality));
                if (!expect(quality != 0, "format %u has no %u-sample target", formats[i].format, samples))
                  return verdict();
              }
              D3D11_TEXTURE2D_DESC desc{SIDE,
                                        SIDE,
                                        1,
                                        1,
                                        formats[i].format,
                                        {samples, 0},
                                        D3D11_USAGE_DEFAULT,
                                        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
              D3D11_RENDER_TARGET_VIEW_DESC view_desc{formats[i].format, D3D11_RTV_DIMENSION_TEXTURE2DARRAY};
              view_desc.Texture2DArray = {0, 0, 1};
              if (layered && !i) {
                desc.ArraySize = 2;
                desc.Format = DXGI_FORMAT_R32G32B32A32_TYPELESS;
              }
              CHECK(device->CreateTexture2D(&desc, nullptr, &textures[i]));
              CHECK(device->CreateRenderTargetView(textures[i].Get(), layered && !i ? &view_desc : nullptr, &rtvs[i]));
              const float clear[4] = {};
              context->ClearRenderTargetView(rtvs[i].Get(), clear);
              bound[i ? other_slot : slot] = rtvs[i].Get();
            }
            D3D11_BLEND_DESC blend_desc{};
            blend_desc.IndependentBlendEnable = !dual;
            blend_desc.AlphaToCoverageEnable = mode == AlphaToCoverage || mode == CoverageOverridesAlpha;
            for (auto &rt : blend_desc.RenderTarget) {
              rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
              rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
              rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
              rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            }
            auto &blend = blend_desc.RenderTarget[slot];
            blend.RenderTargetWriteMask = mode == ZeroMask ? 0
                                          : mode == Mask ? D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_ALPHA
                                                         : D3D11_COLOR_WRITE_ENABLE_ALL;
            blend.BlendEnable = mode == Blend || dual;
            blend.SrcBlend = blend.SrcBlendAlpha = dual ? D3D11_BLEND_SRC1_COLOR : D3D11_BLEND_BLEND_FACTOR;
            if (dual)
              blend.SrcBlendAlpha = D3D11_BLEND_SRC1_ALPHA;
            ComPtr<ID3D11BlendState> blend_state;
            CHECK(device->CreateBlendState(&blend_desc, &blend_state));
            UINT sample_mask = ~0u;
            if (mode == PixelFrequency || mode == AlphaToCoverage || mode == CoverageOverridesAlpha) {
              sample_mask = 0;
              for (UINT s = 0; s < samples; s += 2)
                sample_mask |= 1u << s;
            }
            const float factor[4] = {0.5, 0.5, 0.5, 0.5};
            context->OMSetBlendState(blend_state.Get(), factor, sample_mask);
            ComPtr<ID3D11Texture2D> depth;
            ComPtr<ID3D11DepthStencilView> dsv;
            if (mode == DepthPresent) {
              D3D11_TEXTURE2D_DESC desc{
                  SIDE, SIDE, 1, 1, DXGI_FORMAT_D32_FLOAT, {samples, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL};
              CHECK(device->CreateTexture2D(&desc, nullptr, &depth));
              CHECK(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv));
              context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
            }
            context->OMSetRenderTargets(dual ? 1 : std::max(slot, other_slot) + 1, bound, dsv.Get());
            context->PSSetShader(shaders[kind][mode][slot].Get(), nullptr, 0);
            context->GSSetShader(mode == Geometry ? gs.Get() : nullptr, nullptr, 0);
            context->HSSetShader(mode == Tessellation ? hs.Get() : nullptr, nullptr, 0);
            context->DSSetShader(mode == Tessellation ? ds.Get() : nullptr, nullptr, 0);
            context->IASetPrimitiveTopology(mode == Tessellation ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                                                 : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->Draw(3, 0);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            for (UINT i = 0; i < (dual ? 1u : UINT(std::size(textures))); i++) {
              const char *bytes = nullptr;
              UINT pitch = 0;
              D3D11_MAPPED_SUBRESOURCE mapped{};
              ComPtr<ID3D11Texture2D> staging;
              std::vector<UINT> sampled;
              if (multisample) {
                ComPtr<ID3D11ShaderResourceView> srv;
                CHECK(device->CreateShaderResourceView(textures[i].Get(), nullptr, &srv));
                context->CSSetShader(cs[formats[i].kind == Sint   ? 1
                                        : formats[i].kind == Uint ? 2
                                                                  : 0]
                                         .Get(),
                                     nullptr, 0);
                context->CSSetShaderResources(0, 1, srv.GetAddressOf());
                context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
                context->Dispatch(SIDE, SIDE, 1);
                ID3D11ShaderResourceView *none = nullptr;
                context->CSSetShaderResources(0, 1, &none);
                sampled = read(device.Get(), context.Get(), result.Get());
                if (!expect(sampled.size() * sizeof(UINT) == result_bytes, "sample readback failed"))
                  return verdict();
                bytes = (const char *)sampled.data();
                pitch = SIDE * samples * sizeof(UINT[4]);
              } else {
                D3D11_TEXTURE2D_DESC desc;
                textures[i]->GetDesc(&desc);
                desc.Usage = D3D11_USAGE_STAGING;
                desc.BindFlags = 0;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
                context->CopyResource(staging.Get(), textures[i].Get());
                CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                bytes = (const char *)mapped.pData;
                pitch = mapped.RowPitch;
              }
              for (UINT y = 0; y < SIDE; y++)
                for (UINT x = 0; x < SIDE; x++)
                  for (UINT s = 0; s < samples; s++) {
                    float colour[] = {(1 + (x + (mode == SampleFrequency ? s : 0)) % 3) * 0.25f, -0.5f, 0.5f, 1};
                    if (mode == AlphaToCoverage || mode == CoverageOverridesAlpha)
                      colour[3] = alphas[(y * SIDE + x) % std::size(alphas)];
                    const float secondary[] = {0.5, 0.25, 1, 0.75};
                    for (UINT c = 0; c < formats[i].components; c++) {
                      bool written = (i || (blend.RenderTargetWriteMask >> c & 1)) && (sample_mask >> s & 1);
                      if (mode == AlphaToCoverage && !(colour[3] > 0)) written = false;
                      if (!i && c >= components && written)
                        continue;
                      float value = i ? secondary[c] : colour[c];
                      if (!i && dual)
                        value *= secondary[c];
                      else if (!i && mode == Blend && target_kind != Sint && target_kind != Uint)
                        value *= factor[c];
                      UINT got = 0;
                      UINT stride = multisample ? sizeof(UINT[4]) : formats[i].components * formats[i].bits / CHAR_BIT;
                      const char *at = bytes + y * pitch + (x * samples + s) * stride;
                      memcpy(&got, at + c * (multisample ? sizeof(UINT) : formats[i].bits / CHAR_BIT),
                             multisample ? sizeof(UINT) : formats[i].bits / CHAR_BIT);
                      expect(matches(formats[i], got, written ? value : 0, c, multisample),
                             "target %u pixel %u,%u sample %u channel %u has %#x, expected %#x", i, x, y, s, c, got,
                             stored(formats[i], written ? value : 0, c));
                    }
                  }
              if (staging)
                context->Unmap(staging.Get(), 0);
            }
            if (depth) {
              D3D11_TEXTURE2D_DESC desc;
              depth->GetDesc(&desc);
              desc.Usage = D3D11_USAGE_STAGING;
              desc.BindFlags = 0;
              desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
              ComPtr<ID3D11Texture2D> staging;
              CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
              context->CopyResource(staging.Get(), depth.Get());
              D3D11_MAPPED_SUBRESOURCE mapped;
              CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
              for (UINT y = 0; y < SIDE; y++)
                for (UINT x = 0; x < SIDE; x++)
                  expect(((const float *)((const char *)mapped.pData + y * mapped.RowPitch))[x] == 0.75f,
                         "depth at %u,%u is not the shader's 0.75", x, y);
              context->Unmap(staging.Get(), 0);
            }
          }
  return verdict();
}
