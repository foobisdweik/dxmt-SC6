// contract: native Direct3D 10/10.1 BC storage, typed/typeless SRVs and copies preserve decoded values.
// "Decompression always occurs before filtering." (D3D11.3 19.5.1). "Values that the reference decodes to
// 0.0, 1.0 or -1.0 must always be exact." (19.5.2). BC1-5 endpoint words/pairs and every 4x4 selector use
// the 19.5.2/.6-.12 error bounds. supplied mips include 3x2, 2x2 and 1x1 virtual sizes in physical blocks.
// newer BC formats run only when advertised, using the bit-accurate 19.5.13/.14 reference shared with
// the other APIs. shader-model 4.0 Load and point SampleLevel both produce readable float32 targets.
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include "../depth_formats.hpp"
#include "../block_values.hpp"

using namespace block_values;

int
main() {
  auto vertex = compile(hlsl, "vs", "vs_4_0", {"POINT_SAMPLE=0"});
  ComPtr<ID3DBlob> pixels[2];
  for (UINT mode = 0; mode < std::size(pixels); mode++)
    pixels[mode] = compile(hlsl, "ps", "ps_4_0", {"POINT_SAMPLE=" + std::to_string(mode)});
  if (!expect(vertex && pixels[0] && pixels[1], "block HLSL did not compile"))
    return verdict();
  HMODULE dll = LoadLibraryA("d3d10_1.dll");
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      dll ? (void *)GetProcAddress(dll, "D3D10CreateDevice1") : nullptr
  );
  if (!expect(create != nullptr, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  UINT tested = 0;
  for (auto level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, level, D3D10_1_SDK_VERSION, &device));
    ComPtr<ID3D10VertexShader> vs;
    ComPtr<ID3D10PixelShader> ps[std::size(pixels)];
    CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), &vs));
    for (UINT mode = 0; mode < std::size(ps); mode++) {
      CHECK(device->CreatePixelShader(pixels[mode]->GetBufferPointer(), pixels[mode]->GetBufferSize(), &ps[mode]));
    }
    D3D10_SAMPLER_DESC sd{
        D3D10_FILTER_MIN_MAG_MIP_POINT, D3D10_TEXTURE_ADDRESS_CLAMP, D3D10_TEXTURE_ADDRESS_CLAMP,
        D3D10_TEXTURE_ADDRESS_CLAMP
    };
    sd.MaxLOD = D3D10_FLOAT32_MAX;
    ComPtr<ID3D10SamplerState> sampler;
    CHECK(device->CreateSamplerState(&sd, &sampler));
    D3D10_RASTERIZER_DESC rd{D3D10_FILL_SOLID, D3D10_CULL_NONE};
    rd.DepthClipEnable = TRUE;
    ComPtr<ID3D10RasterizerState> raster;
    CHECK(device->CreateRasterizerState(&rd, &raster));
    for (auto f : formats())
      for (bool parent : {false, true})
        for (UINT width : {side * (1u << CHAR_BIT), side * 3}) {
          const UINT height = width == side * 3 ? side * 2 : width, mips = std::bit_width(width | height);
          UINT caps = 0;
          HRESULT hr = device->CheckFormatSupport(parent ? f.parent : f.id, &caps);
          UINT view_caps = 0;
          device->CheckFormatSupport(f.id, &view_caps);
          step("%s parent %u: %ux%u, %u mips", f.name, parent, width, height, mips);
          std::vector<std::vector<uint8_t>> bytes(mips);
          std::vector<D3D10_SUBRESOURCE_DATA> data(mips);
          for (UINT mip = 0; mip < mips; mip++) {
            UINT wide = std::max(1u, width >> mip), high = std::max(1u, height >> mip);
            bytes[mip] = image(f, wide, high, mip);
            data[mip] = {bytes[mip].data(), (wide + side - 1) / side * f.bytes};
          }
          D3D10_TEXTURE2D_DESC desc{width,
                                    height,
                                    mips,
                                    1,
                                    parent ? f.parent : f.id,
                                    {1, 0},
                                    D3D10_USAGE_DEFAULT,
                                    D3D10_BIND_SHADER_RESOURCE};
          ComPtr<ID3D10Texture2D> texture, copied;
          HRESULT created = device->CreateTexture2D(&desc, data.data(), &texture);
          if (FAILED(hr) || !(caps & D3D10_FORMAT_SUPPORT_TEXTURE2D)) {
            expect(FAILED(created) && !texture, "unsupported BC storage was created, hr %08lx", created);
            continue;
          }
          if (!expect(created == S_OK && texture, "advertised BC storage failed: %08lx", created))
            return verdict();
          CHECK(device->CreateTexture2D(&desc, nullptr, &copied));
          D3D10_SHADER_RESOURCE_VIEW_DESC vd{f.id, D3D10_SRV_DIMENSION_TEXTURE2D};
          vd.Texture2D.MipLevels = mips;
          ComPtr<ID3D10ShaderResourceView> views[2];
          CHECK(device->CreateShaderResourceView(texture.Get(), &vd, &views[0]));
          CHECK(device->CreateShaderResourceView(copied.Get(), &vd, &views[1]));
          for (UINT mip = 0; mip < mips; mip++)
            for (UINT mode = 0; mode < std::size(ps); mode++) {
              if (!(view_caps & (mode ? D3D10_FORMAT_SUPPORT_SHADER_SAMPLE : D3D10_FORMAT_SUPPORT_SHADER_LOAD)))
                continue;
              UINT wide = std::max(1u, width >> mip), high = std::max(1u, height >> mip);
              step(
                  "D3D10 level %#x %s parent %u, mip %u (%ux%u), %s", level, f.name, parent, mip, wide, high,
                  mode ? "point sample" : "load after copy"
              );
              device->ClearState();
              if (!mode)
                device->CopyResource(copied.Get(), texture.Get());
              D3D10_TEXTURE2D_DESC out_desc{wide,
                                            high,
                                            1,
                                            1,
                                            DXGI_FORMAT_R32G32B32A32_FLOAT,
                                            {1, 0},
                                            D3D10_USAGE_DEFAULT,
                                            D3D10_BIND_RENDER_TARGET};
              ComPtr<ID3D10Texture2D> target, staging;
              ComPtr<ID3D10RenderTargetView> rtv;
              CHECK(device->CreateTexture2D(&out_desc, nullptr, &target));
              CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
              out_desc.Usage = D3D10_USAGE_STAGING;
              out_desc.BindFlags = 0;
              out_desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
              CHECK(device->CreateTexture2D(&out_desc, nullptr, &staging));
              const UINT shape[4] = {wide, high, mip};
              D3D10_BUFFER_DESC bd{sizeof(shape), D3D10_USAGE_IMMUTABLE, D3D10_BIND_CONSTANT_BUFFER};
              D3D10_SUBRESOURCE_DATA bd_data{shape};
              ComPtr<ID3D10Buffer> constants;
              CHECK(device->CreateBuffer(&bd, &bd_data, &constants));
              device->VSSetShader(vs.Get());
              device->PSSetShader(ps[mode].Get());
              device->PSSetShaderResources(0, 1, views[mode ? 0 : 1].GetAddressOf());
              device->PSSetSamplers(0, 1, sampler.GetAddressOf());
              device->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
              device->RSSetState(raster.Get());
              D3D10_VIEWPORT viewport{0, 0, wide, high, 0, 1};
              device->RSSetViewports(1, &viewport);
              device->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
              const float poison[] = {-1, -1, -1, -1};
              device->ClearRenderTargetView(rtv.Get(), poison);
              device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
              device->Draw(3, 0);
              device->ClearState();
              device->CopyResource(staging.Get(), target.Get());
              D3D10_MAPPED_TEXTURE2D mapped{};
              CHECK(staging->Map(0, D3D10_MAP_READ, 0, &mapped));
              bool same = true;
              for (UINT y = 0; same && y < high; y++)
                for (UINT x = 0; x < wide; x++) {
                  const auto *pixel = reinterpret_cast<const float *>(
                                          static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch
                                      ) +
                                      x * 4;
                  if (!matches(f, bytes[mip].data(), wide, x, y, pixel)) {
                    expect(false, "texel %u,%u is %.9g %.9g %.9g %.9g", x, y, pixel[0], pixel[1], pixel[2], pixel[3]);
                    same = false;
                    break;
                  }
                }
              staging->Unmap(0);
              tested++;
              if (!same)
                return verdict();
            }
        }
  }
  FreeLibrary(dll);
  expect(tested != 0, "no D3D10 BC values were sampled");
  return verdict();
}
