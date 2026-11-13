// contract: copies preserve bits within a type group and the D3D10.1 reinterpret pairs, and refuse other pairs.
// "but the raw value of memory happens to be equal" (D3D11.3 5.6.2, 5.6.3). Microsoft Learn, Format Conversion
// Using Direct3D 10.1: "Direct3D 10.1 enables copies between prestructured-typed textures and block-compressed
// textures of the same bit widths." its table permits R16G16B16A16/R32G32 with 64-bit blocks, R32G32B32A32 with
// 128-bit blocks, and R32 with R9G9B9E5. Wine's test_format_compatibility and test_compressed_format_compatibility
// witness refusal on Windows, also on feature level 10_0. format names and widths come from the SDK, not Mullion.
// X is "Unused bits in format" (D3D11.3 19.1.3.1), so only defined components are compared. depth copies use the
// whole subresource (Microsoft Learn, CopySubresourceRegion, Remarks: "you must copy the whole subresource").
// an initial-data control distinguishes storage loss from copy refusal.
// initial data retains every component: "later components are still located at larger addresses than earlier
// components." (D3D11.3 19.1.3.1).
// ID3D11Device::CreateTexture2D: "To load data into the texture, applications can supply the data initially".
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createtexture2d
#include "d3d11_test.hpp"
#include "../copy_formats.hpp"
#include <algorithm>
#include <bit>

int main() {
  for (auto level : {D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_11_0}) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context, deferred;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                            nullptr, &context));
    CHECK(device->CreateDeferredContext(0, &deferred));
    for (const char *family : {"A8", "R32", "R32G32B32A32", "R32G32B32"})
      for (const auto &format : copy_formats) {
        if (format.depth || strcmp(format.family, family))
          continue;
        for (UINT width : {5u, 1u})
          for (UINT height : {3u, 1u})
            for (UINT padding : {0u, UINT(sizeof(float))})
              for (bool mipmapped : {false, true})
                for (UINT slices : {1u, 2u}) {
                  const UINT mips = mipmapped ? std::bit_width(width | height) : 1;
                  std::vector<std::vector<uint8_t>> bytes(mips * slices), poison(mips * slices);
                  std::vector<D3D11_SUBRESOURCE_DATA> data(mips * slices), blank(mips * slices);
                  for (UINT slice = 0; slice < slices; slice++)
                    for (UINT mip = 0; mip < mips; mip++) {
                      const UINT sub = D3D11CalcSubresource(mip, slice, mips);
                      const UINT row_bytes = std::max(1u, width >> mip) * format.bytes;
                      const UINT pitch = row_bytes + padding;
                      bytes[sub].resize(pitch * std::max(1u, height >> mip));
                      poison[sub].resize(bytes[sub].size());
                      for (UINT i = 0; i < bytes[sub].size(); i++) {
                        bytes[sub][i] = uint8_t(i * 13 + 201 + sub);
                        poison[sub][i] = uint8_t(~bytes[sub][i]);
                      }
                      data[sub] = {bytes[sub].data(), pitch};
                      blank[sub] = {poison[sub].data(), pitch};
                    }
                  D3D11_TEXTURE2D_DESC desc{width, height, mips, slices, format.id, {1, 0}, D3D11_USAGE_STAGING,
                                           0, D3D11_CPU_ACCESS_READ};
                  ComPtr<ID3D11Texture2D> staging;
                  step("initial data %s, %ux%u, padding %u, %u mips, %u slices: staging control", format.name,
                       width, height, padding, mips, slices);
                  CHECK(device->CreateTexture2D(&desc, data.data(), &staging));
                  for (auto usage : {D3D11_USAGE_STAGING, D3D11_USAGE_DEFAULT, D3D11_USAGE_IMMUTABLE}) {
                    ComPtr<ID3D11Texture2D> texture;
                    if (usage != D3D11_USAGE_STAGING) {
                      desc.Usage = usage;
                      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                      desc.CPUAccessFlags = 0;
                      CHECK(device->CreateTexture2D(&desc, data.data(), &texture));
                    }
                    for (bool listed : {false, true}) {
                      if (usage == D3D11_USAGE_STAGING && listed)
                        continue;
                      step("initial data FL %x, %s, %ux%u, padding %u, %u mips, %u slices: usage %u, %s", level,
                           format.name, width, height, padding, mips, slices, usage, listed ? "list" : "immediate");
                      if (usage != D3D11_USAGE_STAGING) {
                        auto read_desc = desc;
                        read_desc.Usage = D3D11_USAGE_STAGING;
                        read_desc.BindFlags = 0;
                        read_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        CHECK(device->CreateTexture2D(&read_desc, blank.data(), &staging));
                        (listed ? deferred.Get() : context.Get())->CopyResource(staging.Get(), texture.Get());
                        if (listed) {
                          ComPtr<ID3D11CommandList> commands;
                          CHECK(deferred->FinishCommandList(FALSE, &commands));
                          context->ExecuteCommandList(commands.Get(), FALSE);
                        }
                      }
                      for (UINT sub = 0; sub < data.size(); sub++) {
                        const UINT mip = sub % mips;
                        const UINT row_bytes = std::max(1u, width >> mip) * format.bytes;
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        CHECK(context->Map(staging.Get(), sub, D3D11_MAP_READ, 0, &mapped));
                        bool same = true;
                        for (UINT y = 0; y < std::max(1u, height >> mip); y++)
                          for (UINT b = 0; b < row_bytes; b++) {
                            auto have = ((const uint8_t *)mapped.pData)[y * mapped.RowPitch + b];
                            auto want = bytes[sub][y * data[sub].SysMemPitch + b];
                            if (same && have != want) {
                              expect(false, "subresource %u, row %u, byte %u is %#x, want %#x", sub, y, b,
                                     have, want);
                              same = false;
                            }
                          }
                        context->Unmap(staging.Get(), sub);
                        if (!same)
                          return verdict();
                      }
                    }
                  }
                }
      }
    UINT tested = 0, unsupported = 0;
    for (auto &src : copy_formats)
      for (auto &dst : copy_formats) {
        UINT src_support = 0, dst_support = 0;
        device->CheckFormatSupport(src.id, &src_support);
        device->CheckFormatSupport(dst.id, &dst_support);
        if (!(src_support & dst_support & D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
          unsupported++;
          continue;
        }
        if ((src.depth || dst.depth) && (level < D3D_FEATURE_LEVEL_10_1 || strcmp(src.family, dst.family)))
          continue;
        tested++;
        bool compatible = !strcmp(src.family, dst.family);
        auto linear = src.unit == 1 ? src.family : dst.family;
        if (level >= D3D_FEATURE_LEVEL_10_1 && src.bytes == dst.bytes) {
          if (src.unit != dst.unit)
            compatible =
                !strcmp(linear, "R16G16B16A16") || !strcmp(linear, "R32G32") || !strcmp(linear, "R32G32B32A32");
          if ((!strcmp(src.family, "R32") && !strcmp(dst.family, "R9G9B9E5")) ||
              (!strcmp(dst.family, "R32") && !strcmp(src.family, "R9G9B9E5")))
            compatible = true;
        }
        for (bool listed : {false, true})
          for (bool whole : {false, true}) {
            const UINT wide = 5, high = 3;
            auto recording = listed ? deferred.Get() : context.Get();
            step("seed 17/201, FL %x, %s -> %s, %s, %s", level, src.name, dst.name, listed ? "list" : "immediate",
                 whole ? "resource" : "region");
            std::vector<uint8_t> source(wide * high * src.bytes), initial(wide * high * dst.bytes);
            for (UINT i = 0; i < source.size(); i++)
              source[i] = uint8_t(i * 29 + i / src.bytes + 17);
            for (UINT i = 0; i < initial.size(); i++)
              initial[i] = uint8_t(i * 13 + 201);
            if ((src.depth || dst.depth) && (!strcmp(src.family, "R32") || !strcmp(src.family, "R32G8X24")))
              for (UINT i = 0; i < wide * high; i++) {
                float depth = float(i + 1) / (wide * high + 1);
                memcpy(source.data() + i * src.bytes, &depth, sizeof(depth));
                depth = 1.0f - depth;
                memcpy(initial.data() + i * dst.bytes, &depth, sizeof(depth));
              }
            auto make = [&](const CopyFormat &format, D3D11_USAGE usage, const void *bytes,
                            ComPtr<ID3D11Texture2D> &texture) {
              if (format.depth && usage == D3D11_USAGE_IMMUTABLE)
                usage = D3D11_USAGE_DEFAULT;
              D3D11_TEXTURE2D_DESC desc{wide * format.unit,
                                        high * format.unit,
                                        1,
                                        1,
                                        format.id,
                                        {1, 0},
                                        usage,
                                        usage == D3D11_USAGE_STAGING ? 0u
                                        : format.depth               ? D3D11_BIND_DEPTH_STENCIL
                                                                     : D3D11_BIND_SHADER_RESOURCE,
                                        usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ : 0u};
              D3D11_SUBRESOURCE_DATA data{bytes, wide * format.bytes};
              return device->CreateTexture2D(&desc, bytes ? &data : nullptr, &texture);
            };
            ComPtr<ID3D11Texture2D> from, into, readback;
            CHECK(make(src, D3D11_USAGE_IMMUTABLE, source.data(), from));
            CHECK(make(dst, D3D11_USAGE_DEFAULT, initial.data(), into));
            CHECK(make(dst, D3D11_USAGE_STAGING, nullptr, readback));
            for (bool copy : {false, true}) {
              step("seed 17/201, FL %x, %s -> %s, %s, %s, %s", level, src.name, dst.name, listed ? "list" : "immediate",
                   whole ? "resource" : "region", copy ? "copy" : "initial data control");
              const D3D11_BOX box{src.unit, 0, 0, 3 * src.unit, 2 * src.unit, 1};
              if (copy && whole)
                recording->CopyResource(into.Get(), from.Get());
              else if (copy && (src.depth || dst.depth))
                recording->CopySubresourceRegion(into.Get(), 0, 0, 0, 0, from.Get(), 0, nullptr);
              else if (copy)
                recording->CopySubresourceRegion(into.Get(), 0, 2 * dst.unit, dst.unit, 0, from.Get(), 0, &box);
              recording->CopyResource(readback.Get(), into.Get());
              if (listed) {
                ComPtr<ID3D11CommandList> commands;
                CHECK(deferred->FinishCommandList(FALSE, &commands));
                context->ExecuteCommandList(commands.Get(), FALSE);
              }
              D3D11_MAPPED_SUBRESOURCE mapped{};
              CHECK(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
              bool same = true;
              for (UINT y = 0; y < high; y++)
                for (UINT x = 0; x < wide; x++) {
                  bool full = whole || src.depth || dst.depth;
                  bool copied = copy && compatible && (full || (x >= 2 && x < 4 && y >= 1));
                  const uint8_t *want =
                      copied ? source.data() + ((full ? y * wide + x : (y - 1) * wide + x - 1) * src.bytes)
                             : initial.data() + (y * wide + x) * dst.bytes;
                  auto defined = copied ? src.defined & dst.defined : dst.defined;
                  for (UINT b = 0; b < dst.bytes; b++)
                    if (defined & (1u << b))
                      same &= ((const uint8_t *)mapped.pData)[y * mapped.RowPitch + x * dst.bytes + b] == want[b];
                }
              context->Unmap(readback.Get(), 0);
              if (!expect(same, "%s has wrong bytes",
                          !copy        ? "initial data control"
                          : compatible ? "compatible copy"
                                       : "refused copy"))
                return verdict();
            }
          }
      }
    printf("FL %x: %u format pairs checked, %u pairs without texture support\n", level, tested, unsupported);
    if (!expect(tested != 0, "no texture formats were exercised"))
      return verdict();
  }
  return verdict();
}
