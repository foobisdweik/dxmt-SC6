// contract: D3D11.3 17.9: "When DepthEnable is false, depth test and depth
// buffer write are not performed, regardless of any other settings." 17.11: "If
// no Depth Buffer is bound, the depth test always passes." 17.10: "z =
// min(Viewport.MaxDepth,max(Viewport.MinDepth,z))". 17.13 ANDs write enable
// with the DSV read-only flag. source: D3D11_3_FunctionalSpec.htm. Microsoft
// Learn D3D11_STENCIL_OP defines clamp and wrap separately:
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_stencil_op
// initial bytes and expected color/depth/stencil come from om_depth.hpp,
// independently of the state implementation. every stored stencil byte is
// exercised against every comparison/reference, and all read/write masks are
// used.
// shader stencil reference replaces the API reference in comparison and
// REPLACE. all byte references, comparisons and read/write masks are checked
// when reported.
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/shader-specified-stencil-reference-value
#include "d3d11_test.hpp"
#include <d3d11_3.h>
#include "../om_depth.hpp"

int main() {
  const UINT width = om::depth_width, faces = 2;
  const char *entries[] = {"ps", "ps_depth", "ps_le", "ps_ge", "ps_stencil"};
  auto vertex = compile(om::depth_hlsl, "vs", "vs");
  ComPtr<ID3DBlob> pixels[std::size(entries)];
  for (UINT i = 0; i < std::size(entries); i++) {
    pixels[i] = compile(
        om::depth_hlsl, entries[i], "ps",
        {"CONSERVATIVE=1", "STENCIL_REFERENCE=1",
         "STENCIL_MASK=" + std::to_string(D3D11_DEFAULT_STENCIL_READ_MASK)});
    if (!expect(bool(pixels[i]), "depth HLSL %s did not compile", entries[i]))
      return verdict();
  }
  if (!expect(bool(vertex), "vertex HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level,
                          1, D3D11_SDK_VERSION, &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  D3D11_FEATURE_DATA_D3D11_OPTIONS2 options{};
  HRESULT options_hr = device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2,
                                                   &options, sizeof(options));
  bool stencil_reference =
      SUCCEEDED(options_hr) && options.PSSpecifiedStencilRefSupported;
  if (!stencil_reference)
    printf("unsupported: pixel-shader stencil reference (query %08lx)\n",
           options_hr);
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps[std::size(entries)];
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(),
                                   vertex->GetBufferSize(), nullptr, &vs));
  for (UINT i = 0; i < std::size(entries); i++)
    if (i != 4 || stencil_reference)
      CHECK(device->CreatePixelShader(pixels[i]->GetBufferPointer(),
                                      pixels[i]->GetBufferSize(), nullptr,
                                      &ps[i]));
  struct Params {
    float depth;
    UINT reverse;
    UINT stencil_reference;
    float padding;
  } params{};
  D3D11_BUFFER_DESC cb_desc{sizeof(params), D3D11_USAGE_DEFAULT,
                            D3D11_BIND_CONSTANT_BUFFER};
  ComPtr<ID3D11Buffer> cb;
  CHECK(device->CreateBuffer(&cb_desc, nullptr, &cb));
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster_desc.DepthClipEnable = raster_desc.ScissorEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));
  for (auto format : om::depth_formats) {
    const auto cases =
        om::depth_cases(stencil_reference && format.stencil_bits);
    const UINT height = UINT(cases.size()) * faces;
    if (!expect(height <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION,
                "case surface exceeds the API's texture limit"))
      return verdict();
    UINT support;
    CHECK(device->CheckFormatSupport(format.value, &support));
    if (!expect(support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL,
                "depth format %u not supported", format.value))
      return verdict();
    auto initial = om::depth_seed(format, height);
    for (UINT path = 0; path < 2; path++) {
      step("depth format %u, path %u: initial bytes", format.value, path);
      immediate->ClearState();
      deferred->ClearState();
      auto context = path == 1 ? deferred.Get() : immediate.Get();
      D3D11_TEXTURE2D_DESC desc{width,
                                height,
                                1,
                                1,
                                format.value,
                                {1, 0},
                                D3D11_USAGE_DEFAULT,
                                D3D11_BIND_DEPTH_STENCIL};
      D3D11_SUBRESOURCE_DATA data{initial.data(), width * format.stride};
      ComPtr<ID3D11Texture2D> depth, depth_read, color, color_read;
      CHECK(device->CreateTexture2D(&desc, &data, &depth));
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&desc, nullptr, &depth_read));
      desc.Format = DXGI_FORMAT_R32_UINT;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET;
      desc.CPUAccessFlags = 0;
      std::vector<UINT> zero(width * height);
      data = {zero.data(), width * sizeof(UINT)};
      CHECK(device->CreateTexture2D(&desc, &data, &color));
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&desc, nullptr, &color_read));
      ComPtr<ID3D11RenderTargetView> rtv;
      CHECK(device->CreateRenderTargetView(color.Get(), nullptr, &rtv));
      ComPtr<ID3D11DepthStencilView> dsv[4];
      for (UINT flags = 0; flags < std::size(dsv); flags++) {
        if (flags & D3D11_DSV_READ_ONLY_STENCIL && !format.stencil_bits)
          continue;
        D3D11_DEPTH_STENCIL_VIEW_DESC view{
            format.value, D3D11_DSV_DIMENSION_TEXTURE2D, flags};
        CHECK(device->CreateDepthStencilView(depth.Get(), &view, &dsv[flags]));
      }
      context->RSSetState(raster.Get());
      context->VSSetShader(vs.Get(), nullptr, 0);
      context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
      context->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      for (UINT i = 0; i < cases.size(); i++) {
        const auto &k = cases[i];
        if ((!format.stencil_bits && (k.flags & D3D11_DSV_READ_ONLY_STENCIL)))
          continue;
        ComPtr<ID3D11DepthStencilState> state;
        CHECK(device->CreateDepthStencilState(&k.state, &state));
        context->OMSetDepthStencilState(
            state.Get(), k.shader == 4
                             ? k.reference ^ D3D11_DEFAULT_STENCIL_READ_MASK
                             : k.reference);
        context->PSSetShader(ps[k.shader].Get(), nullptr, 0);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(),
                                    k.absent ? nullptr : dsv[k.flags].Get());
        for (UINT face = 0; face < faces; face++) {
          step("depth format %u, path %u, case %u, face %u", format.value, path,
               i, face);
          params.depth = k.depth;
          params.reverse = face;
          params.stencil_reference = k.reference;
          context->UpdateSubresource(cb.Get(), 0, nullptr, &params, 0, 0);
          UINT y = i * faces + face;
          D3D11_VIEWPORT viewport{0, float(y), float(width), 1, k.min, k.max};
          D3D11_RECT scissor{1, LONG(y), LONG(width - 1), LONG(y + 1)};
          context->RSSetViewports(1, &viewport);
          context->RSSetScissorRects(1, &scissor);
          context->Draw(3, 0);
        }
      }
      context->OMSetRenderTargets(0, nullptr, nullptr);
      if (path == 1) {
        ComPtr<ID3D11CommandList> commands;
        CHECK(deferred->FinishCommandList(FALSE, &commands));
        immediate->ExecuteCommandList(commands.Get(), FALSE);
      }
      immediate->CopyResource(color_read.Get(), color.Get());
      immediate->CopyResource(depth_read.Get(), depth.Get());
      D3D11_MAPPED_SUBRESOURCE pixels{}, values{};
      CHECK(immediate->Map(color_read.Get(), 0, D3D11_MAP_READ, 0, &pixels));
      CHECK(immediate->Map(depth_read.Get(), 0, D3D11_MAP_READ, 0, &values));
      for (UINT i = 0; i < cases.size(); i++)
        for (UINT face = 0; face < faces; face++) {
          const auto &k = cases[i];
          step("depth format %u, path %u, case %u, face %u: readback",
               format.value, path, i, face);
          UINT y = i * faces + face;
          bool active = !(!format.stencil_bits &&
                          (k.flags & D3D11_DSV_READ_ONLY_STENCIL));
          for (UINT x = 0; x < width; x++) {
            om::DepthResult want{om::depth_word(format, om::initial_depth(x)),
                                 0, UINT8(x - 1)};
            if (active && x && x + 1 < width)
              want = om::depth_result(format, k, x, face);
            auto p = static_cast<const unsigned char *>(values.pData) +
                     y * values.RowPitch + x * format.stride;
            UINT depth_word = 0, color_word;
            memcpy(&depth_word, p, format.depth_bits / CHAR_BIT);
            memcpy(&color_word,
                   static_cast<const char *>(pixels.pData) +
                       y * pixels.RowPitch + x * sizeof(UINT),
                   sizeof(UINT));
            if (!expect(depth_word == want.depth && color_word == want.color &&
                            (!format.stencil_bits ||
                             p[format.depth_bits / CHAR_BIT] == want.stencil),
                        "column %u: depth %#x/%#x, color %u/%u, stencil %u/%u",
                        x, depth_word, want.depth, color_word, want.color,
                        format.stencil_bits ? p[format.depth_bits / CHAR_BIT]
                                            : 0,
                        want.stencil))
              break;
          }
        }
      immediate->Unmap(depth_read.Get(), 0);
      immediate->Unmap(color_read.Get(), 0);
    }
  }
  return verdict();
}
