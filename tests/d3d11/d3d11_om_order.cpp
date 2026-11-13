// contract: D3D11.3 16.12.1 requires "Depth/Stencil tests and
// depth/stencil writes before executing the Pixel Shader" for forced early
// shaders, and "nothing downstream further impacts the count." 16.12.2
// disables early rejection for default UAV shaders. 16.13 preserves UAV writes
// before discard and suppresses later writes. each draw checks all five
// outputs: color, depth, stencil, two UAV counters and an occlusion count.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#include "d3d11_test.hpp"
#include "../om_order.hpp"

int main() {
  const auto cases = om::order_cases();
  const UINT width = om::order_width, height = UINT(cases.size());
  const std::vector<std::string> defines{"COLOR_VALUE=" +
                                         std::to_string(om::order_color)};
  auto vertex = compile(om::order_hlsl, "vs", "vs", defines);
  const auto &entries = om::order_entries;
  ComPtr<ID3DBlob> pixels[std::size(entries)];
  for (UINT i = 0; i < std::size(entries); i++) {
    pixels[i] = compile(om::order_hlsl, entries[i], "ps", defines);
    if (!expect(bool(pixels[i]), "order HLSL %s did not compile", entries[i]))
      return verdict();
  }
  if (!expect(bool(vertex), "order vertex HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level,
                          1, D3D11_SDK_VERSION, &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps[std::size(entries)];
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(),
                                   vertex->GetBufferSize(), nullptr, &vs));
  for (UINT i = 0; i < std::size(entries); i++)
    CHECK(device->CreatePixelShader(pixels[i]->GetBufferPointer(),
                                    pixels[i]->GetBufferSize(), nullptr,
                                    &ps[i]));
  struct Params {
    float raster_depth, output_depth;
    UINT discard, width;
  };
  D3D11_BUFFER_DESC cb_desc{sizeof(Params), D3D11_USAGE_DEFAULT,
                            D3D11_BIND_CONSTANT_BUFFER};
  ComPtr<ID3D11Buffer> cb;
  CHECK(device->CreateBuffer(&cb_desc, nullptr, &cb));
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster_desc.DepthClipEnable = raster_desc.ScissorEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));
  for (bool recorded : {false, true}) {
    step("output ordering, deferred %u: initialize", recorded);
    immediate->ClearState();
    deferred->ClearState();
    auto context = recorded ? deferred.Get() : immediate.Get();
    ComPtr<ID3D11Texture2D> color, depth, color_read, depth_read;
    D3D11_TEXTURE2D_DESC td{width,
                            height,
                            1,
                            1,
                            DXGI_FORMAT_R32_UINT,
                            {1, 0},
                            D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET};
    CHECK(device->CreateTexture2D(&td, nullptr, &color));
    td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    CHECK(device->CreateTexture2D(&td, nullptr, &depth));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&td, nullptr, &depth_read));
    td.Format = DXGI_FORMAT_R32_UINT;
    CHECK(device->CreateTexture2D(&td, nullptr, &color_read));
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    CHECK(device->CreateRenderTargetView(color.Get(), nullptr, &rtv));
    CHECK(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv));
    ComPtr<ID3D11Buffer> counters;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{DXGI_FORMAT_UNKNOWN,
                                        D3D11_UAV_DIMENSION_BUFFER};
    ud.Buffer.NumElements = 2 * width * height;
    D3D11_BUFFER_DESC bd{ud.Buffer.NumElements * UINT(sizeof(UINT)),
                         D3D11_USAGE_DEFAULT,
                         D3D11_BIND_UNORDERED_ACCESS,
                         0,
                         D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                         sizeof(UINT)};
    std::vector<UINT> zero(ud.Buffer.NumElements);
    D3D11_SUBRESOURCE_DATA initial{zero.data()};
    CHECK(device->CreateBuffer(&bd, &initial, &counters));
    ComPtr<ID3D11UnorderedAccessView> uav;
    CHECK(device->CreateUnorderedAccessView(counters.Get(), &ud, &uav));
    const float black[4]{};
    context->ClearRenderTargetView(rtv.Get(), black);
    context->ClearDepthStencilView(dsv.Get(),
                                   D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                   om::order_stored_depth, om::order_stencil);
    context->OMSetRenderTargetsAndUnorderedAccessViews(
        1, rtv.GetAddressOf(), dsv.Get(), 1, 1, uav.GetAddressOf(), nullptr);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
    context->RSSetState(raster.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    std::vector<ComPtr<ID3D11Query>> queries(cases.size());
    std::vector<ComPtr<ID3D11DepthStencilState>> states(cases.size());
    const D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION};
    for (UINT y = 0; y < cases.size(); y++) {
      const auto &k = cases[y];
      step("output ordering, deferred %u, case %u", recorded, y);
      auto state = om::order_state(k);
      CHECK(device->CreateDepthStencilState(&state, &states[y]));
      CHECK(device->CreateQuery(&qd, &queries[y]));
      Params params{k.raster_depth, 1 - k.raster_depth, k.discard, width};
      context->UpdateSubresource(cb.Get(), 0, nullptr, &params, 0, 0);
      context->PSSetShader(ps[k.shader].Get(), nullptr, 0);
      context->OMSetDepthStencilState(states[y].Get(), om::order_reference);
      D3D11_VIEWPORT viewport{0, float(y), float(width), 1, 0, 1};
      D3D11_RECT scissor{1, LONG(y), LONG(width - 1), LONG(y + 1)};
      context->RSSetViewports(1, &viewport);
      context->RSSetScissorRects(1, &scissor);
      context->Begin(queries[y].Get());
      context->Draw(3, 0);
      context->End(queries[y].Get());
    }
    ID3D11UnorderedAccessView *none = nullptr;
    context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1,
                                                       1, &none, nullptr);
    if (recorded) {
      ComPtr<ID3D11CommandList> commands;
      CHECK(deferred->FinishCommandList(FALSE, &commands));
      immediate->ExecuteCommandList(commands.Get(), FALSE);
    }
    immediate->CopyResource(color_read.Get(), color.Get());
    immediate->CopyResource(depth_read.Get(), depth.Get());
    auto words = read(device.Get(), immediate.Get(), counters.Get());
    if (!expect(words.size() == zero.size(), "order UAV readback missing"))
      return verdict();
    D3D11_MAPPED_SUBRESOURCE colors{}, depths{};
    CHECK(immediate->Map(color_read.Get(), 0, D3D11_MAP_READ, 0, &colors));
    CHECK(immediate->Map(depth_read.Get(), 0, D3D11_MAP_READ, 0, &depths));
    for (UINT y = 0; y < cases.size(); y++) {
      step("output ordering, deferred %u, case %u: readback", recorded, y);
      UINT64 want_count = 0, count = UINT64_MAX;
      for (UINT x = 0; x < width; x++) {
        auto want = om::order_result(cases[y], x);
        want_count += want.occlusion;
        UINT color_word, depth_word;
        memcpy(&color_word,
               static_cast<const char *>(colors.pData) + y * colors.RowPitch +
                   x * sizeof(UINT),
               sizeof(UINT));
        auto p = static_cast<const unsigned char *>(depths.pData) +
                 y * depths.RowPitch + x * (sizeof(float) + sizeof(UINT));
        memcpy(&depth_word, p, sizeof(UINT));
        UINT i = 2 * (y * width + x);
        expect(color_word == want.color &&
                   depth_word == std::bit_cast<UINT>(want.depth) &&
                   p[sizeof(float)] == want.stencil &&
                   words[i] == want.before && words[i + 1] == want.after,
               "pixel %u: color %u/%u, depth %#x/%#x, stencil %u/%u, UAV "
               "%u,%u/%u,%u",
               x, color_word, want.color, depth_word,
               std::bit_cast<UINT>(want.depth), p[sizeof(float)], want.stencil,
               words[i], words[i + 1], want.before, want.after);
      }
      HRESULT hr;
      do {
        hr = immediate->GetData(queries[y].Get(), &count, sizeof(count), 0);
      } while (hr == S_FALSE && SUCCEEDED(device->GetDeviceRemovedReason()));
      expect(hr == S_OK && count == want_count,
             "occlusion returned %08lx, count %llu/%llu", hr, count,
             want_count);
    }
    immediate->Unmap(depth_read.Get(), 0);
    immediate->Unmap(color_read.Get(), 0);
  }
  return verdict();
}
