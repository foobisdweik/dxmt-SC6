// contract: every buffer use observes the latest update or copy, including after whole updates rename its storage.
// "the Subresource may still be able to be modified with UpdateSubresourceUP" (D3D11.3 5.6.8).
// DISCARD can discard bytes outside the written extent (5.6.9), so partial DISCARD checks only the written region.
// NO_OVERWRITE changes a row no pending draw reads, before any full-buffer copies. CB ranges follow 5.3.4.3.2:
// "FirstConstant must be a multiple of 16 constants." ordinary CBs use their whole binding, larger CBs also use a
// nonzero range.
// sizes cross BufferAllocation's minimum and MULLION_PAGE_SIZE. fifteen page / bytes capacities (one above a page)
// plus one update pass at least the first and last names of the first three doublings, including names above a page.
// every generation draws distinct integer pixels. copies check every byte through staging Map, including bytes
// outside partial writes. view, vertex, index, constant and argument offsets are nonzero independently of storage
// offsets. immediate and deferred contexts use the same operations; output-bound controls must retain their bytes.
// VertexID is the index value (8.16) and InstanceID starts at zero (8.18). indirect draws vary the instance count
// or index selection; the last instance writes the pixel. shader row selection uses an independent index buffer.
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <algorithm>
#include <array>

using Row = std::array<UINT, 4>;
enum Reader { Vertex, Index, Constant, Typed, Structured, Raw, Indirect, IndexedIndirect, VertexOutput, TypedOutput };

static const char hlsl[] = R"hlsl(
cbuffer Constants : register(b0) { uint4 rows[CONSTANTS]; };
#if READER == 3 || READER == 9
Buffer<uint4> typed_data : register(t0);
#elif READER == 4
StructuredBuffer<uint4> structured_data : register(t0);
#elif READER == 5
ByteAddressBuffer raw_data : register(t0);
#endif
struct V { float4 position : SV_Position; nointerpolation uint4 value : VALUE; };
V vs(uint4 data : DATA, uint id : SV_VertexID, uint instance_id : SV_InstanceID) {
  V v;
  v.position = float4(0, 0, 0, 1);
#if READER == 0 || READER == 8
  v.value = data;
#elif READER == 2
  v.value = rows[id];
#elif READER == 3 || READER == 9
  v.value = typed_data.Load(id);
#elif READER == 4
  v.value = structured_data[id];
#elif READER == 5
  v.value = raw_data.Load4(id * ROW_BYTES);
#else
  v.value = uint4(id, instance_id, id ^ instance_id, 1);
#endif
  return v;
}
uint4 ps(V v) : SV_Target { return v.value; }
)hlsl";

int
main(int argc, char **argv) {
  step("compile every shader before creating a device");
  ComPtr<ID3DBlob> code[TypedOutput + 1];
  for (UINT kind = 0; kind < std::size(code); kind++) {
    code[kind] = compile(hlsl, "vs", "vs", {"READER=" + std::to_string(kind),
                                            "ROW_BYTES=" + std::to_string(sizeof(Row)),
                                            "CONSTANTS=" + std::to_string(D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT)});
    if (!code[kind])
      return 1;
  }
  auto ps_code = compile(hlsl, "ps", "ps", {"READER=0",
                                           "CONSTANTS=" + std::to_string(D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT)});
  if (!ps_code)
    return 1;
  if (argc > 1 && !strcmp(argv[1], "shaders"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11DeviceContext1> immediate1, deferred1;
  CHECK(immediate.As(&immediate1));
  CHECK(deferred.As(&deferred1));
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  ComPtr<ID3D11VertexShader> shaders[std::size(code)];
  ComPtr<ID3D11InputLayout> layouts[std::size(code)];
  D3D11_INPUT_ELEMENT_DESC element{"DATA", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
  for (UINT kind = 0; kind < std::size(code); kind++) {
    CHECK(device->CreateVertexShader(code[kind]->GetBufferPointer(), code[kind]->GetBufferSize(), nullptr,
                                     &shaders[kind]));
    if (kind == Vertex || kind == VertexOutput)
      CHECK(device->CreateInputLayout(&element, 1, code[kind]->GetBufferPointer(), code[kind]->GetBufferSize(),
                                      &layouts[kind]));
  }
  std::vector<UINT> sizes;
  for (UINT bytes = 4 * sizeof(Row); bytes < MULLION_PAGE_SIZE; bytes *= 2)
    sizes.push_back(bytes);
  sizes.insert(sizes.end(), {MULLION_PAGE_SIZE - sizeof(Row), MULLION_PAGE_SIZE, MULLION_PAGE_SIZE + sizeof(Row),
                              4 * MULLION_PAGE_SIZE});
  const char *names[] = {"vertex", "index", "constant", "typed", "structured", "raw", "indirect", "indexed indirect",
                         "vertex/output", "typed/output"};
  for (UINT bytes : sizes)
    for (UINT kind = 0; kind < std::size(code); kind++)
      for (bool initialized : {false, true})
        for (bool recorded : {false, true}) {
          step("%s %s, %u bytes, initial data %u", recorded ? "deferred" : "immediate", names[kind], bytes,
               initialized);
          immediate->ClearState();
          deferred->ClearState();
          auto context = recorded ? deferred1.Get() : immediate1.Get();
          const UINT rows = bytes / sizeof(Row);
          const UINT generations = (1 + 2 + 4 + 8) * std::max(1u, MULLION_PAGE_SIZE / bytes) + 1;
          // D3D11.3 5.3.4.3.2 defines binding alignment in constants, distinct from a constant's byte size
          const UINT range_alignment = 16, first = kind == Constant && rows > range_alignment ? range_alignment : 0;
          const UINT probes[] = {first + 1, std::max(first + 1, rows / 2), rows - 1};
          auto data = [&](UINT version) {
            std::vector<Row> out(rows);
            for (UINT r = 0; r < rows; r++)
              for (UINT c = 0; c < out[r].size(); c++)
                out[r][c] = (version * rows + r) * out[r].size() + c + 1;
            if (kind == Indirect)
              out[1] = {1, version % rows + 1, 0, 0};
            if (kind == IndexedIndirect) {
              out[1] = {1, 1, version % rows, 0};
              out[2][0] = 0;
            }
            return out;
          };
          auto contents = data(0);
          const UINT bind = kind == Constant ? D3D11_BIND_CONSTANT_BUFFER
                            : kind == Index ? D3D11_BIND_INDEX_BUFFER
                            : kind == Vertex || kind == VertexOutput ? D3D11_BIND_VERTEX_BUFFER
                            : kind == Indirect || kind == IndexedIndirect ? 0 : D3D11_BIND_SHADER_RESOURCE;
          D3D11_BUFFER_DESC bd{bytes, D3D11_USAGE_DEFAULT,
                                 bind | (kind == VertexOutput ? D3D11_BIND_STREAM_OUTPUT : 0u) |
                                   (kind == TypedOutput ? D3D11_BIND_UNORDERED_ACCESS : 0u), 0,
                                 kind == Structured ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED
                                 : kind == Raw ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
                                 : kind == Indirect || kind == IndexedIndirect
                                   ? D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS : 0u,
                                 kind == Structured ? UINT(sizeof(Row)) : 0u};
          D3D11_SUBRESOURCE_DATA initial{contents.data()};
          ComPtr<ID3D11Buffer> input, donor;
          CHECK(device->CreateBuffer(&bd, initialized ? &initial : nullptr, &input));
          CHECK(device->CreateBuffer(&bd, &initial, &donor));
          // a fault that reads the initial name's indirect arguments must still draw a bounded number of points
          if (!initialized)
            context->CopyResource(input.Get(), donor.Get());
          ComPtr<ID3D11ShaderResourceView> srv;
          if (bind & D3D11_BIND_SHADER_RESOURCE) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{kind == Structured ? DXGI_FORMAT_UNKNOWN
                                              : kind == Raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32G32B32A32_UINT,
                                              kind == Raw ? D3D11_SRV_DIMENSION_BUFFEREX : D3D11_SRV_DIMENSION_BUFFER};
            sd.BufferEx.FirstElement = kind == Raw ? sizeof(Row) / sizeof(UINT) : 1;
            sd.BufferEx.NumElements = kind == Raw ? (rows - 1) * sizeof(Row) / sizeof(UINT) : rows - 1;
            sd.BufferEx.Flags = kind == Raw ? D3D11_BUFFEREX_SRV_FLAG_RAW : 0;
            CHECK(device->CreateShaderResourceView(input.Get(), &sd, &srv));
          }
          const UINT width = (generations + 7) * std::size(probes);
          D3D11_TEXTURE2D_DESC td{width, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_RENDER_TARGET};
          ComPtr<ID3D11Texture2D> target, staging;
          ComPtr<ID3D11RenderTargetView> rtv;
          CHECK(device->CreateTexture2D(&td, nullptr, &target));
          CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
          td.Usage = D3D11_USAGE_STAGING;
          td.BindFlags = 0;
          td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          CHECK(device->CreateTexture2D(&td, nullptr, &staging));
          const float clear[4]{};
          immediate->ClearRenderTargetView(rtv.Get(), clear);
          context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
          context->IASetInputLayout(layouts[kind].Get());
          context->VSSetShader(shaders[kind].Get(), nullptr, 0);
          context->PSSetShader(ps.Get(), nullptr, 0);
          context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
          const UINT stride = sizeof(Row), vertex_offset = sizeof(Row);
          if (bind & D3D11_BIND_VERTEX_BUFFER)
            context->IASetVertexBuffers(0, 1, input.GetAddressOf(), &stride, &vertex_offset);
          if (kind == Constant) {
            const UINT count = (rows - first + range_alignment - 1) / range_alignment * range_alignment;
            if (first)
              context->VSSetConstantBuffers1(0, 1, input.GetAddressOf(), &first, &count);
            else
              context->VSSetConstantBuffers(0, 1, input.GetAddressOf());
          }
          if (srv)
            context->VSSetShaderResources(0, 1, srv.GetAddressOf());
          std::vector<UINT> indices(rows);
          for (UINT r = 0; r < rows; r++)
            indices[r] = r;
          D3D11_BUFFER_DESC idesc{UINT(indices.size() * sizeof(UINT)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER};
          D3D11_SUBRESOURCE_DATA idata{indices.data()};
          ComPtr<ID3D11Buffer> selector;
          CHECK(device->CreateBuffer(&idesc, &idata, &selector));
          if (kind == Index)
            context->IASetIndexBuffer(input.Get(), DXGI_FORMAT_R32_UINT, sizeof(Row));
          if (kind != Index && kind != Vertex && kind != VertexOutput && kind != Indirect)
            context->IASetIndexBuffer(selector.Get(), DXGI_FORMAT_R32_UINT, 0);
          std::vector<Row> want(width);
          UINT pixel = 0;
          auto draw = [&](UINT row) {
            D3D11_VIEWPORT viewport{float(pixel), 0, 1, 1, 0, 1};
            context->RSSetViewports(1, &viewport);
            if (kind == Index) {
              context->DrawIndexed(1, (row - 1) * sizeof(Row) / sizeof(UINT), 0);
              UINT id = contents[row][0];
              want[pixel++] = {id, 0, id, 1};
            } else if (kind == Indirect || kind == IndexedIndirect) {
              if (kind == Indirect)
                context->DrawInstancedIndirect(input.Get(), sizeof(Row));
              else
                context->DrawIndexedInstancedIndirect(input.Get(), sizeof(Row));
              UINT id = kind == Indirect ? 0 : contents[1][2];
              UINT instance = kind == Indirect ? contents[1][1] - 1 : 0;
              want[pixel++] = {id, instance, id ^ instance, 1};
            } else {
              const UINT base = kind == Constant ? first : 1;
              if (kind == Vertex || kind == VertexOutput)
                context->Draw(1, row - base);
              else
                context->DrawIndexed(1, row - base, 0);
              want[pixel++] = contents[row];
            }
          };
          if (initialized)
            for (UINT row : probes)
              draw(row);
          for (UINT n = 1; n <= generations; n++) {
            contents = data(n);
            context->UpdateSubresource(input.Get(), 0, nullptr, contents.data(), 0, 0);
            for (UINT row : probes)
              draw(row);
          }
          step("NO_OVERWRITE changes an unreferenced row");
          auto patch = data(generations + 1);
          const UINT untouched = kind == Constant ? first : 0;
          D3D11_BOX box{untouched * stride, 0, 0, (untouched + 1) * stride, 1, 1};
          context->UpdateSubresource1(input.Get(), 0, &box, &patch[untouched], 0, 0, D3D11_COPY_NO_OVERWRITE);
          contents[untouched] = patch[untouched];
          for (UINT row : probes)
            draw(row);
          auto copied = buffer(device.Get(), bytes, 0);
          CHECK(copied ? S_OK : E_FAIL);
          context->UpdateSubresource(copied.Get(), 0, nullptr, patch.data(), 0, 0);
          context->UpdateSubresource(copied.Get(), 0, nullptr, patch.data(), 0, 0);
          context->CopyResource(copied.Get(), input.Get());
          const auto copied_want = contents;
          step("partial update preserves surrounding bytes; constant buffers use whole DISCARD");
          patch = data(generations + 2);
          if (kind == Constant) {
            context->UpdateSubresource1(input.Get(), 0, nullptr, patch.data(), 0, 0, D3D11_COPY_DISCARD);
            contents = patch;
          } else {
            box = {sizeof(Row), 0, 0, 3 * sizeof(Row), 1, 1};
            context->UpdateSubresource(input.Get(), 0, &box, &patch[1], 0, 0);
            std::copy(patch.begin() + 1, patch.begin() + 3, contents.begin() + 1);
          }
          for (UINT row : probes)
            draw(row);
          auto partial = buffer(device.Get(), bytes, 0);
          CHECK(partial ? S_OK : E_FAIL);
          context->CopyResource(partial.Get(), input.Get());
          const auto partial_want = contents;
          step("GPU whole and region copies use both renamed endpoints");
          auto donor_data = data(generations + 3);
          for (UINT n = 0; n < 2; n++)
            context->UpdateSubresource(donor.Get(), 0, nullptr, donor_data.data(), 0, 0);
          context->CopyResource(input.Get(), donor.Get());
          contents = donor_data;
          for (UINT row : probes)
            draw(row);
          auto whole = buffer(device.Get(), bytes, 0);
          CHECK(whole ? S_OK : E_FAIL);
          context->CopyResource(whole.Get(), input.Get());
          const auto whole_want = contents;
          donor_data = data(generations + 4);
          context->UpdateSubresource(donor.Get(), 0, nullptr, donor_data.data(), 0, 0);
          box = {sizeof(Row), 0, 0, 3 * sizeof(Row), 1, 1};
          const UINT destination = kind == Constant ? first : 1;
          context->CopySubresourceRegion(input.Get(), 0, destination * sizeof(Row), 0, 0, donor.Get(), 0, &box);
          std::copy(donor_data.begin() + 1, donor_data.begin() + 3, contents.begin() + destination);
          for (UINT row : probes)
            draw(row);
          auto last = buffer(device.Get(), bytes, 0);
          CHECK(last ? S_OK : E_FAIL);
          context->CopyResource(last.Get(), input.Get());
          const auto last_want = contents;
          auto region = buffer(device.Get(), bytes, 0);
          CHECK(region ? S_OK : E_FAIL);
          context->CopySubresourceRegion(region.Get(), 0, sizeof(Row), 0, 0, input.Get(), 0, &box);
          std::vector<Row> region_want(rows);
          std::copy(contents.begin() + 1, contents.begin() + 3, region_want.begin() + 1);
          step("partial DISCARD reads only the written extent");
          patch = data(generations + 5);
          box = {sizeof(Row), 0, 0, 3 * sizeof(Row), 1, 1};
          if (kind == Constant)
            box = {first * stride, 0, 0, (first + 2) * stride, 1, 1};
          context->UpdateSubresource1(input.Get(), 0, &box, &patch[box.left / sizeof(Row)], 0, 0, D3D11_COPY_DISCARD);
          std::copy(patch.begin() + box.left / sizeof(Row), patch.begin() + box.right / sizeof(Row),
                    contents.begin() + box.left / sizeof(Row));
          if (kind == Indirect || kind == IndexedIndirect)
            draw(1);
          else
            draw(box.left / sizeof(Row) + (kind == Constant ? 0 : 1));
          auto discarded = buffer(device.Get(), 2 * sizeof(Row), 0);
          CHECK(discarded ? S_OK : E_FAIL);
          context->CopySubresourceRegion(discarded.Get(), 0, 0, 0, 0, input.Get(), 0, &box);
          if (recorded) {
            ComPtr<ID3D11CommandList> list;
            CHECK(deferred->FinishCommandList(FALSE, &list));
            immediate->ExecuteCommandList(list.Get(), FALSE);
          }
          for (auto mode : {D3D11_MAP_READ, D3D11_MAP_WRITE_DISCARD}) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            auto hr = context->Map(input.Get(), 0, mode, 0, &mapped);
            expect(hr == E_INVALIDARG, "default Map(%u) returned %#lx", mode, hr);
            if (SUCCEEDED(hr))
              context->Unmap(input.Get(), 0);
          }
          auto verify = [&](ID3D11Buffer *source, const std::vector<Row> &expected) {
            auto got = read(device.Get(), immediate.Get(), source);
            return expect(got.size() * sizeof(UINT) == expected.size() * sizeof(Row) &&
                            !memcmp(got.data(), expected.data(), expected.size() * sizeof(Row)), "copied bytes differ");
          };
          if (!verify(copied.Get(), copied_want) || !verify(partial.Get(), partial_want) ||
              !verify(whole.Get(), whole_want) || !verify(last.Get(), last_want) ||
              !verify(region.Get(), region_want) ||
              !verify(discarded.Get(), {patch[box.left / sizeof(Row)], patch[box.left / sizeof(Row) + 1]}))
            return verdict();
          immediate->CopyResource(staging.Get(), target.Get());
          D3D11_MAPPED_SUBRESOURCE mapped{};
          CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
          auto got = static_cast<const Row *>(mapped.pData);
          for (UINT p = 0; p < width; p++)
            if (!expect(got[p] == want[p], "pixel %u: %u,%u,%u,%u, want %u,%u,%u,%u", p, got[p][0], got[p][1],
                        got[p][2], got[p][3], want[p][0], want[p][1], want[p][2], want[p][3])) {
              immediate->Unmap(staging.Get(), 0);
              return verdict();
            }
          immediate->Unmap(staging.Get(), 0);
        }
  return verdict();
}
