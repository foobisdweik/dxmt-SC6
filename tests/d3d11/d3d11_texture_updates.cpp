// contract: a texture update starts at the supplied source pointer and addresses rows/planes by the supplied pitches.
// "Resource pitches are always expressed in bytes"; "including zero for which the first row will be replicated
// to all rows" (D3D11.3 5.5). "DISCARD means that the system may discard the entire contents of the destination
// memory outside the region being updated." (5.6.9). UpdateSubresource copies the source before it returns:
// "the application is free to change or even free the data pointed to by pSrcData" (Microsoft Learn, Remarks).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-updatesubresource
// full, inset, trailing and empty boxes span every mip/slice of 1D/2D/3D resources. poisoned row/plane padding,
// tight rows with padded planes, and zero pitches distinguish each source-address term. both contexts and the
// Direct3D 10 wrapper read the same independently computed words. no discarded byte has an invented expectation.
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <d3d11_1.h>
#include <algorithm>
#include <array>
#include <climits>
#include <bit>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11DeviceContext1> contexts[2];
  CHECK(immediate.As(&contexts[0]));
  CHECK(deferred.As(&contexts[1]));
  ComPtr<ID3D11Device1> device1;
  CHECK(device.As(&device1));
  ComPtr<ID3DDeviceContextState> compatibility;
  CHECK(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D10Device1),
                                         nullptr, &compatibility));
  ComPtr<ID3D10Device1> legacy;
  CHECK(device.As(&legacy));
  constexpr UINT word_bytes = sizeof(UINT), poison = UINT_MAX;
  const UINT page_words = MULLION_PAGE_SIZE / word_bytes;
  UINT serial = 0;
  for (UINT dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
       dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D; dimension++)
    for (UINT width : {1u, 3u, 7u, 8u, 9u, page_words - 1, page_words, page_words + 1}) {
      const UINT height = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? 1 : 5;
      const UINT depth = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 3 : 1;
      const UINT layers = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1 : 3;
      UINT mips = 1;
      for (UINT extent = std::max({width, height, depth}); extent > 1; extent >>= 1)
        mips++;
      ComPtr<ID3D11Resource> target, staging;
      for (bool readback : {false, true}) {
        const auto usage = readback ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
        const UINT access = readback ? D3D11_CPU_ACCESS_READ : 0;
        ComPtr<ID3D11Resource> made;
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
          D3D11_TEXTURE1D_DESC desc{width, mips, layers, DXGI_FORMAT_R32_UINT, usage, 0, access};
          ComPtr<ID3D11Texture1D> texture;
          CHECK(device->CreateTexture1D(&desc, nullptr, &texture));
          made = texture;
        } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
          D3D11_TEXTURE2D_DESC desc{width, height, mips, layers, DXGI_FORMAT_R32_UINT, {1, 0}, usage, 0, access};
          ComPtr<ID3D11Texture2D> texture;
          CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
          made = texture;
        } else {
          D3D11_TEXTURE3D_DESC desc{width, height, depth, mips, DXGI_FORMAT_R32_UINT, usage, 0, access};
          ComPtr<ID3D11Texture3D> texture;
          CHECK(device->CreateTexture3D(&desc, nullptr, &texture));
          made = texture;
        }
        (readback ? staging : target) = made;
      }
      ComPtr<ID3D10Resource> target10, staging10;
      CHECK(target.As(&target10));
      CHECK(staging.As(&staging10));
      for (UINT route = 0; route < 3; route++)
        for (UINT flags : {0u, UINT(D3D11_COPY_NO_OVERWRITE), UINT(D3D11_COPY_DISCARD)}) {
          if (route == 2 && flags)
            continue;
          for (UINT sub = 0; sub < layers * mips; sub++) {
            const UINT mip = sub % mips;
            const std::array<UINT, 3> e{std::max(1u, width >> mip), std::max(1u, height >> mip),
                                       std::max(1u, depth >> mip)};
            std::vector<UINT> before(e[0] * e[1] * e[2]);
            for (auto &word : before)
              word = ++serial;
            for (UINT mode = 0; mode < 6; mode++)
              for (UINT pitch = 0; pitch < 4; pitch++) {
                std::array<UINT, 3> begin{}, end = e;
                if (mode == 1)
                  for (UINT axis = 0; axis < e.size(); axis++) {
                    begin[axis] = e[axis] > 2 ? 1 : 0;
                    end[axis] -= e[axis] > 2 ? 1 : 0;
                  }
                if (mode == 2)
                  for (UINT axis = 0; axis < e.size(); axis++)
                    begin[axis] = end[axis] - 1;
                if (mode >= 3)
                  end[mode - 3] = begin[mode - 3];
                const UINT w = end[0] - begin[0], h = end[1] - begin[1], d = end[2] - begin[2];
                const UINT row_words = pitch == 3 ? 0 : w + (pitch == 1 ? 3 : 0);
                const UINT plane_words = pitch == 3 ? 0 : row_words * h + (pitch ? 5 : 0);
                std::vector<UINT> source(std::max(1u, plane_words * std::max(1u, d) + row_words + w), poison);
                for (UINT z = 0; z < d; z++)
                  for (UINT y = 0; y < h; y++)
                    for (UINT x = 0; x < w; x++)
                      source[z * plane_words + y * row_words + x] = ++serial;
                auto expected = before;
                for (UINT z = begin[2]; z < end[2]; z++)
                  for (UINT y = begin[1]; y < end[1]; y++)
                    for (UINT x = begin[0]; x < end[0]; x++)
                      expected[(z * e[1] + y) * e[0] + x] =
                          source[(z - begin[2]) * plane_words + (y - begin[1]) * row_words + x - begin[0]];
                step("texture dim %u width %u sub %u route %u flags %#x mode %u pitches %u/%u",
                     dimension, width, sub, route, flags, mode, row_words * word_bytes, plane_words * word_bytes);
                const D3D11_BOX box{begin[0], begin[1], begin[2], end[0], end[1], end[2]};
                if (route == 2) {
                  ComPtr<ID3DDeviceContextState> previous;
                  contexts[0]->SwapDeviceContextState(compatibility.Get(), &previous);
                  legacy->UpdateSubresource(target10.Get(), sub, nullptr, before.data(), e[0] * word_bytes,
                                             e[0] * e[1] * word_bytes);
                  const auto box10 = std::bit_cast<D3D10_BOX>(box);
                  legacy->UpdateSubresource(target10.Get(), sub, mode ? &box10 : nullptr, source.data(),
                                             row_words * word_bytes, plane_words * word_bytes);
                  legacy->CopySubresourceRegion(staging10.Get(), sub, 0, 0, 0, target10.Get(), sub, nullptr);
                  contexts[0]->SwapDeviceContextState(previous.Get(), nullptr);
                } else {
                  auto context = contexts[route].Get();
                  context->UpdateSubresource1(target.Get(), sub, nullptr, before.data(), e[0] * word_bytes,
                                               e[0] * e[1] * word_bytes, D3D11_COPY_DISCARD);
                  if (flags)
                    context->UpdateSubresource1(target.Get(), sub, mode ? &box : nullptr, source.data(),
                                                 row_words * word_bytes, plane_words * word_bytes, flags);
                  else
                    context->UpdateSubresource(target.Get(), sub, mode ? &box : nullptr, source.data(),
                                                row_words * word_bytes, plane_words * word_bytes);
                  context->CopySubresourceRegion(staging.Get(), sub, 0, 0, 0, target.Get(), sub, nullptr);
                }
                std::fill(source.begin(), source.end(), poison);
                if (route == 1) {
                  ComPtr<ID3D11CommandList> commands;
                  CHECK(deferred->FinishCommandList(FALSE, &commands));
                  immediate->ExecuteCommandList(commands.Get(), FALSE);
                }
                D3D11_MAPPED_SUBRESOURCE mapped;
                CHECK(immediate->Map(staging.Get(), sub, D3D11_MAP_READ, 0, &mapped));
                bool ok = true;
                for (UINT z = 0; z < e[2] && ok; z++)
                  for (UINT y = 0; y < e[1] && ok; y++)
                    for (UINT x = 0; x < e[0] && ok; x++) {
                      const bool written = x >= begin[0] && x < end[0] && y >= begin[1] && y < end[1] &&
                                           z >= begin[2] && z < end[2];
                      if (flags == D3D11_COPY_DISCARD && w && h && d && !written)
                        continue;
                      UINT got;
                      memcpy(&got, static_cast<char *>(mapped.pData) + z * mapped.DepthPitch + y * mapped.RowPitch +
                                       x * word_bytes, sizeof(got));
                      const UINT want = expected[(z * e[1] + y) * e[0] + x];
                      ok = expect(got == want, "texel (%u,%u,%u): %#x, want %#x", x, y, z, got, want);
                    }
                immediate->Unmap(staging.Get(), sub);
                if (!ok)
                  return verdict();
              }
          }
        }
    }
  return verdict();
}
