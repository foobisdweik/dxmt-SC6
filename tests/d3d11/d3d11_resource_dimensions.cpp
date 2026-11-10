// contract: each texture dimension is positive and within its feature level's maximum; validation-only creation
// applies the same rules. "Size dimensions are restricted only to positive integers" (D3D11.3 5.5), with the
// per-resource maxima in 5.3.5/5.3.6/5.3.7 and the D3D10_REQ_TEXTURE*/D3D11_REQ_TEXTURE* SDK constants.
// "S_FALSE indicates a pass" for null texture output (Microsoft Learn, CreateTexture1D/2D/3D).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createtexture2d
// only one axis grows at a time, keeping every legal boundary small enough to initialize and read in full.
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <d3d11_3.h>
#include <algorithm>
#include <array>
#include <bit>
#include <climits>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, &context));
  ComPtr<ID3D11Device3> device3;
  CHECK(device.As(&device3));
  ComPtr<ID3D10Device1> legacy;
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create != nullptr, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1,
                 D3D10_1_SDK_VERSION, &legacy));
  ComPtr<ID3D11Device> legacy11;
  CHECK(legacy.As(&legacy11));
  ComPtr<ID3D11DeviceContext> legacy_context;
  legacy11->GetImmediateContext(&legacy_context);
  ComPtr<ID3D11Device1> legacy1;
  ComPtr<ID3D11DeviceContext1> legacy_context1;
  CHECK(legacy11.As(&legacy1));
  CHECK(legacy_context.As(&legacy_context1));
  const D3D_FEATURE_LEVEL legacy_level = D3D_FEATURE_LEVEL_10_1;
  ComPtr<ID3DDeviceContextState> readback_state;
  CHECK(legacy1->CreateDeviceContextState(0, &legacy_level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1),
                                         nullptr, &readback_state));
  for (UINT route = 0; route < 3; route++)
    for (UINT dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
         dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D; dimension++) {
      const bool via10 = route == 2;
      const UINT limit = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D
                             ? (via10 ? D3D10_REQ_TEXTURE3D_U_V_OR_W_DIMENSION : D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION)
                             : dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D
                                   ? (via10 ? D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION : D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
                                   : (via10 ? D3D10_REQ_TEXTURE1D_U_DIMENSION : D3D11_REQ_TEXTURE1D_U_DIMENSION);
      auto d = via10 ? legacy11.Get() : device.Get();
      auto c = via10 ? legacy_context.Get() : context.Get();
      for (UINT axis = 0; axis < dimension - D3D11_RESOURCE_DIMENSION_BUFFER; axis++)
        for (UINT size : {0u, 1u, limit - 1, limit, limit + 1, UINT_MAX})
          for (bool output : {false, true}) {
            std::array<UINT, 3> extent{1, 1, 1};
            extent[axis] = size;
            const bool valid = size && size <= limit;
            std::vector<UINT> words(valid ? size : 1);
            for (UINT i = 0; i < words.size(); i++)
              words[i] = i * 3 + 1;
            const UINT row_pitch = valid ? extent[0] * UINT(sizeof(UINT)) : 0;
            const UINT depth_pitch = valid ? row_pitch * extent[1] : 0;
            const D3D11_SUBRESOURCE_DATA data{words.data(), row_pitch, depth_pitch};
            step("dimension %u axis %u size %u limit %u route %u output %u", dimension, axis, size, limit, route, output);
            ComPtr<ID3D11Resource> made;
            HRESULT hr;
            if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
              D3D11_TEXTURE1D_DESC desc{extent[0], 1, 1, DXGI_FORMAT_R32_UINT, D3D11_USAGE_DEFAULT,
                                         D3D11_BIND_SHADER_RESOURCE};
              ComPtr<ID3D11Texture1D> object;
              if (via10) {
                const auto desc10 = std::bit_cast<D3D10_TEXTURE1D_DESC>(desc);
                const auto initial = std::bit_cast<D3D10_SUBRESOURCE_DATA>(data);
                ComPtr<ID3D10Texture1D> old;
                hr = legacy->CreateTexture1D(&desc10, valid ? &initial : nullptr,
                                                output ? old.GetAddressOf() : nullptr);
                if (old)
                  CHECK(old.As(&made));
              } else {
                hr = d->CreateTexture1D(&desc, valid ? &data : nullptr, output ? object.GetAddressOf() : nullptr);
                made = object;
              }
            } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
              D3D11_TEXTURE2D_DESC desc{extent[0], extent[1], 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                         D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
              ComPtr<ID3D11Texture2D> object;
              if (via10) {
                const auto desc10 = std::bit_cast<D3D10_TEXTURE2D_DESC>(desc);
                const auto initial = std::bit_cast<D3D10_SUBRESOURCE_DATA>(data);
                ComPtr<ID3D10Texture2D> old;
                hr = legacy->CreateTexture2D(&desc10, valid ? &initial : nullptr,
                                                output ? old.GetAddressOf() : nullptr);
                if (old)
                  CHECK(old.As(&made));
              } else if (route) {
                D3D11_TEXTURE2D_DESC1 desc1{desc.Width, desc.Height, 1, 1, desc.Format, desc.SampleDesc, desc.Usage,
                                             desc.BindFlags, 0, 0, D3D11_TEXTURE_LAYOUT_UNDEFINED};
                ComPtr<ID3D11Texture2D1> object1;
                hr = device3->CreateTexture2D1(&desc1, valid ? &data : nullptr,
                                                output ? object1.GetAddressOf() : nullptr);
                made = object1;
              } else {
                hr = d->CreateTexture2D(&desc, valid ? &data : nullptr, output ? object.GetAddressOf() : nullptr);
                made = object;
              }
            } else {
              D3D11_TEXTURE3D_DESC desc{extent[0], extent[1], extent[2], 1, DXGI_FORMAT_R32_UINT,
                                         D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
              ComPtr<ID3D11Texture3D> object;
              if (via10) {
                const auto desc10 = std::bit_cast<D3D10_TEXTURE3D_DESC>(desc);
                const auto initial = std::bit_cast<D3D10_SUBRESOURCE_DATA>(data);
                ComPtr<ID3D10Texture3D> old;
                hr = legacy->CreateTexture3D(&desc10, valid ? &initial : nullptr,
                                                output ? old.GetAddressOf() : nullptr);
                if (old)
                  CHECK(old.As(&made));
              } else if (route) {
                D3D11_TEXTURE3D_DESC1 desc1{desc.Width, desc.Height, desc.Depth, 1, desc.Format, desc.Usage,
                                             desc.BindFlags, 0, 0, D3D11_TEXTURE_LAYOUT_UNDEFINED};
                ComPtr<ID3D11Texture3D1> object1;
                hr = device3->CreateTexture3D1(&desc1, valid ? &data : nullptr,
                                                output ? object1.GetAddressOf() : nullptr);
                made = object1;
              } else {
                hr = d->CreateTexture3D(&desc, valid ? &data : nullptr, output ? object.GetAddressOf() : nullptr);
                made = object;
              }
            }
            const HRESULT want = valid ? (output ? S_OK : S_FALSE) : E_INVALIDARG;
            if (!expect(hr == want && bool(made) == (valid && output), "HRESULT %#lx, want %#lx, resource %p",
                        hr, want, made.Get()))
              return verdict();
            if (!made)
              continue;
            ComPtr<ID3DDeviceContextState> previous;
            if (via10)
              legacy_context1->SwapDeviceContextState(readback_state.Get(), &previous);
            ComPtr<ID3D11Resource> staging;
            if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
              D3D11_TEXTURE1D_DESC desc{extent[0], 1, 1, DXGI_FORMAT_R32_UINT, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
              ComPtr<ID3D11Texture1D> object;
              CHECK(d->CreateTexture1D(&desc, nullptr, &object));
              staging = object;
            } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
              D3D11_TEXTURE2D_DESC desc{extent[0], extent[1], 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_STAGING,
                                         0, D3D11_CPU_ACCESS_READ};
              ComPtr<ID3D11Texture2D> object;
              CHECK(d->CreateTexture2D(&desc, nullptr, &object));
              staging = object;
            } else {
              D3D11_TEXTURE3D_DESC desc{extent[0], extent[1], extent[2], 1, DXGI_FORMAT_R32_UINT, D3D11_USAGE_STAGING,
                                         0, D3D11_CPU_ACCESS_READ};
              ComPtr<ID3D11Texture3D> object;
              CHECK(d->CreateTexture3D(&desc, nullptr, &object));
              staging = object;
            }
            c->CopyResource(staging.Get(), made.Get());
            D3D11_MAPPED_SUBRESOURCE mapped;
            CHECK(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            for (UINT z = 0; z < extent[2]; z++)
              for (UINT y = 0; y < extent[1]; y++)
                for (UINT x = 0; x < extent[0]; x++) {
                  UINT got;
                  memcpy(&got, static_cast<char *>(mapped.pData) + z * mapped.DepthPitch + y * mapped.RowPitch +
                                   x * sizeof(UINT), sizeof(got));
                  expect(got == words[(z * extent[1] + y) * extent[0] + x], "texel (%u,%u,%u) differs", x, y, z);
                }
            c->Unmap(staging.Get(), 0);
            if (via10)
              legacy_context1->SwapDeviceContextState(previous.Get(), nullptr);
          }
    }
  return verdict();
}
