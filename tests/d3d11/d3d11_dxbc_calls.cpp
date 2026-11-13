// contract: "Subroutines can nest 32 deep." Conditional call and return test any set bit (D3D11.3 22.7.10-11,
// 22.7.17; quoted and evaluated on the CPU in dxbc_calls.hpp). SM4.0/4.1/5.0 run through both D3D11 contexts.
#include "d3d11_test.hpp"
#include "../dxbc_calls.hpp"

int
main()
{
  auto vertex_code = compile(dxbc_calls::hlsl, "vs", "vs");
  if (!expect(!!vertex_code, "call witness vertex shader compiled"))
    return verdict();
  struct Profile {
    const char *name;
    std::vector<dxbc_calls::Case> cases;
  };
  std::vector<Profile> profiles;
  for (auto name : {"ps_4_0", "ps_4_1", "ps_5_0"}) {
    auto seed = compile(dxbc_calls::hlsl, "ps", name);
    if (!expect(!!seed, "%s call fixture compiled", name))
      return verdict();
    std::string code(static_cast<const char *>(seed->GetBufferPointer()), seed->GetBufferSize());
    auto cases = dxbc_calls::cases(code);
    if (!expect(!cases.empty(), "%s conditional call programs generated", name))
      return verdict();
    profiles.push_back({name, std::move(cases)});
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  CHECK(device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), nullptr, &vs));
  D3D11_TEXTURE2D_DESC desc{
      1, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
  D3D11_BUFFER_DESC cb_desc{sizeof(dxbc_calls::Value), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  ComPtr<ID3D11Buffer> constants;
  CHECK(device->CreateBuffer(&cb_desc, nullptr, &constants));
  D3D11_RASTERIZER_DESC raster{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> rasterizer;
  CHECK(device->CreateRasterizerState(&raster, &rasterizer));
  D3D11_DEPTH_STENCIL_DESC depth{};
  depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
  ComPtr<ID3D11DepthStencilState> depth_state;
  CHECK(device->CreateDepthStencilState(&depth, &depth_state));
  const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
  const float clear[4] = {};
  for (auto &profile : profiles) {
    std::string previous;
    ComPtr<ID3D11PixelShader> ps;
    for (auto &c : profile.cases) {
      step("create %s %s", profile.name, c.name.c_str());
      if (c.code != previous) {
        ps.Reset();
        CHECK(device->CreatePixelShader(c.code.data(), c.code.size(), nullptr, &ps));
        previous = c.code;
      }
      for (auto *context : {immediate.Get(), deferred.Get()}) {
        step("%s %s condition %#x, %s", profile.name, c.name.c_str(), c.input[0],
             context == immediate.Get() ? "immediate" : "deferred");
        context->UpdateSubresource(constants.Get(), 0, nullptr, c.input.data(), 0, 0);
        context->ClearRenderTargetView(rtv.Get(), clear);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        context->OMSetDepthStencilState(depth_state.Get(), 0);
        context->RSSetState(rasterizer.Get());
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->Draw(3, 0);
        context->ClearState();
        if (context == deferred.Get()) {
          ComPtr<ID3D11CommandList> commands;
          CHECK(deferred->FinishCommandList(FALSE, &commands));
          immediate->ExecuteCommandList(commands.Get(), FALSE);
        }
        immediate->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped;
        CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        dxbc_calls::check(static_cast<const uint32_t *>(mapped.pData), c);
        immediate->Unmap(staging.Get(), 0);
      }
    }
  }
  return verdict();
}
