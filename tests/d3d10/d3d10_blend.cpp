// contract: blend factors multiply the shader and stored colors before add/subtract, separately for RGB and alpha.
// D3D11.3 17.1: "The min and max blend ops ignore D3D11_BLEND modes"; 17.6 restricts dual-source operations to
// add/subtract/revsubtract and one target. 17.15: "The masks simply limit writes." the CPU equations below use
// the factor definitions in 17.1 and the alpha subset in 17.2.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// SUBTRACT is "Result = Source - Destination", REV_SUBTRACT is "Result = Destination - Source".
// https://raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs-ddi/staging/wdk-ddi-src/content/d3d10umddi/ne-d3d10umddi-d3d10_ddi_blend_op.md
// R32G32B32A32_FLOAT blending is required at both levels. dyadic test data makes fused/reordered arithmetic exact.
// https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/format-support-for-direct3d-feature-level-10-0-hardware
// D3D10 has per-target enables/masks; D3D10.1 additionally has per-target equations. disabled writes retain bytes.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/ns-d3d10-d3d10_blend_desc
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10_1/ns-d3d10_1-d3d10_blend_desc1
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iterator>

using Color = std::array<float, D3D10_STANDARD_VECTOR_SIZE>;
static constexpr Color source{0.75f, 0.25f, 0.5f, 0.625f}, secondary{0.5f, 0.875f, 0.25f, 0.75f},
    destination{0.25f, 0.5f, 0.75f, 0.875f}, blend_factor{0.875f, 0.5f, 0.125f, 0.25f};

static bool
dual(D3D10_BLEND factor) {
  return factor >= D3D10_BLEND_SRC1_COLOR && factor <= D3D10_BLEND_INV_SRC1_ALPHA;
}

static float
weight(D3D10_BLEND factor, UINT component, const Color &s) {
  constexpr UINT alpha = D3D10_STANDARD_VECTOR_SIZE - 1;
  switch (factor) {
  case D3D10_BLEND_ZERO:
    return 0;
  case D3D10_BLEND_ONE:
    return 1;
  case D3D10_BLEND_SRC_COLOR:
    return s[component];
  case D3D10_BLEND_INV_SRC_COLOR:
    return 1 - s[component];
  case D3D10_BLEND_SRC_ALPHA:
    return s[alpha];
  case D3D10_BLEND_INV_SRC_ALPHA:
    return 1 - s[alpha];
  case D3D10_BLEND_DEST_COLOR:
    return destination[component];
  case D3D10_BLEND_INV_DEST_COLOR:
    return 1 - destination[component];
  case D3D10_BLEND_DEST_ALPHA:
    return destination[alpha];
  case D3D10_BLEND_INV_DEST_ALPHA:
    return 1 - destination[alpha];
  case D3D10_BLEND_SRC_ALPHA_SAT:
    return component == alpha ? 1 : std::min(s[alpha], 1 - destination[alpha]);
  case D3D10_BLEND_BLEND_FACTOR:
    return blend_factor[component];
  case D3D10_BLEND_INV_BLEND_FACTOR:
    return 1 - blend_factor[component];
  case D3D10_BLEND_SRC1_COLOR:
    return secondary[component];
  case D3D10_BLEND_INV_SRC1_COLOR:
    return 1 - secondary[component];
  case D3D10_BLEND_SRC1_ALPHA:
    return secondary[alpha];
  case D3D10_BLEND_INV_SRC1_ALPHA:
    return 1 - secondary[alpha];
  default:
    std::abort();
  }
}

static float
combine(D3D10_BLEND_OP operation, D3D10_BLEND from, D3D10_BLEND to, UINT component, const Color &s) {
  if (operation == D3D10_BLEND_OP_MIN)
    return std::min(s[component], destination[component]);
  if (operation == D3D10_BLEND_OP_MAX)
    return std::max(s[component], destination[component]);
  const float from_source = s[component] * weight(from, component, s),
              d = destination[component] * weight(to, component, s);
  if (operation == D3D10_BLEND_OP_SUBTRACT)
    return from_source - d;
  if (operation == D3D10_BLEND_OP_REV_SUBTRACT)
    return d - from_source;
  return from_source + d;
}

int
main() {
  static const char hlsl[] = R"hlsl(
cbuffer Colors : register(b0) { float4 source_color; float4 second_color; };
float4 vs() : SV_Position { return float4(0, 0, 0, 1); }
struct Pair { float4 first : SV_Target0; float4 second : SV_Target1; };
Pair dual_ps() { Pair p; p.first = source_color; p.second = second_color; return p; }
struct Targets { float4 colors[TARGETS] : SV_Target; };
Targets ps() {
  Targets p;
  [unroll] for (uint i = 0; i < TARGETS; i++) p.colors[i] = source_color;
  return p;
}
)hlsl";
  const std::vector<std::string> defines{"TARGETS=" + std::to_string(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT)};
  auto vs_code = compile(hlsl, "vs", "vs_4_0", defines), ps_code = compile(hlsl, "ps", "ps_4_0", defines),
       dual_code = compile(hlsl, "dual_ps", "ps_4_0", defines);
  if (!expect(vs_code && ps_code && dual_code, "blend shaders did not compile"))
    return verdict();

  struct Case {
    D3D10_BLEND_DESC1 desc;
    bool dual_source, independent;
    Color color = source;
  };
  std::vector<Case> cases;
  auto append = [&](D3D10_BLEND from, D3D10_BLEND to, D3D10_BLEND_OP operation, bool alpha) {
    const bool uses_second = dual(from) || dual(to);
    if (uses_second && operation > D3D10_BLEND_OP_REV_SUBTRACT)
      return;
    Case c{};
    c.dual_source = uses_second;
    for (auto &rt : c.desc.RenderTarget) {
      rt.BlendEnable = TRUE;
      rt.SrcBlend = alpha ? D3D10_BLEND_ONE : from;
      rt.DestBlend = alpha ? D3D10_BLEND_ZERO : to;
      rt.BlendOp = alpha ? D3D10_BLEND_OP_ADD : operation;
      rt.SrcBlendAlpha = alpha ? from : D3D10_BLEND_ONE;
      rt.DestBlendAlpha = alpha ? to : D3D10_BLEND_ZERO;
      rt.BlendOpAlpha = alpha ? operation : D3D10_BLEND_OP_ADD;
      rt.RenderTargetWriteMask = D3D10_COLOR_WRITE_ENABLE_ALL;
    }
    cases.push_back(c);
    if (from == D3D10_BLEND_SRC_ALPHA_SAT || to == D3D10_BLEND_SRC_ALPHA_SAT) {
      c.color.back() = 0.0625f;
      cases.push_back(c);
    }
  };
  const D3D10_BLEND alpha_factors[] = {
      D3D10_BLEND_ZERO,          D3D10_BLEND_ONE,           D3D10_BLEND_SRC_ALPHA,
      D3D10_BLEND_INV_SRC_ALPHA, D3D10_BLEND_DEST_ALPHA,    D3D10_BLEND_INV_DEST_ALPHA,
      D3D10_BLEND_SRC_ALPHA_SAT, D3D10_BLEND_BLEND_FACTOR,  D3D10_BLEND_INV_BLEND_FACTOR,
      D3D10_BLEND_SRC1_ALPHA,    D3D10_BLEND_INV_SRC1_ALPHA
  };
  for (UINT operation = D3D10_BLEND_OP_ADD; operation <= D3D10_BLEND_OP_MAX; operation++) {
    for (UINT from = D3D10_BLEND_ZERO; from <= D3D10_BLEND_INV_SRC1_ALPHA; from++) {
      if (from > D3D10_BLEND_SRC_ALPHA_SAT && from < D3D10_BLEND_BLEND_FACTOR)
        continue;
      for (UINT to = D3D10_BLEND_ZERO; to <= D3D10_BLEND_INV_SRC1_ALPHA; to++) {
        if (to > D3D10_BLEND_SRC_ALPHA_SAT && to < D3D10_BLEND_BLEND_FACTOR)
          continue;
        append(D3D10_BLEND(from), D3D10_BLEND(to), D3D10_BLEND_OP(operation), false);
      }
    }
    for (auto from : alpha_factors)
      for (auto to : alpha_factors)
        append(from, to, D3D10_BLEND_OP(operation), true);
    for (UINT mask = 0; mask <= D3D10_COLOR_WRITE_ENABLE_ALL; mask++)
      for (bool enabled : {false, true})
        for (bool independent : {false, true}) {
          append(D3D10_BLEND_SRC_ALPHA, D3D10_BLEND_INV_SRC_ALPHA, D3D10_BLEND_OP(operation), false);
          auto &c = cases.back();
          c.independent = independent;
          c.desc.IndependentBlendEnable = independent;
          for (UINT rt = 0; rt < std::size(c.desc.RenderTarget); rt++) {
            auto &r = c.desc.RenderTarget[rt];
            r.BlendEnable = (rt % 2) != enabled;
            r.RenderTargetWriteMask = (mask + rt) & D3D10_COLOR_WRITE_ENABLE_ALL;
            if (independent) {
              r.SrcBlend = alpha_factors[rt % std::size(alpha_factors)];
              r.DestBlend = alpha_factors[(rt + 1) % std::size(alpha_factors)];
              r.BlendOp = D3D10_BLEND_OP(D3D10_BLEND_OP_ADD + rt % (D3D10_BLEND_OP_MAX - D3D10_BLEND_OP_ADD + 1));
              r.BlendOpAlpha = D3D10_BLEND_OP(D3D10_BLEND_OP_MAX - rt % (D3D10_BLEND_OP_MAX - D3D10_BLEND_OP_ADD + 1));
            }
          }
        }
  }
  if (!expect(cases.size() <= D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION, "case row exceeds the texture limit"))
    return verdict();
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")
  );
  if (!expect(create != nullptr, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  for (auto level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, level, D3D10_1_SDK_VERSION, &device));
    UINT support = 0;
    CHECK(device->CheckFormatSupport(DXGI_FORMAT_R32G32B32A32_FLOAT, &support));
    expect(
        (support & (D3D10_FORMAT_SUPPORT_RENDER_TARGET | D3D10_FORMAT_SUPPORT_BLENDABLE)) ==
            (D3D10_FORMAT_SUPPORT_RENDER_TARGET | D3D10_FORMAT_SUPPORT_BLENDABLE),
        "required float blend support"
    );
    ComPtr<ID3D10VertexShader> vs;
    ComPtr<ID3D10PixelShader> ps, second_ps;
    CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), &vs));
    CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), &ps));
    CHECK(device->CreatePixelShader(dual_code->GetBufferPointer(), dual_code->GetBufferSize(), &second_ps));
    Color colors[] = {source, secondary};
    D3D10_BUFFER_DESC cb_desc{sizeof(colors), D3D10_USAGE_DEFAULT, D3D10_BIND_CONSTANT_BUFFER};
    D3D10_SUBRESOURCE_DATA initial{colors};
    ComPtr<ID3D10Buffer> constants;
    CHECK(device->CreateBuffer(&cb_desc, &initial, &constants));
    device->VSSetShader(vs.Get());
    device->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_POINTLIST);
    ComPtr<ID3D10Texture2D> targets[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT], staging;
    ComPtr<ID3D10RenderTargetView> views[std::size(targets)];
    ID3D10RenderTargetView *rtvs[std::size(targets)];
    D3D10_TEXTURE2D_DESC td{
        UINT(cases.size()),      1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D10_USAGE_DEFAULT,
        D3D10_BIND_RENDER_TARGET
    };
    for (UINT rt = 0; rt < std::size(targets); rt++) {
      CHECK(device->CreateTexture2D(&td, nullptr, &targets[rt]));
      CHECK(device->CreateRenderTargetView(targets[rt].Get(), nullptr, &views[rt]));
      rtvs[rt] = views[rt].Get();
      device->ClearRenderTargetView(rtvs[rt], destination.data());
    }
    td.Usage = D3D10_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&td, nullptr, &staging));
    std::vector<std::array<Color, std::size(targets)>> expected(cases.size());
    for (UINT i = 0; i < cases.size(); i++) {
      const auto &c = cases[i];
      expected[i].fill(destination);
      if (c.independent && level == D3D10_FEATURE_LEVEL_10_0)
        continue;
      step(
          "level=%#x blend case=%u src=%u dst=%u op=%u src_alpha=%u dst_alpha=%u alpha_op=%u independent=%u", level, i,
          c.desc.RenderTarget[0].SrcBlend, c.desc.RenderTarget[0].DestBlend, c.desc.RenderTarget[0].BlendOp,
          c.desc.RenderTarget[0].SrcBlendAlpha, c.desc.RenderTarget[0].DestBlendAlpha,
          c.desc.RenderTarget[0].BlendOpAlpha, c.independent
      );
      ComPtr<ID3D10BlendState> state;
      if (c.independent) {
        ComPtr<ID3D10BlendState1> state1;
        CHECK(device->CreateBlendState1(&c.desc, &state1));
        state = state1;
      } else {
        D3D10_BLEND_DESC desc{};
        const auto &r = c.desc.RenderTarget[0];
        desc.SrcBlend = r.SrcBlend;
        desc.DestBlend = r.DestBlend;
        desc.BlendOp = r.BlendOp;
        desc.SrcBlendAlpha = r.SrcBlendAlpha;
        desc.DestBlendAlpha = r.DestBlendAlpha;
        desc.BlendOpAlpha = r.BlendOpAlpha;
        for (UINT rt = 0; rt < std::size(targets); rt++) {
          desc.BlendEnable[rt] = c.desc.RenderTarget[rt].BlendEnable;
          desc.RenderTargetWriteMask[rt] = c.desc.RenderTarget[rt].RenderTargetWriteMask;
        }
        CHECK(device->CreateBlendState(&desc, &state));
      }
      const UINT target_count = c.dual_source ? 1 : UINT(std::size(targets));
      if (colors[0] != c.color) {
        colors[0] = c.color;
        device->UpdateSubresource(constants.Get(), 0, nullptr, colors, 0, 0);
      }
      device->OMSetRenderTargets(target_count, rtvs, nullptr);
      device->PSSetShader(c.dual_source ? second_ps.Get() : ps.Get());
      device->OMSetBlendState(state.Get(), blend_factor.data(), D3D10_DEFAULT_SAMPLE_MASK);
      D3D10_VIEWPORT viewport{INT(i), 0, 1, 1, 0, 1};
      device->RSSetViewports(1, &viewport);
      device->Draw(1, 0);
      device->OMSetBlendState(nullptr, nullptr, D3D10_DEFAULT_SAMPLE_MASK);
      for (UINT rt = 0; rt < target_count; rt++) {
        auto r = c.desc.RenderTarget[rt];
        if (!c.independent) {
          const auto &common = c.desc.RenderTarget[0];
          r.SrcBlend = common.SrcBlend;
          r.DestBlend = common.DestBlend;
          r.BlendOp = common.BlendOp;
          r.SrcBlendAlpha = common.SrcBlendAlpha;
          r.DestBlendAlpha = common.DestBlendAlpha;
          r.BlendOpAlpha = common.BlendOpAlpha;
        }
        for (UINT component = 0; component < source.size(); component++)
          if (r.RenderTargetWriteMask & (1u << component))
            expected[i][rt][component] =
                !r.BlendEnable ? c.color[component]
                               : combine(
                                     component == source.size() - 1 ? r.BlendOpAlpha : r.BlendOp,
                                     component == source.size() - 1 ? r.SrcBlendAlpha : r.SrcBlend,
                                     component == source.size() - 1 ? r.DestBlendAlpha : r.DestBlend, component, c.color
                                 );
      }
    }
    device->ClearState();
    for (UINT rt = 0; rt < std::size(targets); rt++) {
      device->CopyResource(staging.Get(), targets[rt].Get());
      D3D10_MAPPED_TEXTURE2D mapped{};
      CHECK(staging->Map(0, D3D10_MAP_READ, 0, &mapped));
      const auto pixels = static_cast<const Color *>(mapped.pData);
      for (UINT i = 0; i < cases.size(); i++)
        for (UINT component = 0; component < source.size(); component++)
          expect(
              pixels[i][component] == expected[i][rt][component],
              "level=%#x case=%u target=%u component=%u got=%g wanted=%g", level, i, rt, component,
              pixels[i][component], expected[i][rt][component]
          );
      staging->Unmap(0);
    }
  }
  return verdict();
}
