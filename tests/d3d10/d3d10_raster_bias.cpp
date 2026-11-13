// contract: D3D11.3 15.10: "Where r is the minimum representable value > 0 in the depth buffer format,
// converted to float32." for float depth, r is the mantissa width, excluding the hidden bit.
// "Depth Bias is not applied to any point or line primitives, except for lines drawn in wireframe mode"
// (quotation ends before the Fill Modes cross-reference). ../raster_bias.hpp computes the bias and encoded
// depth independently; all depth formats are checked through Direct3D 10 draws.
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include "../raster_bias.hpp"
#include <cstddef>

int main() {
  if (!raster::reference_contract())
    return 1;
  step("compile depth-only vertex shader before device creation");
  auto code = compile(raster::hlsl, "vs", "vs_4_0", {"DISTANCES=0"});
  if (!code)
    return 1;
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  ComPtr<ID3D10Device1> device;
  CHECK(
      create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &device));
  ComPtr<ID3D10VertexShader> vs;
  CHECK(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), &vs));
  D3D10_INPUT_ELEMENT_DESC input{
      "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(raster::Vertex, position), D3D10_INPUT_PER_VERTEX_DATA,
      0};
  ComPtr<ID3D10InputLayout> layout;
  CHECK(device->CreateInputLayout(&input, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout));
  D3D10_BUFFER_DESC bd{sizeof(raster::Vertex) * raster::Triangle, D3D10_USAGE_DEFAULT, D3D10_BIND_VERTEX_BUFFER};
  ComPtr<ID3D10Buffer> vb;
  CHECK(device->CreateBuffer(&bd, nullptr, &vb));
  D3D10_DEPTH_STENCIL_DESC dd{};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D10_COMPARISON_ALWAYS;
  ComPtr<ID3D10DepthStencilState> ds;
  CHECK(device->CreateDepthStencilState(&dd, &ds));
  const auto all = raster_bias::cases();
  for (auto &format : raster_bias::formats()) {
    UINT support = 0;
    CHECK(device->CheckFormatSupport(format.value, &support));
    if (!(support & D3D10_FORMAT_SUPPORT_DEPTH_STENCIL)) {
      printf("unsupported: %s\n", format.name.c_str());
      continue;
    }
    D3D10_TEXTURE2D_DESC td{raster::width,       raster::height,          1, 1, format.value, {1, 0},
                            D3D10_USAGE_DEFAULT, D3D10_BIND_DEPTH_STENCIL};
    ComPtr<ID3D10Texture2D> depth, staging;
    ComPtr<ID3D10DepthStencilView> dsv;
    CHECK(device->CreateTexture2D(&td, nullptr, &depth));
    CHECK(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv));
    td.Usage = D3D10_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&td, nullptr, &staging));
    for (size_t n = 0; n < all.size(); n++) {
      const auto &c = all[n];
      auto geometry = raster_bias::geometry(c);
      step("%s %s bias case %zu", "D3D10", format.name.c_str(), n);
      D3D10_RASTERIZER_DESC rd{c.wire ? D3D10_FILL_WIREFRAME : D3D10_FILL_SOLID, D3D10_CULL_NONE};
      rd.DepthClipEnable = c.depth_clip;
      rd.DepthBias = c.bias;
      rd.DepthBiasClamp = c.clamp;
      rd.SlopeScaledDepthBias = c.scale;
      ComPtr<ID3D10RasterizerState> state;
      CHECK(device->CreateRasterizerState(&rd, &state));
      D3D10_RASTERIZER_DESC got;
      state->GetDesc(&got);
      expect(got.DepthBias == rd.DepthBias &&
                 (got.SlopeScaledDepthBias == rd.SlopeScaledDepthBias ||
                  (std::isnan(got.SlopeScaledDepthBias) && std::isnan(rd.SlopeScaledDepthBias))) &&
                 (got.DepthBiasClamp == rd.DepthBiasClamp ||
                  (std::isnan(got.DepthBiasClamp) && std::isnan(rd.DepthBiasClamp))),
             "depth-bias state description changed");
      device->RSSetState(c.default_state ? nullptr : state.Get());
      device->OMSetDepthStencilState(ds.Get(), 0);
      D3D10_VIEWPORT vp{0, 0, raster::width, raster::height, c.low, c.high};
      device->RSSetViewports(1, &vp);
      std::array<raster::Vertex, raster::Triangle> vertices{};
      std::copy(geometry.vertices.begin(), geometry.vertices.end(), vertices.begin());
      device->UpdateSubresource(vb.Get(), 0, nullptr, vertices.data(), 0, 0);
      const UINT stride = sizeof(raster::Vertex), offset = 0;
      device->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
      device->IASetInputLayout(layout.Get());
      device->IASetPrimitiveTopology(c.shape == raster::Point  ? D3D10_PRIMITIVE_TOPOLOGY_POINTLIST
                                     : c.shape == raster::Line ? D3D10_PRIMITIVE_TOPOLOGY_LINELIST
                                                               : D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      device->VSSetShader(vs.Get());
      device->PSSetShader(nullptr);
      device->OMSetRenderTargets(0, nullptr, dsv.Get());
      device->ClearDepthStencilView(dsv.Get(), D3D10_CLEAR_DEPTH, 1, 0);
      device->Draw(geometry.vertices.size(), 0);
      device->CopyResource(staging.Get(), depth.Get());
      D3D10_MAPPED_TEXTURE2D mapped;
      CHECK(staging->Map(0, D3D10_MAP_READ, 0, &mapped));
      raster_bias::compare(format, c, static_cast<const char *>(mapped.pData), mapped.RowPitch);
      staging->Unmap(0);
    }
  }
  return verdict();
}
