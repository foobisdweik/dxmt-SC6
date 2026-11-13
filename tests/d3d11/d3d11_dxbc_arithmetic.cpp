// contract: "Divide by zero returns 0xffffffff for both quotient and remainder." (D3D11.3 22.12.9).
// dxbc_arithmetic.hpp quotes the bitfield, product, carry and conversion contracts and derives the results.
// dynamic constant-buffer rows feed independent pixels, so every width/offset and half encoding is checked
// without one CPU/GPU round trip per input. both D3D11 context paths must produce the same bytes.
#include "d3d11_test.hpp"
#include "../dxbc_arithmetic.hpp"

int
main()
{
  using namespace dxbc_arithmetic;
  constexpr UINT rows = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
  const UINT side = UINT(std::sqrt(rows));
  if (!expect(side * side == rows, "constant-buffer rows form the test's square"))
    return verdict();
  std::vector<std::string> defines{"ROWS=" + std::to_string(rows), "SIDE=" + std::to_string(side)};
  auto vertex_code = compile(hlsl, "vs", "vs", defines);
  if (!expect(!!vertex_code, "arithmetic vertex shader compiled"))
    return verdict();
  struct Shader {
    const char *profile;
    Subject subject;
    std::string code;
  };
  std::vector<Shader> shaders;
  for (auto profile : {"ps_4_0", "ps_4_1", "ps_5_0"}) {
    auto seed = compile(hlsl, "ps", profile, defines);
    if (!expect(!!seed, "%s arithmetic fixture compiled", profile))
      return verdict();
    std::string base(static_cast<const char *>(seed->GetBufferPointer()), seed->GetBufferSize());
    for (unsigned subject = 0; subject < std::size(names); subject++) {
      if (strcmp(profile, "ps_5_0") && subject != products && subject != division && subject != float_specials)
        continue;
      auto code = program(base, Subject(subject), side);
      if (!expect(!code.empty(), "%s %s instructions generated", profile, names[subject]))
        return verdict();
      shaders.push_back({profile, Subject(subject), std::move(code)});
    }
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
      side, side, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
  D3D11_BUFFER_DESC cb_desc{rows * sizeof(Value), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
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
  const D3D11_VIEWPORT viewport{0, 0, float(side), float(side), 0, 1};
  const Value padding{0, 0, 0, UINT32_MAX};
  uint64_t checked = 0;
  for (auto &shader : shaders) {
    step("create %s %s", shader.profile, names[shader.subject]);
    ComPtr<ID3D11PixelShader> ps;
    CHECK(device->CreatePixelShader(shader.code.data(), shader.code.size(), nullptr, &ps));
    auto all = inputs(shader.subject);
    unsigned wrong = 0;
    for (size_t start = 0; start < all.size(); start += rows) {
      std::vector<Value> batch(rows, padding);
      std::copy_n(all.begin() + start, std::min<size_t>(rows, all.size() - start), batch.begin());
      for (auto *context : {immediate.Get(), deferred.Get()}) {
        step("%s %s inputs %zu..%zu, %s", shader.profile, names[shader.subject], start,
             std::min(start + rows, all.size()), context == immediate.Get() ? "immediate" : "deferred");
        context->UpdateSubresource(constants.Get(), 0, nullptr, batch.data(), 0, 0);
        const float clear[4] = {};
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
        for (UINT i = 0; i < rows; i++) {
          auto got = reinterpret_cast<const uint32_t *>(static_cast<const char *>(mapped.pData) +
                                                        (i / side) * mapped.RowPitch) +
                     (i % side) * std::tuple_size_v<Value>;
          auto want = reference(shader.subject, batch[i]);
          for (UINT c = 0; c < want.size(); c++, checked++)
            if (!allowed(shader.subject, c, got[c], want[c], batch[i]) && wrong++ < want.size())
              expect(false, "input %zu (%#x,%#x,%#x,%#x), component %u: %#x, want %#x", start + i, batch[i][0],
                     batch[i][1], batch[i][2], batch[i][3], c, got[c], want[c]);
        }
        immediate->Unmap(staging.Get(), 0);
      }
    }
    expect(wrong == 0, "%u incorrect %s results", wrong, names[shader.subject]);
  }
  printf("%llu arithmetic components checked\n", static_cast<unsigned long long>(checked));
  return verdict();
}
