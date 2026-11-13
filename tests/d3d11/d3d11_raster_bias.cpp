// contract: D3D11.3 15.10: "Where r is the minimum representable value > 0 in the depth buffer format,
// converted to float32." for float depth, r is the mantissa width, excluding the hidden bit.
// "Depth Bias is not applied to any point or line primitives, except for lines drawn in wireframe mode"
// (quotation ends before the Fill Modes cross-reference). ../raster_bias.hpp computes the bias and encoded
// depth independently; all depth formats are checked through immediate and deferred draws.
#include "d3d11_test.hpp"
#include "../raster_bias.hpp"
#include <cstddef>

int main() {
  if (!raster::reference_contract())
    return 1;
  step("compile depth-only vertex shader before device creation");
  auto code = compile(raster::hlsl, "vs", "vs", {"DISTANCES=0"});
  if (!code)
    return 1;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                          nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  CHECK(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs));
  D3D11_INPUT_ELEMENT_DESC input{
      "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(raster::Vertex, position), D3D11_INPUT_PER_VERTEX_DATA,
      0};
  ComPtr<ID3D11InputLayout> layout;
  CHECK(device->CreateInputLayout(&input, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout));
  D3D11_BUFFER_DESC bd{sizeof(raster::Vertex) * raster::Triangle, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
  ComPtr<ID3D11Buffer> vb;
  CHECK(device->CreateBuffer(&bd, nullptr, &vb));
  D3D11_DEPTH_STENCIL_DESC dd{};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
  ComPtr<ID3D11DepthStencilState> ds;
  CHECK(device->CreateDepthStencilState(&dd, &ds));
  const auto all = raster_bias::cases();
  for (auto &format : raster_bias::formats()) {
    UINT support = 0;
    CHECK(device->CheckFormatSupport(format.value, &support));
    if (!(support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL)) {
      printf("unsupported: %s\n", format.name.c_str());
      continue;
    }
    D3D11_TEXTURE2D_DESC td{raster::width,       raster::height,          1, 1, format.value, {1, 0},
                            D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL};
    ComPtr<ID3D11Texture2D> depth, staging;
    ComPtr<ID3D11DepthStencilView> dsv;
    CHECK(device->CreateTexture2D(&td, nullptr, &depth));
    CHECK(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&td, nullptr, &staging));
    for (auto context : {immediate.Get(), deferred.Get()})
      for (size_t n = 0; n < all.size(); n++) {
        const auto &c = all[n];
        auto geometry = raster_bias::geometry(c);
        step("%s %s bias case %zu", context == immediate.Get() ? "immediate" : "deferred", format.name.c_str(), n);
        D3D11_RASTERIZER_DESC rd{c.wire ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID, D3D11_CULL_NONE};
        rd.DepthClipEnable = c.depth_clip;
        rd.DepthBias = c.bias;
        rd.DepthBiasClamp = c.clamp;
        rd.SlopeScaledDepthBias = c.scale;
        ComPtr<ID3D11RasterizerState> state;
        CHECK(device->CreateRasterizerState(&rd, &state));
        D3D11_RASTERIZER_DESC got;
        state->GetDesc(&got);
        expect(got.DepthBias == rd.DepthBias &&
                   (got.SlopeScaledDepthBias == rd.SlopeScaledDepthBias ||
                    (std::isnan(got.SlopeScaledDepthBias) && std::isnan(rd.SlopeScaledDepthBias))) &&
                   (got.DepthBiasClamp == rd.DepthBiasClamp ||
                    (std::isnan(got.DepthBiasClamp) && std::isnan(rd.DepthBiasClamp))),
               "depth-bias state description changed");
        context->RSSetState(c.default_state ? nullptr : state.Get());
        context->OMSetDepthStencilState(ds.Get(), 0);
        D3D11_VIEWPORT vp{0, 0, raster::width, raster::height, c.low, c.high};
        context->RSSetViewports(1, &vp);
        std::array<raster::Vertex, raster::Triangle> vertices{};
        std::copy(geometry.vertices.begin(), geometry.vertices.end(), vertices.begin());
        context->UpdateSubresource(vb.Get(), 0, nullptr, vertices.data(), 0, 0);
        const UINT stride = sizeof(raster::Vertex), offset = 0;
        context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(c.shape == raster::Point  ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
                                        : c.shape == raster::Line ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                                                  : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(nullptr, nullptr, 0);
        context->OMSetRenderTargets(0, nullptr, dsv.Get());
        context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        context->Draw(geometry.vertices.size(), 0);
        context->CopyResource(staging.Get(), depth.Get());
        if (context == deferred.Get()) {
          ComPtr<ID3D11CommandList> commands;
          CHECK(deferred->FinishCommandList(FALSE, &commands));
          immediate->ExecuteCommandList(commands.Get(), FALSE);
        }
        D3D11_MAPPED_SUBRESOURCE mapped;
        CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        raster_bias::compare(format, c, static_cast<const char *>(mapped.pData), mapped.RowPitch);
        immediate->Unmap(staging.Get(), 0);
      }
  }
  return verdict();
}
