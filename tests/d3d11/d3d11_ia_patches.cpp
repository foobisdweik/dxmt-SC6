// contract: D3D11.3 11.8.1.2: "Sending un-tessellated patches to NULL GS + Stream Output is valid."
// "As with other primitive types, only complete patches get written out; if there is not enough room to store a
// complete patch, it is discarded." 8.11 defines all 1 through 32 control-point topologies.
// 8.13: "Each Draw*() call starts a new Primitive Topology". 8.16/8.18 define relative vertex and instance IDs.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// control points are distinct integer records. compare every buffer word, including prefix, partial-patch space,
// and suffix, for every patch size, draw API, index format, and immediate/deferred context.
#include "d3d11_test.hpp"
#include <algorithm>
#include <array>

static const char hlsl[] = R"hlsl(
uint4 vs(uint vid : SV_VertexID, uint iid : SV_InstanceID) : VALUE {
  return uint4(vid, iid, vid * 7 + 1, iid * 11 + vid);
}
)hlsl";

int
main() {
  step("compile the vertex shader before creating a device");
  auto code = compile(hlsl, "vs", "vs");
  if (!expect(bool(code), "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                          &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  using Vertex = std::array<UINT, 4>;
  const UINT stride = sizeof(Vertex), word = sizeof(UINT), sentinel = 0xa5a5a5a5;
  const UINT maximum = D3D11_IA_PATCH_MAX_CONTROL_POINT_COUNT, prefix = word;
  const D3D11_SO_DECLARATION_ENTRY entry{0, "VALUE", 0, 0, 4, 0};
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11GeometryShader> stream;
  CHECK(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(), &entry,
      1, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &stream));
  const UINT first = 2, index_offset = 1;
  std::vector<UINT> indices(index_offset + first + 2 * maximum + 1);
  for (UINT i = 0; i < indices.size(); i++)
    indices[i] = (i * 13 + 5) % (2 * maximum + 1);
  std::vector<UINT16> narrow(indices.begin(), indices.end());
  ComPtr<ID3D11Buffer> index_buffers[2];
  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R32_UINT};
  const UINT index_sizes[] = {sizeof(UINT16), sizeof(UINT)};
  for (UINT f = 0; f < std::size(formats); f++) {
    D3D11_BUFFER_DESC desc{UINT(indices.size()) * index_sizes[f], D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER};
    const D3D11_SUBRESOURCE_DATA initial{f ? (void *)indices.data() : (void *)narrow.data()};
    CHECK(device->CreateBuffer(&desc, &initial, &index_buffers[f]));
  }
  for (auto context : {immediate.Get(), deferred.Get()})
    for (UINT points = 1; points <= maximum; points++) {
      std::vector<UINT> counts{0, points - 1, points, points + 1, 2 * points - 1, 2 * points, 2 * points + 1};
      std::sort(counts.begin(), counts.end());
      counts.erase(std::unique(counts.begin(), counts.end()), counts.end());
      for (UINT capacity : {points - 1, points, 2 * points - 1, 2 * points + 1}) {
        const UINT bytes = prefix + capacity * stride + word;
        auto output = buffer(device.Get(), bytes, D3D11_BIND_STREAM_OUTPUT, sentinel);
        const UINT calls = 6, maximum_cases = calls * std::size(formats) * UINT(counts.size());
        const UINT collected_words = maximum_cases * bytes / word;
        auto collected = buffer(device.Get(), collected_words * word, 0, sentinel);
        std::vector<UINT> all_wanted;
        if (!expect(output && collected, "no stream output or collected buffer"))
          return verdict();
        for (UINT call = 0; call < calls; call++)
          for (UINT f = 0; f < (call % 2 ? std::size(formats) : 1); f++)
            for (UINT count : counts) {
              const bool indexed = call % 2;
              const UINT instances = call < 2 ? 1 : 2;
              step("%s, patch %u, capacity %u vertices plus one word, call %u, format %u, count %u",
                   context == immediate.Get() ? "immediate" : "deferred", points, capacity, call, formats[f], count);
              std::vector<UINT> want(bytes / word, sentinel), initial(want);
              UINT at = prefix / word, remaining = capacity;
              for (UINT instance = 0; instance < instances; instance++)
                for (UINT patch = 0; patch < count / points; patch++) {
                  if (remaining < points)
                    break;
                  for (UINT cp = 0; cp < points; cp++) {
                    const UINT vertex = patch * points + cp;
                    const UINT id = indexed ? indices[index_offset + first + vertex] : vertex;
                    const Vertex value{id, instance, id * 7 + 1, instance * 11 + id};
                    std::copy(value.begin(), value.end(), want.begin() + at);
                    at += value.size();
                  }
                  remaining -= points;
                }
              context->UpdateSubresource(output.Get(), 0, nullptr, initial.data(), 0, 0);
              context->IASetInputLayout(nullptr);
              context->IASetIndexBuffer(index_buffers[f].Get(), formats[f], index_offset * index_sizes[f]);
              context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(
                  D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + points - 1));
              context->VSSetShader(vs.Get(), nullptr, 0);
              context->HSSetShader(nullptr, nullptr, 0);
              context->DSSetShader(nullptr, nullptr, 0);
              context->GSSetShader(stream.Get(), nullptr, 0);
              context->PSSetShader(nullptr, nullptr, 0);
              context->SOSetTargets(1, output.GetAddressOf(), &prefix);
              const INT base = 3;
              const UINT first_instance = 5;
              if (call >= 4) {
                const D3D11_DRAW_INSTANCED_INDIRECT_ARGS plain{count, instances, first, first_instance};
                const D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS indexed_args{count, instances, first, base, first_instance};
                const UINT padding = word, size = indexed ? sizeof(indexed_args) : sizeof(plain);
                std::vector<unsigned char> args(padding + size);
                memcpy(args.data() + padding, indexed ? (const void *)&indexed_args : &plain, size);
                D3D11_BUFFER_DESC desc{UINT(args.size()), D3D11_USAGE_DEFAULT};
                desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
                const D3D11_SUBRESOURCE_DATA data{args.data()};
                ComPtr<ID3D11Buffer> arguments;
                CHECK(device->CreateBuffer(&desc, &data, &arguments));
                if (indexed)
                  context->DrawIndexedInstancedIndirect(arguments.Get(), padding);
                else
                  context->DrawInstancedIndirect(arguments.Get(), padding);
              } else {
                if (call == 0) context->Draw(count, first);
                if (call == 1) context->DrawIndexed(count, first, base);
                if (call == 2) context->DrawInstanced(count, instances, first, first_instance);
                if (call == 3) context->DrawIndexedInstanced(count, instances, first, base, first_instance);
              }
              context->SOSetTargets(0, nullptr, nullptr);
              context->CopySubresourceRegion(collected.Get(), 0, all_wanted.size() * word, 0, 0, output.Get(), 0, nullptr);
              all_wanted.insert(all_wanted.end(), want.begin(), want.end());
            }
        if (context == deferred.Get()) {
          ComPtr<ID3D11CommandList> list;
          CHECK(context->FinishCommandList(FALSE, &list));
          immediate->ExecuteCommandList(list.Get(), TRUE);
        }
        all_wanted.resize(collected_words, sentinel);
        auto got = read(device.Get(), immediate.Get(), collected.Get());
        step("%s, patch %u, capacity %u: collected output", context == immediate.Get() ? "immediate" : "deferred",
             points, capacity);
        if (expect(got.size() == all_wanted.size(), "read %zu words, want %zu", got.size(), all_wanted.size()))
          for (UINT i = 0; i < all_wanted.size(); i++)
            if (got[i] != all_wanted[i]) {
              expect(false, "case %u word %u is %#x, want %#x", i / (bytes / word), i % (bytes / word), got[i], all_wanted[i]);
              break;
            }
      }
    }
  return verdict();
}
