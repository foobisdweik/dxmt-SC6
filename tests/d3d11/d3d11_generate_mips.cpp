// contract: GenerateMips modifies only the lower mip levels selected by its SRV, on every selected slice.
// "largest mipmap level of the view" and "smallest level specified by the view" (Microsoft Learn, GenerateMips).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-generatemips
// "mipslice N+1 size = floor( mipslice N size / 2)" (D3D11.3 5 Resources); dimensions stop at one.
// the generation algorithm is not prescribed (5.9.3.3.1). constant zero/one fields are invariant under filtering;
// independent slice/component values distinguish selecting the wrong mip, face or slice without assuming a kernel.
// resources lacking GENERATE_MIPS must remain unchanged (GenerateMips, Remarks). all other mips retain their bytes.
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <d3d11_1.h>
#include <algorithm>
#include <array>
#include <climits>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11Device1> device1;
  ComPtr<ID3D11DeviceContext1> immediate1;
  CHECK(device.As(&device1));
  CHECK(immediate.As(&immediate1));
  ComPtr<ID3DDeviceContextState> compatibility;
  CHECK(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D10Device1),
                                         nullptr, &compatibility));
  ComPtr<ID3D10Device1> legacy;
  CHECK(device.As(&legacy));
  struct Format { DXGI_FORMAT value; UINT channels; bool floating; };
  const Format formats[] = {{DXGI_FORMAT_R32_FLOAT, 1, true}, {DXGI_FORMAT_R32G32B32A32_FLOAT, 4, true},
                             {DXGI_FORMAT_R8G8B8A8_UNORM, 4, false}, {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, false},
                             {DXGI_FORMAT_A8_UNORM, 1, false}};
  UINT exercised = 0;
  for (const auto &format : formats) {
    UINT support = 0;
    CHECK(device->CheckFormatSupport(format.value, &support));
    if (format.value == DXGI_FORMAT_R8G8B8A8_UNORM)
      expect(support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN, "required RGBA8 mip generation is not advertised");
    if (!(support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN))
      continue;
    const UINT pixel_bytes = format.channels * (format.floating ? sizeof(float) : sizeof(unsigned char));
    for (UINT dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
         dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D; dimension++)
      for (UINT size : {1u, 2u, 3u, 7u, 8u, 9u, 31u, 32u, 33u})
        for (UINT cube_count : {0u, 1u, 2u}) {
          if (cube_count && dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            continue;
          const UINT faces = 6; // D3D11.3 5.3.8: a cube is six Texture2D faces
          const UINT layers = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1 : cube_count ? faces * cube_count : 3;
          const UINT height = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? 1 : size;
          const UINT depth = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? size : 1;
          UINT mips = 1;
          for (UINT extent = size; extent > 1; extent >>= 1)
            mips++;
          for (bool generated : {false, true}) {
            ComPtr<ID3D11Resource> resource, staging;
            for (bool readback : {false, true}) {
              const auto usage = readback ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
              const UINT bind = readback ? 0 : D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
              const UINT access = readback ? D3D11_CPU_ACCESS_READ : 0;
              const UINT misc = readback ? 0 : (generated ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0u) |
                                                   (cube_count ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u);
              ComPtr<ID3D11Resource> made;
              if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
                D3D11_TEXTURE1D_DESC desc{size, mips, layers, format.value, usage, bind, access, misc};
                ComPtr<ID3D11Texture1D> texture;
                CHECK(device->CreateTexture1D(&desc, nullptr, &texture));
                made = texture;
              } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC desc{size, height, mips, layers, format.value, {1, 0}, usage, bind, access, misc};
                ComPtr<ID3D11Texture2D> texture;
                CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
                made = texture;
              } else {
                D3D11_TEXTURE3D_DESC desc{size, height, depth, mips, format.value, usage, bind, access, misc};
                ComPtr<ID3D11Texture3D> texture;
                CHECK(device->CreateTexture3D(&desc, nullptr, &texture));
                made = texture;
              }
              (readback ? staging : resource) = made;
            }
            for (UINT first_mip = 0; first_mip < mips; first_mip++)
              for (UINT mip_count : {1u, mips - first_mip})
                for (UINT first_layer : {0u, layers - (cube_count ? faces : 1)})
                  for (UINT route = 0; route < 3; route++) {
                    step("format %u dimension %u size %u cubes %u generated %u mip [%u,%u) layer %u route %u",
                         format.value, dimension, size, cube_count, generated, first_mip, first_mip + mip_count,
                         first_layer, route);
                    const UINT selected_layers = layers - first_layer;
                    std::vector<std::vector<unsigned char>> contents(layers * mips);
                    for (UINT layer = 0; layer < layers; layer++)
                      for (UINT mip = 0; mip < mips; mip++) {
                        const UINT sub = D3D11CalcSubresource(mip, layer, mips);
                        const UINT w = std::max(1u, size >> mip), h = std::max(1u, height >> mip);
                        const UINT d = std::max(1u, depth >> mip);
                        auto &bytes = contents[sub];
                        bytes.resize(w * h * d * pixel_bytes);
                        for (UINT p = 0; p < w * h * d; p++)
                          for (UINT c = 0; c < format.channels; c++) {
                            const bool high = (mip + layer + c) % 2;
                            if (format.floating) {
                              const float value = high ? 1.0f : 0.0f;
                              memcpy(bytes.data() + p * pixel_bytes + c * sizeof(value), &value, sizeof(value));
                            } else
                              bytes[p * pixel_bytes + c] = high ? UCHAR_MAX : 0;
                          }
                        immediate->UpdateSubresource(resource.Get(), sub, nullptr, bytes.data(), w * pixel_bytes,
                                                       w * h * pixel_bytes);
                      }
                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{format.value};
                    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
                      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
                      desc.Texture1DArray = {first_mip, mip_count, first_layer, selected_layers};
                    } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
                      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
                      desc.Texture3D = {first_mip, mip_count};
                    } else if (cube_count) {
                      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
                      desc.TextureCubeArray = {first_mip, mip_count, first_layer, selected_layers / faces};
                    } else {
                      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                      desc.Texture2DArray = {first_mip, mip_count, first_layer, selected_layers};
                    }
                    ComPtr<ID3D11ShaderResourceView> view;
                    CHECK(device->CreateShaderResourceView(resource.Get(), &desc, &view));
                    if (route == 2) {
                      ComPtr<ID3D10ShaderResourceView> view10;
                      CHECK(view.As(&view10));
                      ComPtr<ID3DDeviceContextState> previous;
                      immediate1->SwapDeviceContextState(compatibility.Get(), &previous);
                      legacy->GenerateMips(view10.Get());
                      immediate1->SwapDeviceContextState(previous.Get(), nullptr);
                    } else {
                      (route ? deferred.Get() : immediate.Get())->GenerateMips(view.Get());
                      if (route) {
                        ComPtr<ID3D11CommandList> commands;
                        CHECK(deferred->FinishCommandList(FALSE, &commands));
                        immediate->ExecuteCommandList(commands.Get(), FALSE);
                      }
                    }
                    immediate->CopyResource(staging.Get(), resource.Get());
                    exercised++;
                    for (UINT layer = 0; layer < layers; layer++)
                      for (UINT mip = 0; mip < mips; mip++) {
                        const UINT sub = D3D11CalcSubresource(mip, layer, mips);
                        const bool changed = generated && layer >= first_layer && mip > first_mip &&
                                             mip < first_mip + mip_count;
                        const auto &want = contents[changed ? D3D11CalcSubresource(first_mip, layer, mips) : sub];
                        D3D11_MAPPED_SUBRESOURCE mapped;
                        CHECK(immediate->Map(staging.Get(), sub, D3D11_MAP_READ, 0, &mapped));
                        bool ok = true;
                        const UINT w = std::max(1u, size >> mip), h = std::max(1u, height >> mip);
                        for (UINT z = 0; z < std::max(1u, depth >> mip) && ok; z++)
                          for (UINT y = 0; y < h && ok; y++)
                            for (UINT x = 0; x < w * pixel_bytes && ok; x++) {
                              const auto got = static_cast<unsigned char *>(mapped.pData)[z * mapped.DepthPitch +
                                                                                          y * mapped.RowPitch + x];
                              const auto expected = want[changed ? x % pixel_bytes : (z * h + y) * w * pixel_bytes + x];
                              ok = expect(got == expected, "sub %u z %u row %u byte %u: %#x, want %#x",
                                          sub, z, y, x, got, expected);
                            }
                        immediate->Unmap(staging.Get(), sub);
                        if (!ok)
                          return verdict();
                      }
                  }
          }
        }
  }
  expect(exercised != 0, "no mip generation configuration was exercised");
  return verdict();
}
