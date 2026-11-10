// contract: "Divide by zero returns 0xffffffff for both quotient and remainder." (D3D11.3 22.12.9).
// full signed/unsigned product halves and floating special-value rules also apply to SM4.0/4.1. the CPU oracle
// in dxbc_arithmetic.hpp checks their bytes through the actual D3D10 device methods.
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include "../dxbc_arithmetic.hpp"

int
main()
{
  using namespace dxbc_arithmetic;
  constexpr UINT rows = D3D10_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
  const UINT side = UINT(std::sqrt(rows));
  if (!expect(side * side == rows, "constant-buffer rows form the test's square"))
    return verdict();
  const std::vector<std::string> defines{"ROWS=" + std::to_string(rows), "SIDE=" + std::to_string(side)};
  auto vertex_code = compile(hlsl, "vs", "vs_4_0", defines);
  if (!expect(!!vertex_code, "SM4 vertex shader compiled"))
    return verdict();
  struct Shader {
    const char *profile;
    Subject subject;
    std::string code;
  };
  std::vector<Shader> shaders;
  for (auto profile : {"ps_4_0", "ps_4_1"}) {
    auto seed = compile(hlsl, "ps", profile, defines);
    if (!expect(!!seed, "%s arithmetic fixture compiled", profile))
      return verdict();
    const std::string base(static_cast<const char *>(seed->GetBufferPointer()), seed->GetBufferSize());
    for (Subject subject : {products, division, float_specials}) {
      auto code = program(base, subject, side);
      if (!expect(!code.empty(), "%s %s instructions generated", profile, names[subject]))
        return verdict();
      shaders.push_back({profile, subject, std::move(code)});
    }
  }
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      reinterpret_cast<void *>(GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")));
  if (!expect(create != nullptr, "D3D10CreateDevice1 available"))
    return verdict();
  ComPtr<ID3D10Device1> device;
  CHECK(
      create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &device));
  ComPtr<ID3D10VertexShader> vs;
  CHECK(device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), &vs));
  D3D10_TEXTURE2D_DESC desc{
      side, side, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET};
  ComPtr<ID3D10Texture2D> target, staging;
  ComPtr<ID3D10RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  desc.Usage = D3D10_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
  D3D10_BUFFER_DESC cb_desc{rows * sizeof(Value), D3D10_USAGE_DEFAULT, D3D10_BIND_CONSTANT_BUFFER};
  ComPtr<ID3D10Buffer> constants;
  CHECK(device->CreateBuffer(&cb_desc, nullptr, &constants));
  D3D10_RASTERIZER_DESC raster{D3D10_FILL_SOLID, D3D10_CULL_NONE};
  raster.DepthClipEnable = TRUE;
  ComPtr<ID3D10RasterizerState> rasterizer;
  CHECK(device->CreateRasterizerState(&raster, &rasterizer));
  D3D10_DEPTH_STENCIL_DESC depth{};
  depth.DepthFunc = D3D10_COMPARISON_ALWAYS;
  ComPtr<ID3D10DepthStencilState> depth_state;
  CHECK(device->CreateDepthStencilState(&depth, &depth_state));
  const D3D10_VIEWPORT viewport{0, 0, side, side, 0, 1};
  const Value padding{0, 0, 0, UINT32_MAX};
  uint64_t checked = 0;
  for (auto &shader : shaders) {
    step("D3D10 create %s %s", shader.profile, names[shader.subject]);
    ComPtr<ID3D10PixelShader> ps;
    CHECK(device->CreatePixelShader(shader.code.data(), shader.code.size(), &ps));
    auto all = inputs(shader.subject);
    unsigned wrong = 0;
    for (size_t start = 0; start < all.size(); start += rows) {
      std::vector<Value> batch(rows, padding);
      std::copy_n(all.begin() + start, std::min<size_t>(rows, all.size() - start), batch.begin());
      step("D3D10 %s %s inputs %zu..%zu", shader.profile, names[shader.subject], start,
           std::min(start + rows, all.size()));
      device->UpdateSubresource(constants.Get(), 0, nullptr, batch.data(), 0, 0);
      const float clear[4] = {};
      device->ClearRenderTargetView(rtv.Get(), clear);
      device->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      device->OMSetDepthStencilState(depth_state.Get(), 0);
      device->RSSetState(rasterizer.Get());
      device->RSSetViewports(1, &viewport);
      device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      device->VSSetShader(vs.Get());
      device->PSSetShader(ps.Get());
      device->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
      device->Draw(3, 0);
      device->ClearState();
      device->CopyResource(staging.Get(), target.Get());
      D3D10_MAPPED_TEXTURE2D mapped;
      CHECK(staging->Map(0, D3D10_MAP_READ, 0, &mapped));
      for (UINT i = 0; i < rows; i++) {
        const auto got =
            reinterpret_cast<const uint32_t *>(static_cast<const char *>(mapped.pData) + (i / side) * mapped.RowPitch) +
            (i % side) * std::tuple_size_v<Value>;
        auto want = reference(shader.subject, batch[i]);
        for (UINT c = 0; c < want.size(); c++, checked++)
          if (!allowed(shader.subject, c, got[c], want[c], batch[i]) && wrong++ < want.size())
            expect(false, "input %zu (%#x,%#x), component %u: %#x, want %#x", start + i, batch[i][0], batch[i][1], c,
                   got[c], want[c]);
      }
      staging->Unmap(0);
    }
    expect(wrong == 0, "%u incorrect %s results", wrong, names[shader.subject]);
  }
  printf("%llu D3D10 arithmetic components checked\n", static_cast<unsigned long long>(checked));
  return verdict();
}
