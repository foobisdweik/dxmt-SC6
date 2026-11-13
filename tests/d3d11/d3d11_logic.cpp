// contract: D3D11.3 17.7.1: "Implementations that expose Logic Ops support must
// support them for all renderable UINT formats." 17.1 lists every truth
// table; 17.15: "The masks simply limit writes." source:
// D3D11_3_FunctionalSpec.htm. the CPU truth tables in om_logic.hpp check all
// four input-bit pairs, alternating patterns and every component write mask,
// across eight targets and both context types.
#include "d3d11_test.hpp"
#include "../om_logic.hpp"

int main() {
  auto vertex = compile(om::logic_hlsl, "vs", "vs"),
       pixel = compile(om::logic_hlsl, "ps", "ps");
  if (!expect(vertex && pixel, "logic HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level,
                          1, D3D11_SDK_VERSION, &device, nullptr, &immediate));
  ComPtr<ID3D11Device1> device1;
  CHECK(device.As(&device1));
  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options,
                                    sizeof(options)));
  if (!options.OutputMergerLogicOp) {
    printf("skipped: output merger logic ops not supported\n");
    return 77;
  }
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(),
                                   vertex->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(pixel->GetBufferPointer(),
                                  pixel->GetBufferSize(), nullptr, &ps));
  D3D11_BUFFER_DESC cb_desc{sizeof(UINT[4]), D3D11_USAGE_DEFAULT,
                            D3D11_BIND_CONSTANT_BUFFER};
  ComPtr<ID3D11Buffer> cb;
  CHECK(device->CreateBuffer(&cb_desc, nullptr, &cb));
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster_desc.DepthClipEnable = raster_desc.ScissorEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));
  const auto formats = om::logic_formats();
  if (!expect(!formats.empty(), "no UINT formats read from header"))
    return verdict();
  UINT exercised = 0;
  for (auto f : formats) {
    UINT support;
    CHECK(device->CheckFormatSupport(f.value, &support));
    if (!(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
      continue;
    exercised++;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 more{f.value};
    CHECK(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &more,
                                      sizeof(more)));
    expect(more.OutFormatSupport2 &
               D3D11_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP,
           "renderable UINT format %u lacks logic-op support bit", f.value);
    for (bool recorded : {false, true}) {
      step("logic format %u, deferred %u", f.value, recorded);
      immediate->ClearState();
      deferred->ClearState();
      auto context = recorded ? deferred.Get() : immediate.Get();
      constexpr UINT count = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
      ComPtr<ID3D11Texture2D> textures[count], staging;
      ComPtr<ID3D11RenderTargetView> views[count];
      ID3D11RenderTargetView *bound[count];
      D3D11_TEXTURE2D_DESC desc{om::logic_width,
                                UINT(std::size(om::logic_patterns)),
                                1,
                                1,
                                f.value,
                                {1, 0},
                                D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET};
      for (UINT slot = 0; slot < count; slot++) {
        auto initial = om::logic_seed(f, slot);
        D3D11_SUBRESOURCE_DATA data{initial.data(), om::logic_width * f.stride};
        CHECK(device->CreateTexture2D(&desc, &data, &textures[slot]));
        CHECK(device->CreateRenderTargetView(textures[slot].Get(), nullptr,
                                             &views[slot]));
        bound[slot] = views[slot].Get();
      }
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
      context->RSSetState(raster.Get());
      context->VSSetShader(vs.Get(), nullptr, 0);
      context->PSSetShader(ps.Get(), nullptr, 0);
      context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
      context->OMSetRenderTargets(count, bound, nullptr);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      D3D11_VIEWPORT viewport{
          0, 0, float(om::logic_width), float(std::size(om::logic_patterns)),
          0, 1};
      context->RSSetViewports(1, &viewport);
      for (UINT op = D3D11_LOGIC_OP_CLEAR; op <= D3D11_LOGIC_OP_OR_INVERTED;
           op++)
        for (UINT mask = 0; mask < om::logic_masks; mask++) {
          D3D11_BLEND_DESC1 desc{};
          auto &r = desc.RenderTarget[0];
          r.LogicOpEnable = TRUE;
          r.LogicOp = D3D11_LOGIC_OP(op);
          r.RenderTargetWriteMask = UINT8(mask);
          ComPtr<ID3D11BlendState1> blend;
          CHECK(device1->CreateBlendState1(&desc, &blend));
          context->OMSetBlendState(blend.Get(), nullptr, ~0u);
          for (UINT y = 0; y < std::size(om::logic_patterns); y++) {
            UINT source[4]{};
            for (UINT c = 0; c < f.channels; c++)
              source[c] = om::logic_source(f, y, c);
            context->UpdateSubresource(cb.Get(), 0, nullptr, source, 0, 0);
            UINT x = op * om::logic_masks + mask + 1;
            D3D11_RECT rect{LONG(x), LONG(y), LONG(x + 1), LONG(y + 1)};
            context->RSSetScissorRects(1, &rect);
            context->Draw(3, 0);
          }
        }
      context->OMSetRenderTargets(0, nullptr, nullptr);
      if (recorded) {
        ComPtr<ID3D11CommandList> commands;
        CHECK(deferred->FinishCommandList(FALSE, &commands));
        immediate->ExecuteCommandList(commands.Get(), FALSE);
      }
      for (UINT slot = 0; slot < count; slot++) {
        step("logic format %u, deferred %u, slot %u: bytes", f.value, recorded,
             slot);
        immediate->CopyResource(staging.Get(), textures[slot].Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        om::check_logic(f, slot,
                        static_cast<const unsigned char *>(mapped.pData),
                        mapped.RowPitch);
        immediate->Unmap(staging.Get(), 0);
      }
    }
  }
  expect(exercised, "no renderable UINT format exercised");
  return verdict();
}
