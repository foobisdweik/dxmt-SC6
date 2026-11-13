// contract: D3D11.3 15.8.1: "Values outside [0..n-1] are treated as 0 for indexing viewports."
// the index comes from the leading vertex and reaches the pixel shader unchanged. ../viewports.hpp checks each
// viewport/scissor slot, conflicting per-vertex indices and state replacement through the Direct3D10 interfaces.
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include "../viewports.hpp"

int main() {
  using namespace viewports;
  static_assert(max_viewports == D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
  step("compile Direct3D10 viewport shaders before device creation");
  const std::vector<std::string> defines = {"MAX_VIEWPORTS=" + std::to_string(max_viewports)};
  auto vs_code = compile(hlsl, "vs", "vs_4_0", defines), gs_code = compile(hlsl, "gs", "gs_4_0", defines),
       ps_code = compile(hlsl, "ps", "ps_4_0", defines);
  auto plain_code = compile(hlsl, "ps_plain", "ps_4_0", defines);
  if (!vs_code || !gs_code || !ps_code || !plain_code)
    return 1;
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  for (auto level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, level, D3D10_1_SDK_VERSION, &device));
    ComPtr<ID3D10VertexShader> vs;
    ComPtr<ID3D10GeometryShader> gs;
    ComPtr<ID3D10PixelShader> ps, plain_ps;
    CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), &vs));
    CHECK(device->CreateGeometryShader(gs_code->GetBufferPointer(), gs_code->GetBufferSize(), &gs));
    CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), &ps));
    CHECK(device->CreatePixelShader(plain_code->GetBufferPointer(), plain_code->GetBufferSize(), &plain_ps));
    D3D10_TEXTURE2D_DESC td{
        width, height, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET};
    ComPtr<ID3D10Texture2D> target, staging;
    ComPtr<ID3D10RenderTargetView> rtv;
    CHECK(device->CreateTexture2D(&td, nullptr, &target));
    CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    td.Usage = D3D10_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&td, nullptr, &staging));
    struct Constants {
      UINT index, value, conflicting, padding;
    };
    D3D10_BUFFER_DESC bd{sizeof(Constants), D3D10_USAGE_DEFAULT, D3D10_BIND_CONSTANT_BUFFER};
    ComPtr<ID3D10Buffer> constants;
    CHECK(device->CreateBuffer(&bd, nullptr, &constants));
    ComPtr<ID3D10RasterizerState> states[2];
    for (BOOL scissor : {FALSE, TRUE}) {
      D3D10_RASTERIZER_DESC desc{D3D10_FILL_SOLID, D3D10_CULL_NONE};
      desc.DepthClipEnable = TRUE;
      desc.ScissorEnable = scissor;
      CHECK(device->CreateRasterizerState(&desc, &states[scissor]));
    }
    device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    device->VSSetShader(vs.Get());
    device->PSSetShader(ps.Get());
    device->GSSetConstantBuffers(0, 1, constants.GetAddressOf());
    device->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    device->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    const auto all = cases();
    for (bool scissor : {false, true})
      for (size_t c = 0; c < all.size(); c++) {
        step("Direct3D10 level %#x viewport case %zu scissor %u", level, c, scissor);
        const float clear[4] = {};
        device->ClearRenderTargetView(rtv.Get(), clear);
        device->RSSetState(states[scissor].Get());
        for (size_t n = 0; n < all[c].size(); n++) {
          const auto &draw = all[c][n];
          if (draw.viewports) {
            std::vector<D3D10_VIEWPORT> set;
            for (auto part : *draw.viewports) {
              auto r = rectangle(part);
              set.push_back({r.left, r.top, UINT(r.right - r.left), UINT(r.bottom - r.top), 0, 1});
            }
            device->RSSetViewports(set.size(), set.data());
          }
          if (draw.scissors) {
            std::vector<D3D10_RECT> set;
            for (auto part : *draw.scissors) {
              auto r = rectangle(part);
              set.push_back({r.left, r.top, r.right, r.bottom});
            }
            device->RSSetScissorRects(set.size(), set.data());
          }
          Constants of{draw.index == plain ? 0 : draw.index, UINT(n + 1), draw.conflicting};
          device->UpdateSubresource(constants.Get(), 0, nullptr, &of, 0, 0);
          device->GSSetShader(draw.index == plain ? nullptr : gs.Get());
          device->PSSetShader(draw.index == plain ? plain_ps.Get() : ps.Get());
          device->Draw(3, 0);
        }
        device->CopyResource(staging.Get(), target.Get());
        D3D10_MAPPED_TEXTURE2D mapped;
        CHECK(staging->Map(0, D3D10_MAP_READ, 0, &mapped));
        check("Direct3D10 ", c, expected(all[c], scissor), static_cast<const char *>(mapped.pData), mapped.RowPitch);
        staging->Unmap(0);
      }
  }
  return verdict();
}
