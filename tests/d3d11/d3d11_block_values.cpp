// contract: BC decoding precedes filtering, follows the encoded palettes, and
// uses the mip's virtual dimensions. "Decompression always occurs before
// filtering." (D3D11.3 19.5.1). "Values that the reference decodes to 0.0, 1.0
// or -1.0 must always be exact." (19.5.2). BC1-5 error bounds come from 19.5.2
// and 19.5.6-.12; BC6H/BC7 "decompression hardware is required to be bit
// accurate" (19.5.13/.14). endpoint words/pairs span 16 bits, every block
// position and selector is sampled, including 3x2, 2x2 and 1x1 virtual mips.
// BC6H covers zero endpoints in all legal/reserved modes, then all four ONE
// modes' nonzero endpoints, signed extremes, delta wrap, every index and signed 16.4's -INF
// (19.5.13.3/.5-.10; Learn's BC6H Format has the complete signed unquantize).
// BC7 covers zero
// endpoints in every mode and varying mode-6 endpoints/P-bits/indices.
// encoded bytes survive a staging copy: "Copy the entire contents of the source resource to the destination
// resource using the GPU." (Microsoft Learn, ID3D11DeviceContext::CopyResource).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/bc6h-format
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/bc7-format
#include "d3d11_test.hpp"
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
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate
  ));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps[std::size(pixels)];
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs));
  for (UINT mode = 0; mode < std::size(ps); mode++) {
    CHECK(
        device->CreatePixelShader(pixels[mode]->GetBufferPointer(), pixels[mode]->GetBufferSize(), nullptr, &ps[mode])
    );
  }
  D3D11_SAMPLER_DESC sd{
      D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
      D3D11_TEXTURE_ADDRESS_CLAMP
  };
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  ComPtr<ID3D11SamplerState> sampler;
  CHECK(device->CreateSamplerState(&sd, &sampler));
  D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  rd.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&rd, &raster));
  UINT tested = 0;
  for (auto f : formats())
    for (bool parent : {false, true})
      for (UINT width : {side * (1u << CHAR_BIT), side * 3}) {
        const UINT height = width == side * 3 ? side * 2 : width, mips = std::bit_width(width | height);
        UINT caps = 0;
        HRESULT hr = device->CheckFormatSupport(f.id, &caps);
        step("%s parent %u: %ux%u, %u mips", f.name, parent, width, height, mips);
        std::vector<std::vector<uint8_t>> bytes(mips);
        std::vector<D3D11_SUBRESOURCE_DATA> data(mips);
        for (UINT mip = 0; mip < mips; mip++) {
          UINT wide = std::max(1u, width >> mip), high = std::max(1u, height >> mip);
          bytes[mip] = image(f, wide, high, mip);
          data[mip] = {bytes[mip].data(), (wide + side - 1) / side * f.bytes};
        }
        D3D11_TEXTURE2D_DESC desc{
            width, height, mips, 1, parent ? f.parent : f.id, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE
        };
        ComPtr<ID3D11Texture2D> texture, copied;
        HRESULT created = device->CreateTexture2D(&desc, data.data(), &texture);
        if (FAILED(hr) || !(caps & D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
          expect(FAILED(created) && !texture, "unsupported BC storage was created, hr %08lx", created);
          continue;
        }
        if (!expect(created == S_OK && texture, "advertised BC storage failed: %08lx", created))
          return verdict();
        CHECK(device->CreateTexture2D(&desc, nullptr, &copied));
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{f.id, D3D11_SRV_DIMENSION_TEXTURE2D};
        vd.Texture2D.MipLevels = mips;
        ComPtr<ID3D11ShaderResourceView> views[2];
        CHECK(device->CreateShaderResourceView(texture.Get(), &vd, &views[0]));
        CHECK(device->CreateShaderResourceView(copied.Get(), &vd, &views[1]));
        for (bool listed : {false, true})
          for (UINT mip = 0; mip < mips; mip++)
            for (UINT mode = 0; mode < std::size(ps); mode++) {
              if (!(caps & (mode ? D3D11_FORMAT_SUPPORT_SHADER_SAMPLE : D3D11_FORMAT_SUPPORT_SHADER_LOAD)))
                continue;
              UINT wide = std::max(1u, width >> mip), high = std::max(1u, height >> mip);
              step(
                  "%s parent %u %s, mip %u (%ux%u), %s", f.name, parent, listed ? "deferred" : "immediate", mip, wide,
                  high, mode ? "point sample" : "load after copy"
              );
              auto context = listed ? deferred.Get() : immediate.Get();
              context->ClearState();
              if (!mode)
                context->CopyResource(copied.Get(), texture.Get());
              D3D11_TEXTURE2D_DESC out_desc{wide,
                                            high,
                                            1,
                                            1,
                                            DXGI_FORMAT_R32G32B32A32_FLOAT,
                                            {1, 0},
                                            D3D11_USAGE_DEFAULT,
                                            D3D11_BIND_RENDER_TARGET};
              ComPtr<ID3D11Texture2D> target, staging;
              ComPtr<ID3D11RenderTargetView> rtv;
              CHECK(device->CreateTexture2D(&out_desc, nullptr, &target));
              CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
              out_desc.Usage = D3D11_USAGE_STAGING;
              out_desc.BindFlags = 0;
              out_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
              CHECK(device->CreateTexture2D(&out_desc, nullptr, &staging));
              const UINT shape[4] = {wide, high, mip};
              D3D11_BUFFER_DESC bd{sizeof(shape), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER};
              D3D11_SUBRESOURCE_DATA bd_data{shape};
              ComPtr<ID3D11Buffer> constants;
              CHECK(device->CreateBuffer(&bd, &bd_data, &constants));
              context->VSSetShader(vs.Get(), nullptr, 0);
              context->PSSetShader(ps[mode].Get(), nullptr, 0);
              context->PSSetShaderResources(0, 1, views[mode ? 0 : 1].GetAddressOf());
              context->PSSetSamplers(0, 1, sampler.GetAddressOf());
              context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
              context->RSSetState(raster.Get());
              D3D11_VIEWPORT viewport{0, 0, float(wide), float(high), 0, 1};
              context->RSSetViewports(1, &viewport);
              context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
              const float poison[] = {-1, -1, -1, -1};
              context->ClearRenderTargetView(rtv.Get(), poison);
              context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
              context->Draw(3, 0);
              context->ClearState();
              context->CopyResource(staging.Get(), target.Get());
              if (listed) {
                ComPtr<ID3D11CommandList> commands;
                CHECK(deferred->FinishCommandList(FALSE, &commands));
                immediate->ExecuteCommandList(commands.Get(), FALSE);
              }
              D3D11_MAPPED_SUBRESOURCE mapped{};
              CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
              bool same = true;
              for (UINT y = 0; same && y < high; y++)
                for (UINT x = 0; x < wide; x++) {
                  const auto *pixel = reinterpret_cast<const float *>(
                                          static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch
                                      ) +
                                      x * 4;
                  if (!matches(f, bytes[mip].data(), wide, x, y, pixel)) {
                    const auto block = bytes[mip].data() +
                                       ((y / side) * ((wide + side - 1) / side) + x / side) * f.bytes;
                    const auto want = decode(f, block, (y % side) * side + x % side).value;
                    expect(false, "texel %u,%u is %.9g %.9g %.9g %.9g, want %.9g %.9g %.9g %.9g", x, y,
                           pixel[0], pixel[1], pixel[2], pixel[3], want[0], want[1], want[2], want[3]);
                    same = false;
                    break;
                  }
                }
              immediate->Unmap(staging.Get(), 0);
              tested++;
            }
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> encoded;
        CHECK(device->CreateTexture2D(&desc, nullptr, &encoded));
        immediate->CopyResource(encoded.Get(), texture.Get());
        for (UINT mip = 0; mip < mips; mip++) {
          const UINT wide = std::max(1u, width >> mip), high = std::max(1u, height >> mip);
          const UINT row_bytes = (wide + side - 1) / side * f.bytes;
          D3D11_MAPPED_SUBRESOURCE mapped{};
          CHECK(immediate->Map(encoded.Get(), mip, D3D11_MAP_READ, 0, &mapped));
          for (UINT row = 0; row < (high + side - 1) / side; row++)
            if (!expect(!memcmp(static_cast<const uint8_t *>(mapped.pData) + row * mapped.RowPitch,
                                bytes[mip].data() + row * row_bytes, row_bytes),
                        "encoded mip %u row %u changed", mip, row))
              break;
          immediate->Unmap(encoded.Get(), mip);
        }
      }
  expect(tested != 0, "no BC values were sampled");
  return verdict();
}
