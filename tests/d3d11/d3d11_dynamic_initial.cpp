// contract: dynamic texture creation copies the active texels, independent of ignored 1D pitches or 2D padding.
// "System-memory pitch is used only for 2D and 3D texture data" (Microsoft Learn, D3D11_SUBRESOURCE_DATA).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_subresource_data
// "Pitch values are restricted only to non-negative integers, intentionally including zero for which the first
// row will be replicated to all rows." (D3D11.3 5.5). the oversized source pitch is derived from Map's row pitch,
// so it crosses the implementation's row boundary on each GPU. only active texel bytes are compared.
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <d3d11_1.h>
#include <algorithm>
#include <array>
#include <bit>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, &context));
  ComPtr<ID3D11Device1> device1;
  ComPtr<ID3D11DeviceContext1> context1;
  CHECK(device.As(&device1));
  CHECK(context.As(&context1));
  ComPtr<ID3DDeviceContextState> compatibility;
  CHECK(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D10Device1),
                                         nullptr, &compatibility));
  ComPtr<ID3D10Device1> legacy;
  CHECK(device.As(&legacy));
  constexpr UINT word_bytes = sizeof(UINT), padding = 3, sentinel = 0x45abcdef;
  const UINT page_words = MULLION_PAGE_SIZE / word_bytes;
  for (UINT dimension : {UINT(D3D11_RESOURCE_DIMENSION_TEXTURE1D), UINT(D3D11_RESOURCE_DIMENSION_TEXTURE2D)})
    for (UINT width : {1u, 3u, 7u, 8u, 9u, page_words - 1, page_words, page_words + 1}) {
      const UINT height = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? 1 : 3;
      auto make = [&](D3D11_USAGE usage, const D3D11_SUBRESOURCE_DATA *data, bool via10,
                      ComPtr<ID3D11Resource> &out) -> HRESULT {
        const UINT bind = usage == D3D11_USAGE_STAGING ? 0 : D3D11_BIND_SHADER_RESOURCE;
        const UINT access = usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ : D3D11_CPU_ACCESS_WRITE;
        D3D10_SUBRESOURCE_DATA initial{};
        if (data)
          initial = std::bit_cast<D3D10_SUBRESOURCE_DATA>(*data);
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
          D3D11_TEXTURE1D_DESC desc{width, 1, 1, DXGI_FORMAT_R32_UINT, usage, bind, access};
          if (via10) {
            const auto desc10 = std::bit_cast<D3D10_TEXTURE1D_DESC>(desc);
            ComPtr<ID3D10Texture1D> texture;
            HRESULT hr = legacy->CreateTexture1D(&desc10, data ? &initial : nullptr, &texture);
            return FAILED(hr) ? hr : texture.As(&out);
          }
          ComPtr<ID3D11Texture1D> texture;
          HRESULT hr = device->CreateTexture1D(&desc, data, &texture);
          out = texture;
          return hr;
        }
        D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, usage, bind, access};
        if (via10) {
          const auto desc10 = std::bit_cast<D3D10_TEXTURE2D_DESC>(desc);
          ComPtr<ID3D10Texture2D> texture;
          HRESULT hr = legacy->CreateTexture2D(&desc10, data ? &initial : nullptr, &texture);
          return FAILED(hr) ? hr : texture.As(&out);
        }
        ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = device->CreateTexture2D(&desc, data, &texture);
        out = texture;
        return hr;
      };
      ComPtr<ID3D11Resource> probe, staging;
      CHECK(make(D3D11_USAGE_DYNAMIC, nullptr, false, probe));
      CHECK(make(D3D11_USAGE_STAGING, nullptr, false, staging));
      D3D11_MAPPED_SUBRESOURCE mapped;
      CHECK(context->Map(probe.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
      const UINT row_boundary = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? width * word_bytes : mapped.RowPitch;
      context->Unmap(probe.Get(), 0);
      if (!expect(row_boundary >= width * word_bytes && row_boundary % word_bytes == 0, "invalid Map row pitch"))
        return verdict();
      for (UINT pitch : {0u, width * word_bytes, row_boundary, row_boundary + padding * word_bytes})
        for (bool via10 : {false, true}) {
          step("dynamic initial dimension %u width %u source pitch %u row boundary %u Direct3D %u",
               dimension, width, pitch, row_boundary, via10 ? 10 : 11);
          const UINT stride = pitch / word_bytes;
          std::vector<UINT> source(std::max(width, stride) * height + width, sentinel);
          for (UINT y = 0; y < height; y++)
            for (UINT x = 0; x < width; x++)
              source[y * stride + x] = (y * width + x + 1) * padding;
          std::vector<UINT> expected(width * height);
          for (UINT y = 0; y < height; y++)
            std::copy_n(source.begin() + y * stride, width, expected.begin() + y * width);
          D3D11_SUBRESOURCE_DATA data{source.data(), pitch, UINT(source.size() * word_bytes)};
          ComPtr<ID3D11Resource> resource;
          ComPtr<ID3DDeviceContextState> previous;
          if (via10)
            context1->SwapDeviceContextState(compatibility.Get(), &previous);
          const HRESULT hr = make(D3D11_USAGE_DYNAMIC, &data, via10, resource);
          if (via10)
            context1->SwapDeviceContextState(previous.Get(), nullptr);
          CHECK(hr);
          std::fill(source.begin(), source.end(), sentinel);
          context->CopyResource(staging.Get(), resource.Get());
          CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
          bool ok = true;
          for (UINT y = 0; y < height && ok; y++)
            for (UINT x = 0; x < width && ok; x++) {
              UINT got;
              memcpy(&got, static_cast<char *>(mapped.pData) + y * mapped.RowPitch + x * word_bytes, sizeof(got));
              ok = expect(got == expected[y * width + x], "texel (%u,%u) %#x, want %#x",
                          x, y, got, expected[y * width + x]);
            }
          context->Unmap(staging.Get(), 0);
          if (!ok)
            return verdict();
        }
    }
  return verdict();
}
