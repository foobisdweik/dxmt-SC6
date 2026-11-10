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
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include "../om_depth.hpp"

int main() {
  const auto cases = om::depth_cases();
  const UINT width = om::depth_width, faces = 2,
             height = UINT(cases.size()) * faces;
  struct Code {
    ComPtr<ID3DBlob> vs, ps[2];
  } code[2];
  const char *profiles[] = {"4_0", "4_1"};
  const char *entries[] = {"ps", "ps_depth"};
  for (UINT profile = 0; profile < std::size(code); profile++) {
    code[profile].vs = compile(
        om::depth_hlsl, "vs", (std::string("vs_") + profiles[profile]).c_str());
    for (UINT i = 0; i < std::size(entries); i++)
      code[profile].ps[i] =
          compile(om::depth_hlsl, entries[i],
                  (std::string("ps_") + profiles[profile]).c_str());
    if (!expect(code[profile].vs && code[profile].ps[0] && code[profile].ps[1],
                "depth HLSL did not compile"))
      return verdict();
  }
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>((
      void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create, "D3D10CreateDevice1 unavailable"))
    return verdict();
  const D3D10_FEATURE_LEVEL1 levels[] = {D3D10_FEATURE_LEVEL_10_0,
                                         D3D10_FEATURE_LEVEL_10_1};
  for (UINT profile = 0; profile < std::size(code); profile++) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
                 levels[profile], D3D10_1_SDK_VERSION, &device));
    if (!expect(height <= D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION,
                "case surface exceeds the API's texture limit"))
      return verdict();
    ComPtr<ID3D10VertexShader> vs;
    ComPtr<ID3D10PixelShader> ps[std::size(entries)];
    CHECK(device->CreateVertexShader(code[profile].vs->GetBufferPointer(),
                                     code[profile].vs->GetBufferSize(), &vs));
    for (UINT i = 0; i < std::size(entries); i++)
      CHECK(device->CreatePixelShader(code[profile].ps[i]->GetBufferPointer(),
                                      code[profile].ps[i]->GetBufferSize(),
                                      &ps[i]));
    struct Params {
      float depth;
      UINT reverse;
      float padding[2];
    } params{};
    D3D10_BUFFER_DESC cb_desc{sizeof(params), D3D10_USAGE_DEFAULT,
                              D3D10_BIND_CONSTANT_BUFFER};
    ComPtr<ID3D10Buffer> cb;
    CHECK(device->CreateBuffer(&cb_desc, nullptr, &cb));
    D3D10_RASTERIZER_DESC raster_desc{D3D10_FILL_SOLID, D3D10_CULL_NONE};
    raster_desc.DepthClipEnable = raster_desc.ScissorEnable = TRUE;
    ComPtr<ID3D10RasterizerState> raster;
    CHECK(device->CreateRasterizerState(&raster_desc, &raster));
    for (auto format : om::depth_formats) {
      UINT support;
      CHECK(device->CheckFormatSupport(format.value, &support));
      if (!expect(support & D3D10_FORMAT_SUPPORT_DEPTH_STENCIL,
                  "depth format %u not supported", format.value))
        return verdict();
      auto initial = om::depth_seed(format, height);
      {
        const UINT path = profile;
        step("depth format %u, path %u: initial bytes", format.value, path);
        device->ClearState();
        auto context = device.Get();
        D3D10_TEXTURE2D_DESC desc{width,
                                  height,
                                  1,
                                  1,
                                  format.value,
                                  {1, 0},
                                  D3D10_USAGE_DEFAULT,
                                  D3D10_BIND_DEPTH_STENCIL};
        D3D10_SUBRESOURCE_DATA data{initial.data(), width * format.stride};
        ComPtr<ID3D10Texture2D> depth, depth_read, color, color_read;
        CHECK(device->CreateTexture2D(&desc, &data, &depth));
        desc.Usage = D3D10_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        CHECK(device->CreateTexture2D(&desc, nullptr, &depth_read));
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.Usage = D3D10_USAGE_DEFAULT;
        desc.BindFlags = D3D10_BIND_RENDER_TARGET;
        desc.CPUAccessFlags = 0;
        std::vector<UINT> zero(width * height);
        data = {zero.data(), width * sizeof(UINT)};
        CHECK(device->CreateTexture2D(&desc, &data, &color));
        desc.Usage = D3D10_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        CHECK(device->CreateTexture2D(&desc, nullptr, &color_read));
        ComPtr<ID3D10RenderTargetView> rtv;
        CHECK(device->CreateRenderTargetView(color.Get(), nullptr, &rtv));
        ComPtr<ID3D10DepthStencilView> dsv[1];
        for (UINT flags = 0; flags < std::size(dsv); flags++) {
          D3D10_DEPTH_STENCIL_VIEW_DESC view{format.value,
                                             D3D10_DSV_DIMENSION_TEXTURE2D};
          CHECK(
              device->CreateDepthStencilView(depth.Get(), &view, &dsv[flags]));
        }
        context->RSSetState(raster.Get());
        context->VSSetShader(vs.Get());
        context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
        context->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        context->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (UINT i = 0; i < cases.size(); i++) {
          const auto &k = cases[i];
          if (k.flags || k.shader > 1)
            continue;
          ComPtr<ID3D10DepthStencilState> state;
          D3D10_DEPTH_STENCIL_DESC state_desc;
          static_assert(sizeof(state_desc) == sizeof(k.state));
          memcpy(&state_desc, &k.state, sizeof(state_desc));
          CHECK(device->CreateDepthStencilState(&state_desc, &state));
          context->OMSetDepthStencilState(state.Get(), k.reference);
          context->PSSetShader(ps[k.shader].Get());
          context->OMSetRenderTargets(1, rtv.GetAddressOf(),
                                      k.absent ? nullptr : dsv[k.flags].Get());
          for (UINT face = 0; face < faces; face++) {
            step("depth format %u, path %u, case %u, face %u", format.value,
                 path, i, face);
            params.depth = k.depth;
            params.reverse = face;
            context->UpdateSubresource(cb.Get(), 0, nullptr, &params, 0, 0);
            UINT y = i * faces + face;
            D3D10_VIEWPORT viewport{0, INT(y), width, 1, k.min, k.max};
            D3D10_RECT scissor{1, LONG(y), LONG(width - 1), LONG(y + 1)};
            context->RSSetViewports(1, &viewport);
            context->RSSetScissorRects(1, &scissor);
            context->Draw(3, 0);
          }
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        device->CopyResource(color_read.Get(), color.Get());
        device->CopyResource(depth_read.Get(), depth.Get());
        D3D10_MAPPED_TEXTURE2D pixels{}, values{};
        CHECK(color_read->Map(0, D3D10_MAP_READ, 0, &pixels));
        CHECK(depth_read->Map(0, D3D10_MAP_READ, 0, &values));
        for (UINT i = 0; i < cases.size(); i++)
          for (UINT face = 0; face < faces; face++) {
            const auto &k = cases[i];
            step("depth format %u, path %u, case %u, face %u: readback",
                 format.value, path, i, face);
            UINT y = i * faces + face;
            bool active = !k.flags && k.shader <= 1;
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
              if (!expect(
                      depth_word == want.depth && color_word == want.color &&
                          (!format.stencil_bits ||
                           p[format.depth_bits / CHAR_BIT] == want.stencil),
                      "column %u: depth %#x/%#x, color %u/%u, stencil %u/%u", x,
                      depth_word, want.depth, color_word, want.color,
                      format.stencil_bits ? p[format.depth_bits / CHAR_BIT] : 0,
                      want.stencil))
                break;
            }
          }
        depth_read->Unmap(0);
        color_read->Unmap(0);
      }
    }
  }
  return verdict();
}
