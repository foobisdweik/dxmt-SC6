// contract: D3D10 views select exactly their mip, array, depth-slice or
// buffer-element interval, and getters describe that interval and its resource.
// D3D11.3 5.2.3.1: "The full extent of the resource view is always cleared.
// Viewport and scissor are not applied." 5.2.1: "the effective dimensions and
// array sizes of the Views will, naturally, always be less than or equal to the
// original Resource."
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// D3D10_TEX3D_RTV::WSize: "A value of -1 indicates all of the slices along the
// w axis, starting from FirstWSlice."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/ns-d3d10-d3d10_tex3d_rtv
// each subresource starts with distinct, exactly representable float integers.
// A clear changes only the selected words to 0.25, whose R32_FLOAT encoding is
// exact. Every defined word is checked through the D3D10 Map interface. SRV
// loads also read every selected mip/slice and one coordinate beyond each view
// boundary (5.2.1/22.4.6). constant-source GenerateMips must preserve that
// constant and every mip/array outside the SRV (5.2.1). cube-array SRV1
// descriptors are checked; filtered cube-array sampling is a separate contract.
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include <algorithm>
#include <bit>
#include <iterator>
#include <limits>

static const char hlsl[] = R"hlsl(
cbuffer Parameters : register(b0) { uint lod; uint rows; };
#if KIND == 1
Texture1DArray<float> tex : register(t0);
#elif KIND == 2
Texture2DArray<float> tex : register(t0);
#else
Texture3D<float> tex : register(t0);
#endif
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps(float4 position : SV_Position) : SV_Target {
  int x = int(position.x), y = int(position.y);
#if KIND == 1
  return tex.Load(int3(x, y, lod));
#else
  return tex.Load(int4(x, y % rows, y / rows, lod));
#endif
}
)hlsl";

int
main() {
  auto vertex_code = compile(hlsl, "vs", "vs_4_0", {"KIND=1"});
  ComPtr<ID3DBlob> pixel_code[3];
  for (UINT kind = 0; kind < std::size(pixel_code); kind++)
    pixel_code[kind] = compile(hlsl, "ps", "ps_4_0", {"KIND=" + std::to_string(kind + 1)});
  if (!expect(vertex_code && pixel_code[0] && pixel_code[1] && pixel_code[2], "view-load HLSL did not compile"))
    return verdict();
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")
  );
  if (!expect(create != nullptr, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  constexpr UINT width = D3D10_STANDARD_VECTOR_SIZE * 2, height = D3D10_STANDARD_VECTOR_SIZE;
  constexpr UINT depth = D3D10_STANDARD_VECTOR_SIZE, mips = std::bit_width(width);
  constexpr UINT word_bytes = sizeof(float);
  const float clear[] = {0.25f, 0.5f, 0.75f, 1};
  const UINT rest = std::numeric_limits<UINT>::max();
  for (auto level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, level, D3D10_1_SDK_VERSION, &device));
    ComPtr<ID3D10VertexShader> vertex_shader;
    ComPtr<ID3D10PixelShader> pixel_shaders[3];
    CHECK(device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), &vertex_shader));
    for (UINT i = 0; i < std::size(pixel_code); i++)
      CHECK(device->CreatePixelShader(
          pixel_code[i]->GetBufferPointer(), pixel_code[i]->GetBufferSize(), &pixel_shaders[i]
      ));
    D3D10_BUFFER_DESC constant_desc{
        D3D10_COMMONSHADER_CONSTANT_BUFFER_COMPONENTS * sizeof(UINT), D3D10_USAGE_DEFAULT, D3D10_BIND_CONSTANT_BUFFER
    };
    ComPtr<ID3D10Buffer> constants;
    CHECK(device->CreateBuffer(&constant_desc, nullptr, &constants));
    D3D10_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
    D3D10_RECT scissor{0, 0, 1, 1};
    device->RSSetViewports(1, &viewport);
    device->RSSetScissorRects(1, &scissor);
    D3D10_RASTERIZER_DESC raster_desc{D3D10_FILL_SOLID, D3D10_CULL_NONE};
    raster_desc.ScissorEnable = TRUE;
    ComPtr<ID3D10RasterizerState> raster;
    CHECK(device->CreateRasterizerState(&raster_desc, &raster));
    device->RSSetState(raster.Get());
    D3D10_BLEND_DESC blend_desc{};
    blend_desc.SrcBlend = blend_desc.SrcBlendAlpha = D3D10_BLEND_ONE;
    blend_desc.DestBlend = blend_desc.DestBlendAlpha = D3D10_BLEND_ZERO;
    blend_desc.BlendOp = blend_desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
    ComPtr<ID3D10BlendState> blend;
    CHECK(device->CreateBlendState(&blend_desc, &blend));
    device->OMSetBlendState(blend.Get(), nullptr, 0);

    UINT support;
    CHECK(device->CheckFormatSupport(DXGI_FORMAT_R32_FLOAT, &support));
    const bool mip_generation = support & D3D10_FORMAT_SUPPORT_MIP_AUTOGEN;
    auto owner = [&](ID3D10View *view, ID3D10Resource *resource) {
      ComPtr<ID3D10Resource> of;
      ComPtr<ID3D10Device> creator;
      view->GetResource(&of);
      view->GetDevice(&creator);
      expect(of.Get() == resource && creator.Get() == device.Get(), "view resource or creating device identity");
    };
    for (auto dimension :
         {D3D10_RESOURCE_DIMENSION_TEXTURE1D, D3D10_RESOURCE_DIMENSION_TEXTURE2D, D3D10_RESOURCE_DIMENSION_TEXTURE3D}) {
      for (UINT arrays : {1u, 3u}) {
        if (dimension == D3D10_RESOURCE_DIMENSION_TEXTURE3D && arrays != 1)
          continue;
        const UINT rows = dimension == D3D10_RESOURCE_DIMENSION_TEXTURE1D ? 1 : height;
        const UINT planes = dimension == D3D10_RESOURCE_DIMENSION_TEXTURE3D ? depth : 1;
        std::vector<std::vector<float>> original(arrays * mips);
        std::vector<D3D10_SUBRESOURCE_DATA> initial(original.size());
        for (UINT sub = 0; sub < original.size(); sub++) {
          UINT mip = sub % mips, w = std::max(1u, width >> mip), h = std::max(1u, rows >> mip);
          UINT d = std::max(1u, planes >> mip);
          original[sub].resize(w * h * d);
          for (UINT i = 0; i < original[sub].size(); i++)
            original[sub][i] = float(sub * width * height * depth + i + 1);
          initial[sub] = {original[sub].data(), w * word_bytes, w * h * word_bytes};
        }
        auto resource = [&](bool staging) -> ComPtr<ID3D10Resource> {
          auto usage = staging ? D3D10_USAGE_STAGING : D3D10_USAGE_DEFAULT;
          UINT bind = staging ? 0 : D3D10_BIND_SHADER_RESOURCE | D3D10_BIND_RENDER_TARGET;
          UINT access = staging ? D3D10_CPU_ACCESS_READ : 0;
          UINT misc = !staging && mip_generation ? D3D10_RESOURCE_MISC_GENERATE_MIPS : 0;
          ComPtr<ID3D10Resource> result;
          HRESULT hr;
          if (dimension == D3D10_RESOURCE_DIMENSION_TEXTURE1D) {
            D3D10_TEXTURE1D_DESC desc{width, mips, arrays, DXGI_FORMAT_R32_FLOAT, usage, bind, access, misc};
            ComPtr<ID3D10Texture1D> texture;
            hr = device->CreateTexture1D(&desc, staging ? nullptr : initial.data(), &texture);
            result = texture;
          } else if (dimension == D3D10_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D10_TEXTURE2D_DESC desc{width,  rows,  mips, arrays, DXGI_FORMAT_R32_FLOAT,
                                      {1, 0}, usage, bind, access, misc};
            ComPtr<ID3D10Texture2D> texture;
            hr = device->CreateTexture2D(&desc, staging ? nullptr : initial.data(), &texture);
            result = texture;
          } else {
            D3D10_TEXTURE3D_DESC desc{width, rows, planes, mips, DXGI_FORMAT_R32_FLOAT, usage, bind, access, misc};
            ComPtr<ID3D10Texture3D> texture;
            hr = device->CreateTexture3D(&desc, staging ? nullptr : initial.data(), &texture);
            result = texture;
          }
          expect(hr == S_OK && result, "CreateTexture returned %#lx", hr);
          return result;
        };
        auto texture = resource(false), staging = resource(true);
        if (!texture || !staging)
          return verdict();
        auto check_words = [&](const std::vector<std::vector<float>> &expected) {
          device->CopyResource(staging.Get(), texture.Get());
          for (UINT sub = 0; sub < original.size(); sub++) {
            UINT source_mip = sub % mips;
            UINT w = std::max(1u, width >> source_mip), h = std::max(1u, rows >> source_mip);
            UINT d = std::max(1u, planes >> source_mip);
            void *data = nullptr;
            UINT row_pitch = w * sizeof(float), depth_pitch = h * row_pitch;
            HRESULT hr;
            if (dimension == D3D10_RESOURCE_DIMENSION_TEXTURE1D)
              hr = static_cast<ID3D10Texture1D *>(staging.Get())->Map(sub, D3D10_MAP_READ, 0, &data);
            else if (dimension == D3D10_RESOURCE_DIMENSION_TEXTURE2D) {
              D3D10_MAPPED_TEXTURE2D mapped{};
              hr = static_cast<ID3D10Texture2D *>(staging.Get())->Map(sub, D3D10_MAP_READ, 0, &mapped);
              data = mapped.pData;
              row_pitch = mapped.RowPitch;
            } else {
              D3D10_MAPPED_TEXTURE3D mapped{};
              hr = static_cast<ID3D10Texture3D *>(staging.Get())->Map(sub, D3D10_MAP_READ, 0, &mapped);
              data = mapped.pData;
              row_pitch = mapped.RowPitch;
              depth_pitch = mapped.DepthPitch;
            }
            if (!expect(hr == S_OK && data, "Map returned %#lx", hr))
              return false;
            bool same = true;
            for (UINT z = 0; z < d; z++) {
              for (UINT y = 0; y < h; y++) {
                auto row =
                    reinterpret_cast<const float *>(static_cast<const char *>(data) + z * depth_pitch + y * row_pitch);
                for (UINT x = 0; x < w; x++)
                  same &= row[x] == expected[sub][(z * h + y) * w + x];
              }
            }
            expect(same, "resource words differ in subresource %u", sub);
            if (dimension == D3D10_RESOURCE_DIMENSION_TEXTURE1D)
              static_cast<ID3D10Texture1D *>(staging.Get())->Unmap(sub);
            else if (dimension == D3D10_RESOURCE_DIMENSION_TEXTURE2D)
              static_cast<ID3D10Texture2D *>(staging.Get())->Unmap(sub);
            else
              static_cast<ID3D10Texture3D *>(staging.Get())->Unmap(sub);
          }
          return true;
        };
        for (UINT mip = 0; mip < mips; mip++) {
          UINT slices = dimension == D3D10_RESOURCE_DIMENSION_TEXTURE3D ? std::max(1u, planes >> mip) : arrays;
          for (UINT first = 0; first < slices; first++) {
            for (UINT count = 1; count <= slices - first; count++) {
              for (bool remainder : {false, true}) {
                if (remainder && count != slices - first)
                  continue;
                step(
                    "level %#x dimension %u arrays %u mip %u slices %u+%u "
                    "rest %u",
                    level, dimension, arrays, mip, first, count, remainder
                );
                D3D10_RENDER_TARGET_VIEW_DESC desc{DXGI_FORMAT_R32_FLOAT};
                UINT extent = remainder ? rest : count;
                if (dimension == D3D10_RESOURCE_DIMENSION_TEXTURE1D) {
                  desc.ViewDimension = D3D10_RTV_DIMENSION_TEXTURE1DARRAY;
                  desc.Texture1DArray = {mip, first, extent};
                } else if (dimension == D3D10_RESOURCE_DIMENSION_TEXTURE2D) {
                  desc.ViewDimension = D3D10_RTV_DIMENSION_TEXTURE2DARRAY;
                  desc.Texture2DArray = {mip, first, extent};
                } else {
                  desc.ViewDimension = D3D10_RTV_DIMENSION_TEXTURE3D;
                  desc.Texture3D = {mip, first, extent};
                }
                ComPtr<ID3D10RenderTargetView> view;
                expect(
                    device->CreateRenderTargetView(texture.Get(), &desc, nullptr) == S_FALSE,
                    "valid view validation-only HRESULT"
                );
                CHECK(device->CreateRenderTargetView(texture.Get(), &desc, &view));
                owner(view.Get(), texture.Get());
                D3D10_RENDER_TARGET_VIEW_DESC got{};
                view->GetDesc(&got);
                expect(
                    got.Format == desc.Format && got.ViewDimension == desc.ViewDimension &&
                        got.Texture3D.MipSlice == mip && got.Texture3D.FirstWSlice == first &&
                        got.Texture3D.WSize == count,
                    "RTV descriptor fields"
                );
                for (UINT sub = 0; sub < original.size(); sub++)
                  device->UpdateSubresource(
                      texture.Get(), sub, nullptr, initial[sub].pSysMem, initial[sub].SysMemPitch,
                      initial[sub].SysMemSlicePitch
                  );
                device->RSSetState(raster.Get());
                device->RSSetViewports(1, &viewport);
                device->OMSetBlendState(blend.Get(), nullptr, 0);
                device->ClearRenderTargetView(view.Get(), clear);
                auto expected = original;
                for (UINT sub = 0; sub < expected.size(); sub++) {
                  if (sub % mips != mip)
                    continue;
                  UINT w = std::max(1u, width >> mip), h = std::max(1u, rows >> mip);
                  UINT d = std::max(1u, planes >> mip);
                  for (UINT z = 0; z < d; z++) {
                    UINT slice = dimension == D3D10_RESOURCE_DIMENSION_TEXTURE3D ? z : sub / mips;
                    if (slice >= first && slice < first + count)
                      std::fill_n(expected[sub].begin() + z * w * h, w * h, clear[0]);
                  }
                }
                if (!check_words(expected))
                  return verdict();
              }
            }
          }
          for (UINT levels = 1; levels <= mips - mip; levels++) {
            for (UINT first = 0; first < arrays; first++) {
              for (UINT count = 1; count <= arrays - first; count++) {
                D3D10_SHADER_RESOURCE_VIEW_DESC desc{DXGI_FORMAT_R32_FLOAT};
                if (dimension == D3D10_RESOURCE_DIMENSION_TEXTURE1D) {
                  desc.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE1DARRAY;
                  desc.Texture1DArray = {mip, levels, first, count};
                } else if (dimension == D3D10_RESOURCE_DIMENSION_TEXTURE2D) {
                  desc.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2DARRAY;
                  desc.Texture2DArray = {mip, levels, first, count};
                } else {
                  desc.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE3D;
                  desc.Texture3D = {mip, levels};
                }
                ComPtr<ID3D10ShaderResourceView> view;
                expect(
                    device->CreateShaderResourceView(texture.Get(), &desc, nullptr) == S_FALSE,
                    "SRV validation-only HRESULT"
                );
                CHECK(device->CreateShaderResourceView(texture.Get(), &desc, &view));
                owner(view.Get(), texture.Get());
                D3D10_SHADER_RESOURCE_VIEW_DESC got{};
                view->GetDesc(&got);
                expect(
                    got.Format == desc.Format && got.ViewDimension == desc.ViewDimension &&
                        got.Texture3D.MostDetailedMip == mip && got.Texture3D.MipLevels == levels,
                    "SRV mip fields"
                );
                if (dimension != D3D10_RESOURCE_DIMENSION_TEXTURE3D)
                  expect(
                      got.Texture2DArray.FirstArraySlice == first && got.Texture2DArray.ArraySize == count,
                      "SRV array fields"
                  );
                for (bool generated : {false, true}) {
                  if (generated && !mip_generation)
                    continue;
                  step(
                      "SRV dimension %u mip %u levels %u array %u+%u "
                      "generated %u",
                      dimension, mip, levels, first, count, generated
                  );
                  auto images = original;
                  if (generated) {
                    for (UINT array = first; array < first + count; array++) {
                      float value = original[array * mips + mip][0];
                      for (UINT level = mip; level < mip + levels; level++)
                        std::fill(images[array * mips + level].begin(), images[array * mips + level].end(), value);
                    }
                  }
                  for (UINT sub = 0; sub < original.size(); sub++) {
                    const auto &image = generated && sub % mips == mip ? images[sub] : original[sub];
                    device->UpdateSubresource(
                        texture.Get(), sub, nullptr, image.data(), initial[sub].SysMemPitch,
                        initial[sub].SysMemSlicePitch
                    );
                  }
                  if (generated)
                    device->GenerateMips(view.Get());
                  if (!check_words(images))
                    return verdict();
                  for (UINT relative = 0; relative <= levels; relative++) {
                    const UINT actual_mip = mip + relative;
                    const UINT w = std::max(1u, width >> actual_mip), h = std::max(1u, rows >> actual_mip);
                    const UINT d =
                        dimension == D3D10_RESOURCE_DIMENSION_TEXTURE3D ? std::max(1u, planes >> actual_mip) : count;
                    D3D10_TEXTURE2D_DESC target_desc{w + 1,
                                                     h * d + 1,
                                                     1,
                                                     1,
                                                     DXGI_FORMAT_R32_FLOAT,
                                                     {1, 0},
                                                     D3D10_USAGE_DEFAULT,
                                                     D3D10_BIND_RENDER_TARGET};
                    ComPtr<ID3D10Texture2D> target, readback;
                    ComPtr<ID3D10RenderTargetView> target_view;
                    CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
                    CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &target_view));
                    target_desc.Usage = D3D10_USAGE_STAGING;
                    target_desc.BindFlags = 0;
                    target_desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
                    CHECK(device->CreateTexture2D(&target_desc, nullptr, &readback));
                    UINT parameters[D3D10_COMMONSHADER_CONSTANT_BUFFER_COMPONENTS] = {relative, h};
                    device->UpdateSubresource(constants.Get(), 0, nullptr, parameters, 0, 0);
                    D3D10_VIEWPORT load_viewport{0, 0, target_desc.Width, target_desc.Height, 0, 1};
                    device->RSSetViewports(1, &load_viewport);
                    device->RSSetState(nullptr);
                    device->OMSetBlendState(nullptr, nullptr, D3D10_DEFAULT_SAMPLE_MASK);
                    device->OMSetRenderTargets(1, target_view.GetAddressOf(), nullptr);
                    device->VSSetShader(vertex_shader.Get());
                    device->PSSetShader(pixel_shaders[dimension - D3D10_RESOURCE_DIMENSION_TEXTURE1D].Get());
                    device->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
                    device->PSSetShaderResources(0, 1, view.GetAddressOf());
                    device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    device->Draw(3, 0);
                    device->CopyResource(readback.Get(), target.Get());
                    D3D10_MAPPED_TEXTURE2D mapped{};
                    CHECK(readback->Map(0, D3D10_MAP_READ, 0, &mapped));
                    bool same = true;
                    for (UINT y = 0; y < target_desc.Height; y++) {
                      auto pixels = reinterpret_cast<const float *>(
                          static_cast<const char *>(mapped.pData) + y * mapped.RowPitch
                      );
                      for (UINT x = 0; x < target_desc.Width; x++) {
                        float expected = 0;
                        if (relative < levels && x < w && y < h * d) {
                          UINT array = dimension == D3D10_RESOURCE_DIMENSION_TEXTURE3D ? 0 : first + y / h;
                          UINT z = dimension == D3D10_RESOURCE_DIMENSION_TEXTURE3D ? y / h : 0;
                          expected = images[array * mips + actual_mip][(z * h + y % h) * w + x];
                        }
                        same &= pixels[x] == expected;
                      }
                    }
                    expect(
                        same, "SRV dimension %u mip %u+%u array %u+%u load pixels", dimension, mip, relative, first,
                        count
                    );
                    readback->Unmap(0);
                    device->PSSetShaderResources(0, 1, nullptr);
                    device->OMSetRenderTargets(0, nullptr, nullptr);
                  }
                }
              }
            }
          }
        }
        device->RSSetState(raster.Get());
        device->RSSetViewports(1, &viewport);
        device->OMSetBlendState(blend.Get(), nullptr, 0);
      }
    }
    if (level == D3D10_FEATURE_LEVEL_10_1) {
      const UINT faces = D3D10_TEXTURECUBE_FACE_NEGATIVE_Z + 1, cubes = 2;
      D3D10_TEXTURE2D_DESC desc{
          width,
          width,
          mips,
          faces * cubes,
          DXGI_FORMAT_R32_FLOAT,
          {1, 0},
          D3D10_USAGE_DEFAULT,
          D3D10_BIND_SHADER_RESOURCE,
          0,
          D3D10_RESOURCE_MISC_TEXTURECUBE
      };
      ComPtr<ID3D10Texture2D> texture;
      CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
      for (UINT mip = 0; mip < mips; mip++) {
        for (UINT first = 0; first < cubes; first++) {
          for (UINT count = 1; count <= cubes - first; count++) {
            step("D3D10.1 cube array mip %u first cube %u count %u", mip, first, count);
            D3D10_SHADER_RESOURCE_VIEW_DESC1 requested{desc.Format, D3D10_1_SRV_DIMENSION_TEXTURECUBEARRAY};
            requested.TextureCubeArray = {mip, mips - mip, first * faces, count};
            ComPtr<ID3D10ShaderResourceView1> view;
            CHECK(device->CreateShaderResourceView1(texture.Get(), &requested, &view));
            owner(view.Get(), texture.Get());
            D3D10_SHADER_RESOURCE_VIEW_DESC1 got{};
            view->GetDesc1(&got);
            expect(
                got.Format == requested.Format && got.ViewDimension == requested.ViewDimension &&
                    got.TextureCubeArray.MostDetailedMip == mip && got.TextureCubeArray.MipLevels == mips - mip &&
                    got.TextureCubeArray.First2DArrayFace == first * faces && got.TextureCubeArray.NumCubes == count,
                "cube array descriptor fields"
            );
          }
        }
      }
    }
    device->ClearState();
  }
  return verdict();
}
