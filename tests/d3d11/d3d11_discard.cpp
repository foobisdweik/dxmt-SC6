// contract: discard may invalidate only its specified elements; bindings and subsequent writes remain usable.
// "the contents of a resource (or the subset of it that is in a View) may be discarded." (D3D11.3 5.7).
// "The view can be an SRV, RTV, UAV, or DSV."; "If NULL, DiscardView1 discards the entire view"
// (Microsoft Learn, DiscardView1). discarded elements are overwritten before readback; all other texels keep
// their earlier value. default and staging resources therefore never rely on a discarded value being preserved.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-discardview1
#include "d3d11_test.hpp"
#include <d3d11_2.h>
#include <algorithm>
#include <array>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11DeviceContext1> contexts[2];
  CHECK(immediate.As(&contexts[0]));
  CHECK(deferred.As(&contexts[1]));
  D3D11_FEATURE_DATA_D3D11_OPTIONS1 options{};
  CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &options, sizeof(options)));
  constexpr UINT mips = 3, layers = 4, first_mip = 1, first_layer = 1, layer_count = 2;
  constexpr float before = 1, after = 0.5f;
  const float color[4] = {after};
  for (UINT width : {4u, 7u, 8u, 9u, 31u, 32u, 33u})
    for (UINT kind = 0; kind < 4; kind++) {
      const bool depth = kind == 3;
      if (depth && !options.ClearViewAlsoSupportsDepthOnlyFormats)
        continue;
      const UINT height = 5, bind = depth ? D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE
                                         : D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS |
                                               D3D11_BIND_SHADER_RESOURCE;
      D3D11_TEXTURE2D_DESC desc{width, height, mips, layers, depth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32_FLOAT,
                                 {1, 0}, D3D11_USAGE_DEFAULT, bind};
      ComPtr<ID3D11Texture2D> target, staging;
      CHECK(device->CreateTexture2D(&desc, nullptr, &target));
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
      std::array<ComPtr<ID3D11RenderTargetView>, mips * layers> rtvs;
      std::array<ComPtr<ID3D11DepthStencilView>, mips * layers> dsvs;
      for (UINT sub = 0; sub < mips * layers; sub++) {
        if (depth) {
          D3D11_DEPTH_STENCIL_VIEW_DESC view{DXGI_FORMAT_D32_FLOAT, D3D11_DSV_DIMENSION_TEXTURE2DARRAY};
          view.Texture2DArray = {sub % mips, sub / mips, 1};
          CHECK(device->CreateDepthStencilView(target.Get(), &view, &dsvs[sub]));
        } else {
          D3D11_RENDER_TARGET_VIEW_DESC view{DXGI_FORMAT_R32_FLOAT, D3D11_RTV_DIMENSION_TEXTURE2DARRAY};
          view.Texture2DArray = {sub % mips, sub / mips, 1};
          CHECK(device->CreateRenderTargetView(target.Get(), &view, &rtvs[sub]));
        }
      }
      ComPtr<ID3D11ShaderResourceView> srv;
      D3D11_SHADER_RESOURCE_VIEW_DESC sd{DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2DARRAY};
      sd.Texture2DArray = {first_mip, 1, first_layer, layer_count};
      CHECK(device->CreateShaderResourceView(target.Get(), &sd, &srv));
      ComPtr<ID3D11View> view;
      if (kind == 0)
        view = srv;
      else if (kind == 1) {
        D3D11_RENDER_TARGET_VIEW_DESC vd{DXGI_FORMAT_R32_FLOAT, D3D11_RTV_DIMENSION_TEXTURE2DARRAY};
        vd.Texture2DArray = {first_mip, first_layer, layer_count};
        ComPtr<ID3D11RenderTargetView> rtv;
        CHECK(device->CreateRenderTargetView(target.Get(), &vd, &rtv));
        view = rtv;
      } else if (kind == 2) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC vd{DXGI_FORMAT_R32_FLOAT, D3D11_UAV_DIMENSION_TEXTURE2DARRAY};
        vd.Texture2DArray = {first_mip, first_layer, layer_count};
        ComPtr<ID3D11UnorderedAccessView> uav;
        CHECK(device->CreateUnorderedAccessView(target.Get(), &vd, &uav));
        view = uav;
      } else {
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{DXGI_FORMAT_D32_FLOAT, D3D11_DSV_DIMENSION_TEXTURE2DARRAY};
        vd.Texture2DArray = {first_mip, first_layer, layer_count};
        ComPtr<ID3D11DepthStencilView> dsv;
        CHECK(device->CreateDepthStencilView(target.Get(), &vd, &dsv));
        view = dsv;
      }
      for (UINT route = 0; route < std::size(contexts); route++)
        for (UINT method = 0; method < 4; method++) {
          step("discard width %u kind %u route %u method %u", width, kind, route, method);
          auto context = contexts[route].Get();
          for (UINT sub = 0; sub < mips * layers; sub++) {
            if (depth)
              context->ClearDepthStencilView(dsvs[sub].Get(), D3D11_CLEAR_DEPTH, before, 0);
            else {
              const float old[4] = {before};
              context->ClearRenderTargetView(rtvs[sub].Get(), old);
            }
          }
          context->PSSetShaderResources(0, 1, srv.GetAddressOf());
          const D3D11_RECT rect{1, 0, LONG(width >> first_mip), LONG(height >> first_mip)};
          if (method == 0)
            context->DiscardResource(target.Get());
          else if (method == 1)
            context->DiscardView(view.Get());
          else
            context->DiscardView1(view.Get(), method == 2 ? &rect : nullptr, method == 2 ? 1 : 0);
          ComPtr<ID3D11ShaderResourceView> retained;
          context->PSGetShaderResources(0, 1, &retained);
          expect(retained == srv, "discard changed a resource binding");
          for (UINT sub = 0; sub < mips * layers; sub++) {
            const UINT mip = sub % mips, layer = sub / mips;
            const bool selected = method == 0 ||
                                  (mip == first_mip && layer >= first_layer && layer < first_layer + layer_count);
            if (!selected)
              continue;
            ID3D11View *part = depth ? static_cast<ID3D11View *>(dsvs[sub].Get())
                                    : static_cast<ID3D11View *>(rtvs[sub].Get());
            context->ClearView(part, color, method == 2 ? &rect : nullptr, method == 2 ? 1 : 0);
          }
          context->CopyResource(staging.Get(), target.Get());
          if (route) {
            ComPtr<ID3D11CommandList> commands;
            CHECK(deferred->FinishCommandList(FALSE, &commands));
            immediate->ExecuteCommandList(commands.Get(), FALSE);
          }
          for (UINT sub = 0; sub < mips * layers; sub++) {
            const UINT mip = sub % mips, layer = sub / mips;
            const bool selected = method == 0 ||
                                  (mip == first_mip && layer >= first_layer && layer < first_layer + layer_count);
            D3D11_MAPPED_SUBRESOURCE mapped;
            CHECK(immediate->Map(staging.Get(), sub, D3D11_MAP_READ, 0, &mapped));
            bool ok = true;
            for (UINT y = 0; y < std::max(1u, height >> mip) && ok; y++)
              for (UINT x = 0; x < std::max(1u, width >> mip) && ok; x++) {
                const bool in_rect = method != 2 || (LONG(x) >= rect.left && LONG(x) < rect.right &&
                                                       LONG(y) >= rect.top && LONG(y) < rect.bottom);
                const float want = selected && in_rect ? after : before;
                float got;
                memcpy(&got, static_cast<char *>(mapped.pData) + y * mapped.RowPitch + x * sizeof(float), sizeof(got));
                ok = expect(got == want, "sub %u pixel (%u,%u) %g, want %g", sub, x, y, got, want);
              }
            immediate->Unmap(staging.Get(), sub);
            if (!ok)
              return verdict();
          }
          context->ClearState();
        }
    }
  return verdict();
}
