// contract: D3D11.3 17.1-17.6 defines separate RGB/alpha blend factors and
// equations; MIN/MAX ignore factors. "Fixed-function blend can be enabled and
// configured independently for each RenderTarget." (17.3). "For all formats,
// there is a clamp to the RenderTarget range after blend, before writing values
// out to memory." (17.5). "The masks simply limit writes." (17.15). section 3
// supplies storage conversions, including linear blending of sRGB data (5.6.6).
// source: D3D11_3_FunctionalSpec.htm. om_blend.hpp computes the expected
// pixels. exact destination bytes are uploaded, so the clear path cannot hide a
// blend error. both guard columns survive.
#include "d3d11_test.hpp"
#include "../om_blend.hpp"

int main() {
  const auto cases = om::blend_cases();
  const UINT targets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
             width = UINT(cases.size()) + 2;
  struct Shaders {
    ComPtr<ID3DBlob> vs, ps, dual, special;
  } code[1];
  const char *profiles[] = {"5_0"};
  for (UINT i = 0; i < std::size(code); i++) {
    step("compile blend shaders, profile %s", profiles[i]);
    auto defines =
        std::vector<std::string>{"TARGETS=" + std::to_string(targets)};
    code[i].vs = compile(om::blend_hlsl, "vs",
                         (std::string("vs_") + profiles[i]).c_str(), defines);
    code[i].ps = compile(om::blend_hlsl, "ps",
                         (std::string("ps_") + profiles[i]).c_str(), defines);
    code[i].dual = compile(om::blend_hlsl, "ps_dual",
                           (std::string("ps_") + profiles[i]).c_str(), defines);
    code[i].special =
        compile(om::blend_hlsl, "ps_special",
                (std::string("ps_") + profiles[i]).c_str(), defines);
    if (!expect(code[i].vs && code[i].ps && code[i].dual && code[i].special,
                "blend HLSL did not compile"))
      return verdict();
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level,
                          1, D3D11_SDK_VERSION, &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs[std::size(code)];
  ComPtr<ID3D11PixelShader> ps[std::size(code)][3];
  for (UINT i = 0; i < std::size(code); i++) {
    CHECK(device->CreateVertexShader(code[i].vs->GetBufferPointer(),
                                     code[i].vs->GetBufferSize(), nullptr,
                                     &vs[i]));
    ID3DBlob *pixels[] = {code[i].ps.Get(), code[i].dual.Get(),
                          code[i].special.Get()};
    for (UINT p = 0; p < std::size(pixels); p++)
      CHECK(device->CreatePixelShader(pixels[p]->GetBufferPointer(),
                                      pixels[p]->GetBufferSize(), nullptr,
                                      &ps[i][p]));
  }
  D3D11_BUFFER_DESC constants_desc{sizeof(om::Color) * 2, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_CONSTANT_BUFFER};
  ComPtr<ID3D11Buffer> constants;
  CHECK(device->CreateBuffer(&constants_desc, nullptr, &constants));
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster_desc.DepthClipEnable = raster_desc.ScissorEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));

  for (auto format : om::formats) {
    UINT support = 0;
    CHECK(device->CheckFormatSupport(format.value, &support));
    if (!expect(format.value != DXGI_FORMAT_R8G8B8A8_UNORM ||
                    (support & D3D11_FORMAT_SUPPORT_BLENDABLE),
                "mandatory UNORM blending not supported"))
      return verdict();
    if (!(support & D3D11_FORMAT_SUPPORT_BLENDABLE)) {
      printf("unsupported blend format %u\n", format.value);
      continue;
    }
    const UINT stride =
        sizeof(om::Color) * format.bits / (sizeof(float) * CHAR_BIT);
    std::vector<unsigned char> initial(width * stride);
    for (UINT x = 0; x < width; x++)
      om::seed(format, cases[x && x <= cases.size() ? x - 1 : 0].destination,
               initial.data() + x * stride);
    for (UINT path = 0; path < 2; path++) {
      step("format %u, path %u: upload destination bytes", format.value, path);
      immediate->ClearState();
      deferred->ClearState();
      auto context = path == 1 ? deferred.Get() : immediate.Get();
      UINT profile = 0;
      ComPtr<ID3D11Texture2D> textures[targets], staging;
      ComPtr<ID3D11RenderTargetView> views[targets];
      ID3D11RenderTargetView *bound[targets];
      D3D11_TEXTURE2D_DESC texture_desc{width,
                                        1,
                                        1,
                                        1,
                                        format.value,
                                        {1, 0},
                                        D3D11_USAGE_DEFAULT,
                                        D3D11_BIND_RENDER_TARGET};
      D3D11_SUBRESOURCE_DATA data{initial.data(), width * stride};
      for (UINT slot = 0; slot < targets; slot++) {
        CHECK(device->CreateTexture2D(&texture_desc, &data, &textures[slot]));
        CHECK(device->CreateRenderTargetView(textures[slot].Get(), nullptr,
                                             &views[slot]));
        bound[slot] = views[slot].Get();
      }
      texture_desc.Usage = D3D11_USAGE_STAGING;
      texture_desc.BindFlags = 0;
      texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&texture_desc, nullptr, &staging));
      context->RSSetState(raster.Get());
      const D3D11_VIEWPORT viewport{0, 0, float(width), 1, 0, 1};
      context->RSSetViewports(1, &viewport);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context->VSSetShader(vs[profile].Get(), nullptr, 0);
      context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
      for (UINT i = 0; i < cases.size(); i++) {
        const auto &k = cases[i];
        if ((k.special && (format.normalized || format.bits != 32)))
          continue;
        step("format %u, path %u, equation %u, dual %u", format.value, path, i,
             k.dual);
        const om::Color params[] = {k.source, k.secondary};
        context->UpdateSubresource(constants.Get(), 0, nullptr, params, 0, 0);
        D3D11_RECT scissor{LONG(i + 1), 0, LONG(i + 2), 1};
        context->RSSetScissorRects(1, &scissor);
        context->OMSetRenderTargets(k.dual ? 1 : targets, bound, nullptr);
        ComPtr<ID3D11BlendState> blend;
        CHECK(device->CreateBlendState(&k.state, &blend));
        context->OMSetBlendState(
            blend.Get(), k.null_factor ? nullptr : k.factor.data(), ~0u);
        // a draw without a pixel shader writes no target first, so the
        // pipeline of the draw after it changes by its pixel shader alone
        context->PSSetShader(nullptr, nullptr, 0);
        context->Draw(3, 0);
        context->PSSetShader(ps[profile][k.dual      ? 1
                                         : k.special ? 2
                                                     : 0]
                                 .Get(),
                             nullptr, 0);
        context->Draw(3, 0);
      }
      context->OMSetRenderTargets(0, nullptr, nullptr);
      if (path == 1) {
        ComPtr<ID3D11CommandList> commands;
        CHECK(deferred->FinishCommandList(FALSE, &commands));
        immediate->ExecuteCommandList(commands.Get(), FALSE);
      }
      for (UINT slot = 0; slot < targets; slot++) {
        step("format %u, path %u, slot %u: read pixels", format.value, path,
             slot);
        immediate->CopyResource(staging.Get(), textures[slot].Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        if (!expect(mapped.pData && mapped.RowPitch >= width * stride,
                    "invalid mapped row"))
          return verdict();
        auto got = static_cast<const unsigned char *>(mapped.pData);
        for (UINT x = 0; x < width; x++) {
          bool written = x && x <= cases.size();
          if (written) {
            const auto &k = cases[x - 1];
            written =
                !(k.special && (format.normalized || format.bits != 32)) &&
                (!k.dual || !slot);
          }
          if (written) {
            step("format %u, path %u, slot %u, equation %u", format.value, path,
                 slot, x - 1);
            om::compare(format, cases[x - 1], slot, initial.data() + x * stride,
                        got + x * stride);
          } else
            expect(
                !memcmp(got + x * stride, initial.data() + x * stride, stride),
                "untouched column %u changed", x);
        }
        immediate->Unmap(staging.Get(), 0);
      }
    }
  }
  return verdict();
}
