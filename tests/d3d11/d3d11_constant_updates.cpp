// contract: partial constant updates change whole constants, with no writes from a dropped call.
// "Either NO_OVERWRITE or DISCARD flags must be specified for a partial update, and
// the extents of the pDstBox parameter must be aligned to 16 byte (full constant) boundaries or the call is dropped."
// (D3D11.3 5.3.4.3.1, D3D11_3_FunctionalSpec.htm). "DISCARD means that the system may discard the entire contents
// of the destination memory outside the region being updated." (5.6.9).
// each written byte comes from the start of the supplied data, independent of the destination box offset.
// every alignment residue is tested at both ends, around the maximum shader-visible constant-buffer size.
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <algorithm>
#include <array>

int
main() {
  using Constant = std::array<UINT, 4>;
  constexpr UINT alignment = sizeof(Constant);
  constexpr UINT limit = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * alignment;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11DeviceContext1> contexts[2];
  CHECK(immediate.As(&contexts[0]));
  CHECK(deferred.As(&contexts[1]));
  for (UINT bytes : {alignment, alignment * alignment, limit - alignment, limit, limit + alignment}) {
    std::vector<unsigned char> before(bytes), source(bytes);
    for (UINT i = 0; i < bytes; i++) {
      before[i] = static_cast<unsigned char>(i * 3 + 1);
      source[i] = static_cast<unsigned char>(~before[i]);
    }
    D3D11_BUFFER_DESC desc{bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    ComPtr<ID3D11Buffer> target, staging;
    CHECK(device->CreateBuffer(&desc, nullptr, &target));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateBuffer(&desc, nullptr, &staging));
    for (UINT route = 0; route < std::size(contexts); route++)
      for (UINT flags : {0u, UINT(D3D11_COPY_NO_OVERWRITE), UINT(D3D11_COPY_DISCARD)})
        for (UINT residue = 0; residue < alignment; residue++)
          for (UINT end : {0u, 1u, 2u})
            for (bool near_end : {false, true}) {
              if (end == 2 && bytes == alignment && residue)
                continue;
              const UINT base = near_end && bytes > alignment ? bytes - alignment * (end == 2 ? 2 : 1) : 0;
              const UINT left = base + (end == 1 ? 0 : residue);
              const UINT right = base + alignment - (end == 1 ? residue : 0) + (end == 2 ? residue : 0);
              const bool full = left == 0 && right == bytes;
              const bool valid = (full || flags) && left % alignment == 0 && right % alignment == 0;
              step("%s CB bytes %u flags %#x box [%u,%u)", route ? "deferred" : "immediate",
                   bytes, flags, left, right);
              auto context = contexts[route].Get();
              context->UpdateSubresource1(target.Get(), 0, nullptr, before.data(), 0, 0, D3D11_COPY_DISCARD);
              D3D11_BOX box{left, 0, 0, right, 1, 1};
              context->UpdateSubresource1(target.Get(), 0, &box, source.data(), 0, 0, flags);
              context->CopyResource(staging.Get(), target.Get());
              if (route) {
                ComPtr<ID3D11CommandList> commands;
                CHECK(deferred->FinishCommandList(FALSE, &commands));
                immediate->ExecuteCommandList(commands.Get(), FALSE);
              }
              D3D11_MAPPED_SUBRESOURCE mapped;
              CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
              for (UINT i = 0; i < bytes; i++) {
                const bool written = valid && i >= left && i < right;
                if (valid && flags == D3D11_COPY_DISCARD && !written)
                  continue;
                const auto want = written ? source[i - left] : before[i];
                const auto got = static_cast<unsigned char *>(mapped.pData)[i];
                if (!expect(got == want, "byte %u: %#x, want %#x", i, got, want))
                  break;
              }
              immediate->Unmap(staging.Get(), 0);
            }
  }
  return verdict();
}
