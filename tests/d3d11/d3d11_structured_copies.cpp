// contract: structured/linear copies preserve bytes, while incomplete structures and mismatched strides are refused.
// "linear to structured, structured to linear, and structured to structured" are allowed; "the strides must be
// the same or the runtime will fail the copy operation"; "If the region to copy is not specified as complete
// structures, then the runtime will fail the copy operation." (D3D11.3 5.6.5, D3D11_3_FunctionalSpec.htm).
// the destination remains intact when a void copy is refused. offsets and sizes are independent multiples of
// both structures' strides; each negative case violates only source alignment, destination alignment or length.
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <algorithm>
#include <array>
#include <numeric>

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
  constexpr UINT maximum_stride = D3D11_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES;
  for (UINT source_stride : {0u, UINT(sizeof(UINT)), UINT(3 * sizeof(UINT)), UINT(4 * sizeof(UINT)), maximum_stride})
    for (UINT dest_stride : {0u, UINT(sizeof(UINT)), UINT(3 * sizeof(UINT)), UINT(4 * sizeof(UINT)), maximum_stride}) {
      const UINT unit = std::lcm(std::max(UINT(sizeof(UINT)), source_stride),
                                 std::max(UINT(sizeof(UINT)), dest_stride));
      const UINT bytes = 4 * unit;
      std::vector<unsigned char> source(bytes), before(bytes);
      for (UINT i = 0; i < bytes; i++) {
        source[i] = static_cast<unsigned char>(i * 3 + 1);
        before[i] = static_cast<unsigned char>(~source[i]);
      }
      auto make = [&](UINT stride, const std::vector<unsigned char> &data, ComPtr<ID3D11Buffer> &out) {
        D3D11_BUFFER_DESC desc{bytes, D3D11_USAGE_DEFAULT, stride ? D3D11_BIND_SHADER_RESOURCE : 0u, 0,
                                stride ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED : 0u, stride};
        D3D11_SUBRESOURCE_DATA initial{data.data()};
        return device->CreateBuffer(&desc, &initial, &out);
      };
      ComPtr<ID3D11Buffer> src;
      CHECK(make(source_stride, source, src));
      for (UINT route = 0; route < std::size(contexts); route++)
        for (UINT mode = 0; mode < 7; mode++) {
          if ((mode == 3 && !source_stride) || (mode == 4 && !dest_stride) ||
              (mode == 5 && !source_stride && !dest_stride))
            continue;
          UINT left = mode < 2 ? 0 : unit, length = mode < 2 ? bytes : unit;
          UINT placed = mode < 2 ? 0 : unit * 2;
          if (mode == 3)
            left++;
          if (mode == 4)
            placed++;
          if (mode == 5)
            length--;
          if (mode == 6)
            length = 0;
          const bool compatible = !source_stride || !dest_stride || source_stride == dest_stride;
          const bool complete_source = !source_stride || (left % source_stride == 0 && length % source_stride == 0);
          const bool complete_dest = !dest_stride || (placed % dest_stride == 0 && length % dest_stride == 0);
          const bool copies = compatible && complete_source && complete_dest && length;
          auto expected = before;
          if (copies)
            std::copy_n(source.begin() + left, length, expected.begin() + placed);
          step("structured copy strides %u -> %u route %u mode %u source [%u,%u) dest %u",
               source_stride, dest_stride, route, mode, left, left + length, placed);
          ComPtr<ID3D11Buffer> dst;
          CHECK(make(dest_stride, before, dst));
          auto context = contexts[route].Get();
          const D3D11_BOX box{left, 0, 0, left + length, 1, 1};
          if (!mode)
            context->CopyResource(dst.Get(), src.Get());
          else if (mode % 2)
            context->CopySubresourceRegion1(dst.Get(), 0, placed, 0, 0, src.Get(), 0, &box, 0);
          else
            context->CopySubresourceRegion(dst.Get(), 0, placed, 0, 0, src.Get(), 0, &box);
          if (route) {
            ComPtr<ID3D11CommandList> commands;
            CHECK(deferred->FinishCommandList(FALSE, &commands));
            immediate->ExecuteCommandList(commands.Get(), FALSE);
          }
          const auto words = read(device.Get(), immediate.Get(), dst.Get());
          expect(words.size() * sizeof(UINT) == bytes && !memcmp(words.data(), expected.data(), bytes),
                 "destination bytes differ, copy valid %u", copies);
          const auto unchanged = read(device.Get(), immediate.Get(), src.Get());
          expect(unchanged.size() * sizeof(UINT) == bytes && !memcmp(unchanged.data(), source.data(), bytes),
                 "source bytes changed");
        }
    }
  return verdict();
}
