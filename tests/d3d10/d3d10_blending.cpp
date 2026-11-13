// contract: D3D11.3 17.1-17.6 carries the D3D10 blend equations; 10.1 adds
// independent equations per target. "The masks simply limit writes." (17.15).
// "The min and max blend ops ignore D3D11_BLEND modes" (17.1). source:
// D3D11_3_FunctionalSpec.htm. om_blend.hpp supplies CPU equations and section 3
// storage conversions. each device is created through D3D10CreateDevice1; no
// inactive D3D10 interface from a D3D11 device is used.
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include "../om_blend.hpp"

int main() {
  const auto cases = om::blend_cases();
  const UINT count = D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT,
             width = UINT(cases.size()) + 2;
  struct Code {
    ComPtr<ID3DBlob> vs, ps[3];
  } code[2];
  const char *entries[] = {"ps", "ps_dual", "ps_special"};
  for (UINT version = 0; version < std::size(code); version++) {
    std::vector<std::string> defines{"TARGETS=" + std::to_string(count)};
    code[version].vs =
        compile(om::blend_hlsl, "vs", version ? "vs_4_1" : "vs_4_0", defines);
    for (UINT p = 0; p < std::size(entries); p++) {
      code[version].ps[p] = compile(om::blend_hlsl, entries[p],
                                    version ? "ps_4_1" : "ps_4_0", defines);
      if (!expect(code[version].ps[p], "D3D10 pixel HLSL did not compile"))
        return verdict();
    }
    if (!expect(code[version].vs, "D3D10 vertex HLSL did not compile"))
      return verdict();
  }
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>((
      void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create, "D3D10CreateDevice1 unavailable"))
    return verdict();
  for (UINT version = 0; version < std::size(code); version++) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
                 version ? D3D10_FEATURE_LEVEL_10_1 : D3D10_FEATURE_LEVEL_10_0,
                 D3D10_1_SDK_VERSION, &device));
    ComPtr<ID3D10VertexShader> vs;
    ComPtr<ID3D10PixelShader> ps[3];
    CHECK(device->CreateVertexShader(code[version].vs->GetBufferPointer(),
                                     code[version].vs->GetBufferSize(), &vs));
    for (UINT p = 0; p < std::size(entries); p++)
      CHECK(device->CreatePixelShader(code[version].ps[p]->GetBufferPointer(),
                                      code[version].ps[p]->GetBufferSize(),
                                      &ps[p]));
    D3D10_BUFFER_DESC cbd{sizeof(om::Color) * 2, D3D10_USAGE_DEFAULT,
                          D3D10_BIND_CONSTANT_BUFFER};
    ComPtr<ID3D10Buffer> cb;
    CHECK(device->CreateBuffer(&cbd, nullptr, &cb));
    D3D10_RASTERIZER_DESC rd{D3D10_FILL_SOLID, D3D10_CULL_NONE};
    rd.DepthClipEnable = rd.ScissorEnable = TRUE;
    ComPtr<ID3D10RasterizerState> raster;
    CHECK(device->CreateRasterizerState(&rd, &raster));
    for (auto f : om::formats) {
      UINT support;
      CHECK(device->CheckFormatSupport(f.value, &support));
      if (!expect(f.value != DXGI_FORMAT_R8G8B8A8_UNORM ||
                      (support & D3D10_FORMAT_SUPPORT_BLENDABLE),
                  "mandatory UNORM blending missing"))
        return verdict();
      if (!(support & D3D10_FORMAT_SUPPORT_BLENDABLE)) {
        printf("unsupported blend format %u\n", f.value);
        continue;
      }
      step("D3D10.%u blend format %u", version, f.value);
      const UINT stride =
          sizeof(om::Color) * f.bits / (sizeof(float) * CHAR_BIT);
      std::vector<unsigned char> initial(width * stride);
      for (UINT x = 0; x < width; x++)
        om::seed(f, cases[x && x <= cases.size() ? x - 1 : 0].destination,
                 initial.data() + x * stride);
      D3D10_TEXTURE2D_DESC td{width,
                              1,
                              1,
                              1,
                              f.value,
                              {1, 0},
                              D3D10_USAGE_DEFAULT,
                              D3D10_BIND_RENDER_TARGET};
      D3D10_SUBRESOURCE_DATA data{initial.data(), width * stride};
      ComPtr<ID3D10Texture2D> textures[count], staging;
      ComPtr<ID3D10RenderTargetView> views[count];
      ID3D10RenderTargetView *bound[count];
      for (UINT slot = 0; slot < count; slot++) {
        CHECK(device->CreateTexture2D(&td, &data, &textures[slot]));
        CHECK(device->CreateRenderTargetView(textures[slot].Get(), nullptr,
                                             &views[slot]));
        bound[slot] = views[slot].Get();
      }
      td.Usage = D3D10_USAGE_STAGING;
      td.BindFlags = 0;
      td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&td, nullptr, &staging));
      device->RSSetState(raster.Get());
      device->VSSetShader(vs.Get());
      device->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
      device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      D3D10_VIEWPORT viewport{0, 0, width, 1, 0, 1};
      device->RSSetViewports(1, &viewport);
      for (UINT i = 0; i < cases.size(); i++) {
        const auto &k = cases[i];
        if (k.special && (f.normalized || f.bits != 32))
          continue;
        step("D3D10.%u format %u equation %u", version, f.value, i);
        ComPtr<ID3D10BlendState> blend;
        if (!version) {
          const auto &r = k.state.RenderTarget[0];
          D3D10_BLEND_DESC d{};
          d.SrcBlend = D3D10_BLEND(r.SrcBlend);
          d.DestBlend = D3D10_BLEND(r.DestBlend);
          d.SrcBlendAlpha = D3D10_BLEND(r.SrcBlendAlpha);
          d.DestBlendAlpha = D3D10_BLEND(r.DestBlendAlpha);
          d.BlendOp = D3D10_BLEND_OP(r.BlendOp);
          d.BlendOpAlpha = D3D10_BLEND_OP(r.BlendOpAlpha);
          for (UINT slot = 0; slot < count; slot++) {
            const auto &per_target =
                k.state.RenderTarget[k.state.IndependentBlendEnable ? slot : 0];
            d.BlendEnable[slot] = per_target.BlendEnable;
            d.RenderTargetWriteMask[slot] = per_target.RenderTargetWriteMask;
          }
          CHECK(device->CreateBlendState(&d, &blend));
        } else {
          D3D10_BLEND_DESC1 d{};
          d.IndependentBlendEnable = k.state.IndependentBlendEnable;
          for (UINT slot = 0; slot < count; slot++) {
            const auto &r = k.state.RenderTarget[slot];
            d.RenderTarget[slot] = {r.BlendEnable,
                                    D3D10_BLEND(r.SrcBlend),
                                    D3D10_BLEND(r.DestBlend),
                                    D3D10_BLEND_OP(r.BlendOp),
                                    D3D10_BLEND(r.SrcBlendAlpha),
                                    D3D10_BLEND(r.DestBlendAlpha),
                                    D3D10_BLEND_OP(r.BlendOpAlpha),
                                    r.RenderTargetWriteMask};
          }
          ComPtr<ID3D10BlendState1> blend1;
          CHECK(device->CreateBlendState1(&d, &blend1));
          CHECK(blend1.As(&blend));
        }
        const om::Color params[] = {k.source, k.secondary};
        device->UpdateSubresource(cb.Get(), 0, nullptr, params, 0, 0);
        D3D10_RECT rect{LONG(i + 1), 0, LONG(i + 2), 1};
        device->RSSetScissorRects(1, &rect);
        device->OMSetRenderTargets(k.dual ? 1 : count, bound, nullptr);
        device->OMSetBlendState(blend.Get(),
                                k.null_factor ? nullptr : k.factor.data(), ~0u);
        device->PSSetShader(ps[k.dual ? 1 : k.special ? 2 : 0].Get());
        device->Draw(3, 0);
      }
      device->OMSetRenderTargets(0, nullptr, nullptr);
      for (UINT slot = 0; slot < count; slot++) {
        device->CopyResource(staging.Get(), textures[slot].Get());
        D3D10_MAPPED_TEXTURE2D mapped{};
        CHECK(staging->Map(0, D3D10_MAP_READ, 0, &mapped));
        for (UINT x = 0; x < width; x++) {
          auto got =
              static_cast<const unsigned char *>(mapped.pData) + x * stride;
          bool active = x && x <= cases.size();
          if (active) {
            const auto &k = cases[x - 1];
            active = !(k.special && (f.normalized || f.bits != 32)) &&
                     (!k.dual || !slot);
          }
          if (active) {
            step("D3D10.%u format %u slot %u equation %u: bytes", version,
                 f.value, slot, x - 1);
            auto expected = cases[x - 1];
            if (!version) {
              const auto &per_target =
                  expected.state
                      .RenderTarget[expected.state.IndependentBlendEnable ? slot
                                                                          : 0];
              auto &common = expected.state.RenderTarget[0];
              common.BlendEnable = per_target.BlendEnable;
              common.RenderTargetWriteMask = per_target.RenderTargetWriteMask;
              expected.state.IndependentBlendEnable = FALSE;
            }
            om::compare(f, expected, slot, initial.data() + x * stride, got);
          } else
            expect(!memcmp(got, initial.data() + x * stride, stride),
                   "untouched column %u changed", x);
        }
        staging->Unmap(0);
      }
    }
  }
  return verdict();
}
