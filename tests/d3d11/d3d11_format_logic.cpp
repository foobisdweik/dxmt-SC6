// contract: "Logic ops are supported only on renderable UINT formats. Implementations that expose Logic Ops
// support must support them for all renderable UINT formats." (D3D11.3 17.7.1). 17.1 lists all sixteen Boolean
// equations. "The masks simply limit writes." (17.15). each component bit sees all four operand pairs, plus all-bit
// patterns; every operation and channel mask checks native bytes, immediately and through command lists. operands have
// no bits above the format width, avoiding the truncation/clamping choice in 17.7.1.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/ne-d3d11_1-d3d11_logic_op
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include "../depth_formats.hpp"
#include "../format_logic.hpp"

using namespace format_logic;

int
main() {
  auto vertex = compile(format_logic::hlsl, "vs", "vs"), pixel = compile(format_logic::hlsl, "ps", "ps");
  if (!expect(vertex && pixel, "logic HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11Device1> device1;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate
  ));
  CHECK(device.As(&device1));
  CHECK(device->CreateDeferredContext(0, &deferred));
  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &ps));
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));
  UINT checked = 0;
  auto typed = formats();
  const struct {
    DXGI_FORMAT id;
    const char *name;
  } candidates[] = {DXGI_FORMATS};
  for (auto candidate : candidates) {
    UINT support = 0;
    HRESULT hr = device->CheckFormatSupport(candidate.id, &support);
    if (FAILED(hr)) {
      expect(hr == E_FAIL && !support, "%s unsupported query returned %08lx bits %#x", candidate.name, hr, support);
      continue;
    }
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2{candidate.id};
    CHECK(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support2, sizeof(support2)));
    const bool renderable = (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0;
    auto layout = std::find_if(typed.begin(), typed.end(), [&](const auto &f) { return f.id == candidate.id; });
    const bool legal = layout != typed.end() && layout->kind == Unsigned && renderable;
    expect(
        bool(support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP) ==
            (legal && bool(options.OutputMergerLogicOp)),
        "%s logic capability disagrees with format/global support", candidate.name
    );
    if (!legal)
      continue;
    const auto &f = *layout;
    checked++;
    std::vector<Pixel> source;
    std::vector<uint8_t> destination;
    inputs(f, source, destination);
    const UINT count = UINT(source.size());
    D3D11_BUFFER_DESC bd{UINT(source.size() * sizeof(Pixel)),   D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0,
                         D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(Pixel)};
    D3D11_SUBRESOURCE_DATA data{source.data()};
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> srv;
    CHECK(device->CreateBuffer(&bd, &data, &buffer));
    CHECK(device->CreateShaderResourceView(buffer.Get(), nullptr, &srv));
    D3D11_TEXTURE2D_DESC td{count, 1, 1, 1, f.id, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    data = {destination.data(), count * f.bytes};
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    CHECK(device->CreateTexture2D(&td, &data, &target));
    CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&td, nullptr, &staging));
    for (UINT op = D3D11_LOGIC_OP_CLEAR; op <= D3D11_LOGIC_OP_OR_INVERTED; op++)
      for (UINT mask = 0; mask <= D3D11_COLOR_WRITE_ENABLE_ALL; mask++) {
        D3D11_BLEND_DESC1 desc{};
        desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].LogicOpEnable = TRUE;
        desc.RenderTarget[0].LogicOp = D3D11_LOGIC_OP(op);
        desc.RenderTarget[0].RenderTargetWriteMask = UINT8(mask);
        ComPtr<ID3D11BlendState1> blend;
        hr = device1->CreateBlendState1(&desc, &blend);
        step("%s op %u mask %#x", f.name.c_str(), op, mask);
        if (!options.OutputMergerLogicOp) {
          expect(FAILED(hr) && !blend, "unsupported logic operation creation returned %08lx", hr);
          continue;
        }
        if (!expect(hr == S_OK && blend, "supported logic operation creation returned %08lx", hr))
          return verdict();
        for (bool listed : {false, true}) {
          auto context = listed ? deferred.Get() : immediate.Get();
          context->ClearState();
          context->UpdateSubresource(target.Get(), 0, nullptr, destination.data(), count * f.bytes, 0);
          context->VSSetShader(vs.Get(), nullptr, 0);
          context->PSSetShader(ps.Get(), nullptr, 0);
          context->PSSetShaderResources(0, 1, srv.GetAddressOf());
          context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
          context->OMSetBlendState(blend.Get(), nullptr, ~0u);
          context->RSSetState(raster.Get());
          D3D11_VIEWPORT viewport{0, 0, float(count), 1, 0, 1};
          context->RSSetViewports(1, &viewport);
          context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          context->Draw(3, 0);
          context->ClearState();
          context->CopyResource(staging.Get(), target.Get());
          if (listed) {
            ComPtr<ID3D11CommandList> list;
            CHECK(deferred->FinishCommandList(FALSE, &list));
            immediate->ExecuteCommandList(list.Get(), FALSE);
          }
          D3D11_MAPPED_SUBRESOURCE mapped{};
          CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
          bool same = true;
          for (UINT i = 0; i < count; i++)
            if (!matches(
                    f, op, mask, source[i], destination.data() + i * f.bytes,
                    static_cast<const uint8_t *>(mapped.pData) + i * f.bytes
                )) {
              expect(false, "context %u pixel %u differs from Boolean equation or channel mask", listed, i);
              same = false;
              break;
            }
          immediate->Unmap(staging.Get(), 0);
          if (!same)
            return verdict();
        }
      }
  }
  expect(checked != 0, "no renderable UINT format was checked");
  return verdict();
}
