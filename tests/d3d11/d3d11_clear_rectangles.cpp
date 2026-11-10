// contract: ClearView rectangles use view coordinates and stay inside every selected slice; 3D calls are dropped.
// "The driver or hardware is responsible for clamping rects to the surface extents." (D3D11.3 5.2.3.3).
// "an empty list clears the entire surface" (5.2.3.3); the null-list case checks all selected subresources.
// "that of the view format" (5.2.3.3.1).
// "ClearView doesn’t support 3D textures." (Microsoft Learn, ID3D11DeviceContext1::ClearView, Remarks).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-clearview
// Uint clears copy "the lower ni bits from each array element i to the corresponding channel" (D3D11.3 5.2.3.2).
// membership in seeded rectangles determines each expected word; format names from the SDK give channel widths.
// copies read all elements and all mips and slices, including those outside the view. there are no shaders.
#include "d3d11_test.hpp"
#include "../copy_formats.hpp"
#include <d3d11_2.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <random>

int
main(int argc, char **argv) {
  const UINT seed = argc > 1 ? UINT(strtoul(argv[1], nullptr, 0)) : std::mt19937::default_seed;
  std::mt19937 random(seed);
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate
  ));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11DeviceContext1> immediate1, deferred1;
  CHECK(immediate.As(&immediate1));
  CHECK(deferred.As(&deferred1));
  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  D3D11_FEATURE_DATA_D3D11_OPTIONS1 options1{};
  CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)));
  CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &options1, sizeof(options1)));
  auto map = [&](ID3D11Resource *resource, UINT sub, D3D11_MAPPED_SUBRESOURCE &mapped) -> HRESULT {
    return immediate->Map(resource, sub, D3D11_MAP_READ, 0, &mapped);
  };
  const UINT sentinel = 0x45abcdef;
  UINT variant = 0;
  auto run = [&](UINT dimension, UINT width, UINT layers, UINT mip, UINT first, UINT count, UINT stride, UINT kind,
                 UINT view_kind, bool floating, bool listed, bool ranged = false) -> int {
    const bool buffered = dimension == D3D11_RESOURCE_DIMENSION_BUFFER;
    const bool volume = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    const UINT height = dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE1D ? 1 : 5;
    const UINT mips = buffered ? 1 : 2, subresources = mips * (buffered || volume ? 1 : layers);
    const UINT bind = view_kind == 1   ? D3D11_BIND_RENDER_TARGET
                      : view_kind == 2 ? D3D11_BIND_DEPTH_STENCIL
                                       : D3D11_BIND_UNORDERED_ACCESS;
    const DXGI_FORMAT format = view_kind == 2          ? DXGI_FORMAT_R32_TYPELESS
                               : floating || view_kind ? DXGI_FORMAT_R32_FLOAT
                                                       : DXGI_FORMAT_R32_UINT;
    std::vector<std::vector<UINT>> words(subresources);
    std::vector<std::array<UINT, 3>> extents(subresources);
    std::vector<D3D11_SUBRESOURCE_DATA> initial(subresources);
    for (UINT sub = 0; sub < subresources; sub++) {
      auto &e = extents[sub];
      e = {
          buffered ? width * stride : std::max(1u, width >> (sub % mips)), std::max(1u, height >> (sub % mips)),
          volume ? std::max(1u, layers >> (sub % mips)) : 1
      };
      words[sub].assign(e[0] * e[1] * e[2], sentinel);
      initial[sub] = {words[sub].data(), e[0] * UINT(sizeof(UINT)), e[0] * e[1] * UINT(sizeof(UINT))};
    }
    ComPtr<ID3D11Resource> resource, staging;
    for (bool readback : {false, true}) {
      const auto usage = readback ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
      const UINT access = readback ? D3D11_CPU_ACCESS_READ : 0;
      ComPtr<ID3D11Resource> made;
      if (buffered) {
        const UINT misc = kind == 0   ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
                          : kind == 1 ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED
                                      : 0;
        D3D11_BUFFER_DESC desc{
            width * stride * UINT(sizeof(UINT)),
            usage,
            readback ? 0 : bind,
            access,
            readback ? 0 : misc,
            kind == 1 && !readback ? stride * UINT(sizeof(UINT)) : 0
        };
        ComPtr<ID3D11Buffer> buffer;
        CHECK(device->CreateBuffer(&desc, readback ? nullptr : initial.data(), &buffer));
        made = buffer;
      } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        D3D11_TEXTURE1D_DESC desc{width, mips, layers, format, usage, readback ? 0 : bind, access};
        ComPtr<ID3D11Texture1D> texture;
        CHECK(device->CreateTexture1D(&desc, readback ? nullptr : initial.data(), &texture));
        made = texture;
      } else if (volume) {
        D3D11_TEXTURE3D_DESC desc{width, height, layers, mips, format, usage, readback ? 0 : bind, access};
        ComPtr<ID3D11Texture3D> texture;
        CHECK(device->CreateTexture3D(&desc, readback ? nullptr : initial.data(), &texture));
        made = texture;
      } else {
        D3D11_TEXTURE2D_DESC desc{width, height, mips, layers, format, {1, 0}, usage, readback ? 0 : bind, access};
        ComPtr<ID3D11Texture2D> texture;
        CHECK(device->CreateTexture2D(&desc, readback ? nullptr : initial.data(), &texture));
        made = texture;
      }
      (readback ? staging : resource) = made;
    }
    const UINT first_slice = !buffered && layers > 1 && ranged ? 1 : 0;
    const UINT depth = volume ? std::max(1u, layers >> mip) : layers;
    const UINT slices = first_slice ? depth - 2 : depth;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11View> view;
    if (!view_kind) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
      desc.Format = format;
      if (buffered) {
        desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        desc.Format = kind == 0 ? DXGI_FORMAT_R32_TYPELESS : kind == 1 ? DXGI_FORMAT_UNKNOWN : format;
        desc.Buffer = {first, count, kind == 0 ? UINT(D3D11_BUFFER_UAV_FLAG_RAW) : 0u};
      } else if (volume) {
        desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
        desc.Texture3D = {mip, first_slice, slices};
      } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        desc.ViewDimension = layers > 1 ? D3D11_UAV_DIMENSION_TEXTURE1DARRAY : D3D11_UAV_DIMENSION_TEXTURE1D;
        desc.Texture1DArray = {mip, first_slice, slices};
      } else {
        desc.ViewDimension = layers > 1 ? D3D11_UAV_DIMENSION_TEXTURE2DARRAY : D3D11_UAV_DIMENSION_TEXTURE2D;
        desc.Texture2DArray = {mip, first_slice, slices};
      }
      CHECK(device->CreateUnorderedAccessView(resource.Get(), &desc, &uav));
      view = uav;
    } else if (view_kind == 1) {
      D3D11_RENDER_TARGET_VIEW_DESC desc{};
      desc.Format = format;
      if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        desc.ViewDimension = layers > 1 ? D3D11_RTV_DIMENSION_TEXTURE1DARRAY : D3D11_RTV_DIMENSION_TEXTURE1D;
        desc.Texture1DArray = {mip, first_slice, slices};
      } else {
        desc.ViewDimension = layers > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DARRAY : D3D11_RTV_DIMENSION_TEXTURE2D;
        desc.Texture2DArray = {mip, first_slice, slices};
      }
      ComPtr<ID3D11RenderTargetView> rtv;
      CHECK(device->CreateRenderTargetView(resource.Get(), &desc, &rtv));
      view = rtv;
    } else {
      D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
      desc.Format = DXGI_FORMAT_D32_FLOAT;
      desc.ViewDimension = layers > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DARRAY : D3D11_DSV_DIMENSION_TEXTURE2D;
      desc.Texture2DArray = {mip, first_slice, slices};
      ComPtr<ID3D11DepthStencilView> dsv;
      CHECK(device->CreateDepthStencilView(resource.Get(), &desc, &dsv));
      view = dsv;
    }
    const LONG w = buffered ? count : extents[mip][0], h = extents[mip][1];
    auto context = listed ? deferred1.Get() : immediate1.Get();
    for (UINT mode = 0; mode < 9; mode++) {
      if (mode && (!options.ClearView || (view_kind == 2 && !options1.ClearViewAlsoSupportsDepthOnlyFormats)))
        continue;
      if ((!mode && (view_kind || (buffered && kind != 2))) || (mode && !floating))
        continue;
      std::vector<D3D11_RECT> rects;
      switch (mode) {
      case 0:
        break;
      case 1:
        rects = {{1, 0, w - 1, h}};
        break;
      case 2:
        rects = {{-2, -1, w + 2, h + 1}};
        break;
      case 3:
        rects = {{0, 0, 0, h}, {0, 0, w, 0}, {w, 0, 0, h}, {0, h, w, 0}};
        break;
      case 4:
        rects = {{0, 0, w / 2 + 1, h}, {w / 3, 0, w, h}};
        break;
      case 5:
        rects = {{-3, 0, -1, h}, {w, 0, w + 3, h}, {0, -3, w, 0}, {0, h, w, h + 3}};
        break;
      case 6:
        rects = {
            {std::numeric_limits<LONG>::min(), std::numeric_limits<LONG>::min(), std::numeric_limits<LONG>::max(),
             std::numeric_limits<LONG>::max()}
        };
        break;
      case 8:
        break;
      default:
        for (UINT i = 0; i < 8; i++)
          rects.push_back(
              {LONG(random() % (w + 4)) - 2, LONG(random() % (h + 4)) - 2, LONG(random() % (w + 4)) - 2,
               LONG(random() % (h + 4)) - 2}
          );
        break;
      }
      step(
          "seed %u variant %u dim %u width %u kind %u first %u count %u mip %u view %u float %u listed %u mode %u",
          seed, variant, dimension, width, kind, first, count, mip, view_kind, floating, listed, mode
      );
      for (auto &r : rects)
        printf("rect: %ld,%ld-%ld,%ld\n", r.left, r.top, r.right, r.bottom);
      for (UINT sub = 0; sub < subresources; sub++)
        immediate->UpdateSubresource(
            resource.Get(), sub, nullptr, words[sub].data(), initial[sub].SysMemPitch, initial[sub].SysMemSlicePitch
        );
      const UINT uints[4] = {17, 29, 43, 59};
      const float floats[4] = {0.25f, 0.5f, 0.75f, 1.0f};
      const float clear_view[4] = {};
      UINT value = uints[0];
      if (mode)
        value = 0;
      else if (floating)
        memcpy(&value, &floats[0], sizeof(value));
      if (!mode) {
        if (floating)
          context->ClearUnorderedAccessViewFloat(uav.Get(), floats);
        else
          context->ClearUnorderedAccessViewUint(uav.Get(), uints);
      } else
        context->ClearView(view.Get(), clear_view, rects.empty() ? nullptr : rects.data(), UINT(rects.size()));
      if (listed) {
        ComPtr<ID3D11CommandList> commands;
        CHECK(deferred->FinishCommandList(FALSE, &commands));
        immediate->ExecuteCommandList(commands.Get(), FALSE);
      }
      immediate->CopyResource(staging.Get(), resource.Get());
      for (UINT sub = 0; sub < subresources; sub++) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        CHECK(map(staging.Get(), sub, mapped));
        auto e = extents[sub];
        bool ok = true;
        for (UINT z = 0; z < e[2] && ok; z++)
          for (UINT y = 0; y < e[1] && ok; y++)
            for (UINT x = 0; x < e[0] && ok; x++) {
              const UINT element = buffered ? x / stride : x, slice = volume ? z : sub / mips;
              const LONG local = LONG(element) - (buffered ? LONG(first) : 0);
              const bool in_view = buffered ? element >= first && element < first + count
                                            : sub % mips == mip && slice >= first_slice && slice < first_slice + slices;
              bool covered = mode == 0 || mode == 8;
              for (auto &r : rects)
                covered |= local >= r.left && local < r.right && LONG(y) >= r.top && LONG(y) < r.bottom;
              const UINT want = in_view && covered && !(volume && mode) ? value : sentinel;
              UINT got;
              memcpy(
                  &got,
                  static_cast<char *>(mapped.pData) + z * mapped.DepthPitch + y * mapped.RowPitch + x * sizeof(UINT),
                  sizeof(got)
              );
              ok = expect(got == want, "sub %u (%u,%u,%u): %#x, want %#x", sub, x, y, z, got, want);
            }
        immediate->Unmap(staging.Get(), sub);
        if (!ok)
          return 1;
      }
    }
    return 0;
  };
  for (UINT boundary = 2; boundary <= MULLION_PAGE_SIZE / sizeof(UINT); boundary *= 2)
    for (UINT count : {boundary - 1, boundary, boundary + 1})
      for (UINT kind = 0; kind < 3; kind++)
        for (UINT first : {0u, 3u})
          for (bool listed : {false, true}) {
            variant++;
            if (run(D3D11_RESOURCE_DIMENSION_BUFFER, first + count + 3, 1, 0, first, count, kind == 1 ? 3 : 1, kind, 0,
                    true, listed))
              return 1;
          }
  for (UINT boundary = 4; boundary <= MULLION_PAGE_SIZE / sizeof(UINT); boundary *= 2)
    for (UINT dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D; dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D;
         dimension++)
      for (UINT width : {boundary - 1, boundary, boundary + 1})
        for (UINT layers : {1u, 8u})
          for (UINT mip : {0u, 1u})
            for (UINT view_kind = 0; view_kind < 3; view_kind++) {
              if ((view_kind && dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D) ||
                  (view_kind == 2 && dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D))
                continue;
              for (bool ranged : {false, true}) {
                if (layers == 1 && ranged)
                  continue;
                for (bool floating : {false, true}) {
                  if (view_kind && !floating)
                    continue;
                  for (bool listed : {false, true}) {
                    variant++;
                    if (run(dimension, width, layers, mip, 0, 0, 1, 2, view_kind, floating, listed, ranged))
                      return 1;
                  }
                }
              }
            }
  UINT tested_widths = 0;
  for (const auto &format : copy_formats) {
    const auto length = strlen(format.name);
    if (format.depth || format.unit != 1 || length < 5 || strcmp(format.name + length - 5, "_UINT"))
      continue;
    UINT support;
    CHECK(device->CheckFormatSupport(format.id, &support));
    if (!(support & D3D11_FORMAT_SUPPORT_BUFFER) || !(support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW))
      continue;
    std::vector<UINT> bits;
    for (const char *at = format.family; *at;) {
      const char channel = *at++;
      char *end;
      bits.push_back(UINT(strtoul(at, &end, 10)));
      if (!expect(
              end != at && strchr("RGBA", channel) && bits.back() <= std::numeric_limits<UINT>::digits,
              "unexpected channel layout %s", format.family
          ))
        return verdict();
      at = end;
    }
    const UINT count = MULLION_PAGE_SIZE / format.bytes + 1, first = 3, total = first + count + 3;
    for (bool listed : {false, true}) {
      step("seed %u typed Uint %s first %u count %u listed %u", seed, format.name, first, count, listed);
      std::vector<unsigned char> bytes(total * format.bytes, 0xa5);
      D3D11_BUFFER_DESC desc{UINT(bytes.size()), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS};
      D3D11_SUBRESOURCE_DATA initial{bytes.data()};
      ComPtr<ID3D11Buffer> resource, staging;
      CHECK(device->CreateBuffer(&desc, &initial, &resource));
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateBuffer(&desc, nullptr, &staging));
      D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc{format.id, D3D11_UAV_DIMENSION_BUFFER};
      view_desc.Buffer = {first, count, 0};
      ComPtr<ID3D11UnorderedAccessView> view;
      CHECK(device->CreateUnorderedAccessView(resource.Get(), &view_desc, &view));
      std::mt19937 values_random(seed ^ UINT(format.id) ^ UINT(listed));
      std::array<UINT, 4> values{};
      for (auto &value : values)
        value = (values_random() | (UINT(1) << (std::numeric_limits<UINT>::digits - 1))) & ~1u;
      (listed ? deferred.Get() : immediate.Get())->ClearUnorderedAccessViewUint(view.Get(), values.data());
      if (listed) {
        ComPtr<ID3D11CommandList> commands;
        CHECK(deferred->FinishCommandList(FALSE, &commands));
        immediate->ExecuteCommandList(commands.Get(), FALSE);
      }
      for (UINT element = first; element < first + count; element++) {
        UINT offset = 0;
        for (UINT channel = 0; channel < bits.size(); channel++) {
          tested_widths |= 1u << (bits[channel] % std::numeric_limits<UINT>::digits);
          for (UINT bit = 0; bit < bits[channel]; bit++, offset++) {
            auto &byte = bytes[element * format.bytes + offset / std::numeric_limits<unsigned char>::digits];
            const UINT mask = 1u << (offset % std::numeric_limits<unsigned char>::digits);
            byte = (byte & ~mask) | ((values[channel] >> bit & 1) ? mask : 0);
          }
        }
      }
      immediate->CopyResource(staging.Get(), resource.Get());
      D3D11_MAPPED_SUBRESOURCE mapped;
      CHECK(map(staging.Get(), 0, mapped));
      bool ok = true;
      for (UINT at = 0; at < bytes.size() && ok; at++)
        ok = expect(
            static_cast<unsigned char *>(mapped.pData)[at] == bytes[at], "byte %u: %#x, want %#x", at,
            static_cast<unsigned char *>(mapped.pData)[at], bytes[at]
        );
      immediate->Unmap(staging.Get(), 0);
      if (!ok)
        return 1;
    }
  }
  expect(
      (tested_widths & ((1u << 8) | (1u << 16) | (1u << 10) | (1u << 2))) ==
          ((1u << 8) | (1u << 16) | (1u << 10) | (1u << 2)),
      "8, 16 and 10/10/10/2-bit channels were exercised"
  );
  return verdict();
}
