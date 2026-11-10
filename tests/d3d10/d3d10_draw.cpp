// contract: every D3D10 draw fetches the vertex/index/instance interval
// requested and predication suppresses it exactly when the result equals
// PredicateValue. D3D11.3 8.4: "Which Instance to start sequentially fetching
// from in each Buffer marked as Instance Data." 8.5: "A negative
// BaseVertexLocation allows, for example, the first vertex to be referenced by
// an index value > 0." 8.9: "If the BufferFilledSize is less then or equal to
// the specified buffer offset, then nothing is drawn." (wording preserved from
// the specification.)
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// ID3D10Device::SetPredication: "not actually performed if the resulting
// Predicate data of the Predicate is equal to the PredicateValue." No hint flag
// is used. GetData/Map synchronize, without a timing assumption.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-setpredication
// each fetched vertex supplies its column and each fetched instance its row.
// The pixel contains its linear coordinate plus one, leaving zero
// distinguishable as an untouched pixel. Both index formats use a reversed
// array with a prefix; CPU address calculations follow 8.4.1/8.6.1,
// independently of shader-generated IDs. 8.19.2: "the return is 0 in all
// non-missing components of the format" for an out-of-bounds vertex element.
// IA vertex-buffer slots are D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT or D3D10_1_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT.
// binding the whole range must feed the last slots just as binding them individually does.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-iasetvertexbuffers
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

static const char hlsl[] = R"hlsl(
struct V { float4 position : SV_Position; nointerpolation uint value : VALUE; };
V vs(float column : COLUMN, uint row : ROW) {
  V v;
  v.position = float4((column + 0.5) * 2 / WIDTH - 1, 1 - (row + 0.5) * 2 / HEIGHT, 0, 1);
  v.value = uint(column) + row * WIDTH + 1;
  return v;
}
V replay(float4 position : POSITION, uint value : VALUE) {
  V v; v.position = position; v.value = value; return v;
}
[maxvertexcount(1)] void gs(point V vertices[1], inout PointStream<V> stream) { stream.Append(vertices[0]); }
uint ps(V v) : SV_Target { return v.value; }
)hlsl";

static HRESULT
result(ID3D10Asynchronous *query, void *data, UINT bytes) {
  HRESULT hr;
  do {
    hr = query->GetData(data, bytes, 0);
  } while (hr == S_FALSE);
  return hr;
}

int
main() {
  constexpr UINT width = D3D10_VS_INPUT_REGISTER_COUNT * 2 + 1, height = D3D10_SO_BUFFER_SLOT_COUNT + 1;
  struct Shaders {
    ComPtr<ID3DBlob> vs, replay, gs, ps;
  } shaders[2];
  const char *profiles[] = {"4_0", "4_1"};
  for (UINT i = 0; i < std::size(shaders); i++) {
    std::vector<std::string> defines = {"WIDTH=" + std::to_string(width), "HEIGHT=" + std::to_string(height)};
    shaders[i] = {
        compile(hlsl, "vs", (std::string("vs_") + profiles[i]).c_str(), defines),
        compile(hlsl, "replay", (std::string("vs_") + profiles[i]).c_str(), defines),
        compile(hlsl, "gs", (std::string("gs_") + profiles[i]).c_str(), defines),
        compile(hlsl, "ps", (std::string("ps_") + profiles[i]).c_str(), defines)
    };
    if (!expect(
            shaders[i].vs && shaders[i].replay && shaders[i].gs && shaders[i].ps, "SM%s did not compile", profiles[i]
        ))
      return verdict();
  }
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")
  );
  if (!expect(create != nullptr, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  for (auto level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, level, D3D10_1_SDK_VERSION, &device));
    std::vector<float> columns(width + 1);
    std::vector<UINT> rows(height + 1);
    for (UINT i = 0; i < columns.size(); i++)
      columns[i] = float(i) - 1;
    for (UINT i = 0; i < rows.size(); i++)
      rows[i] = i ? i - 1 : height;
    auto make_buffer = [&](const void *data, UINT bytes, UINT bind) {
      D3D10_BUFFER_DESC desc{bytes, D3D10_USAGE_DEFAULT, bind};
      D3D10_SUBRESOURCE_DATA initial{data};
      ComPtr<ID3D10Buffer> buffer;
      HRESULT hr = device->CreateBuffer(&desc, &initial, &buffer);
      expect(hr == S_OK && buffer, "CreateBuffer returned %#lx", hr);
      return buffer;
    };
    auto vertices = make_buffer(columns.data(), columns.size() * sizeof(float), D3D10_BIND_VERTEX_BUFFER);
    auto instances = make_buffer(rows.data(), rows.size() * sizeof(UINT), D3D10_BIND_VERTEX_BUFFER);
    if (!vertices || !instances)
      return verdict();
    D3D10_TEXTURE2D_DESC desc{
        width, height, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET
    };
    ComPtr<ID3D10Texture2D> target, staging;
    ComPtr<ID3D10RenderTargetView> rtv;
    CHECK(device->CreateTexture2D(&desc, nullptr, &target));
    CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    desc.Usage = D3D10_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
    const float clear[4]{};
    auto read_pixels = [&](const std::vector<UINT> &wanted) {
      device->CopyResource(staging.Get(), target.Get());
      D3D10_MAPPED_TEXTURE2D mapped{};
      auto hr = staging->Map(0, D3D10_MAP_READ, 0, &mapped);
      if (!expect(hr == S_OK && mapped.pData, "readback Map returned %#lx", hr))
        return false;
      bool same = true;
      for (UINT y = 0; y < height; y++) {
        auto row = reinterpret_cast<const UINT *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
        for (UINT x = 0; x < width; x++)
          same &= expect(
              row[x] == wanted[y * width + x], "pixel (%u,%u)=%u expected %u", x, y, row[x], wanted[y * width + x]
          );
      }
      staging->Unmap(0);
      return same;
    };
    D3D10_VIEWPORT viewport{0, 0, width, height, 0, 1};
    D3D10_INPUT_ELEMENT_DESC elements[] = {
        {"COLUMN", 0, DXGI_FORMAT_R32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"ROW", 0, DXGI_FORMAT_R32_UINT, 1, 0, D3D10_INPUT_PER_INSTANCE_DATA, 1}
    };
    ID3D10Buffer *buffers[] = {vertices.Get(), instances.Get()};
    const UINT strides[] = {sizeof(float), sizeof(UINT)}, offsets[] = {sizeof(float), sizeof(UINT)};
    for (UINT profile = 0; profile < std::size(shaders); profile++) {
      if (profile && level == D3D10_FEATURE_LEVEL_10_0)
        continue;
      device->RSSetViewports(1, &viewport);
      device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_POINTLIST);
      device->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      auto &code = shaders[profile];
      ComPtr<ID3D10VertexShader> vs, replay;
      ComPtr<ID3D10GeometryShader> gs;
      ComPtr<ID3D10PixelShader> ps;
      ComPtr<ID3D10InputLayout> layout, replay_layout;
      CHECK(device->CreateVertexShader(code.vs->GetBufferPointer(), code.vs->GetBufferSize(), &vs));
      CHECK(device->CreateVertexShader(code.replay->GetBufferPointer(), code.replay->GetBufferSize(), &replay));
      CHECK(device->CreateGeometryShader(code.gs->GetBufferPointer(), code.gs->GetBufferSize(), &gs));
      CHECK(device->CreatePixelShader(code.ps->GetBufferPointer(), code.ps->GetBufferSize(), &ps));
      CHECK(device->CreateInputLayout(
          elements, std::size(elements), code.vs->GetBufferPointer(), code.vs->GetBufferSize(), &layout
      ));
      for (bool geometry : {false, true}) {
        device->VSSetShader(vs.Get());
        device->PSSetShader(ps.Get());
        device->GSSetShader(geometry ? gs.Get() : nullptr);
        const UINT slot_count = level == D3D10_FEATURE_LEVEL_10_1 ? D3D10_1_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
                                                                : D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
        D3D10_INPUT_ELEMENT_DESC high_elements[std::size(elements)];
        std::copy_n(elements, std::size(elements), high_elements);
        std::vector<ID3D10Buffer *> high_buffers(slot_count);
        std::vector<UINT> high_strides(slot_count), high_offsets(slot_count);
        for (UINT i = 0; i < std::size(elements); i++) {
          UINT slot = slot_count - std::size(elements) + i;
          high_elements[i].InputSlot = slot;
          high_buffers[slot] = buffers[i];
          high_strides[slot] = strides[i];
          high_offsets[slot] = offsets[i];
        }
        ComPtr<ID3D10InputLayout> high_layout;
        CHECK(device->CreateInputLayout(
            high_elements, std::size(high_elements), code.vs->GetBufferPointer(), code.vs->GetBufferSize(), &high_layout
        ));
        step("level %#x profile %u geometry %u: draw from the last slots of %u bound buffers", level, profile, geometry,
             slot_count);
        device->IASetInputLayout(high_layout.Get());
        device->IASetVertexBuffers(0, slot_count, high_buffers.data(), high_strides.data(), high_offsets.data());
        device->ClearRenderTargetView(rtv.Get(), clear);
        device->DrawInstanced(1, 1, 1, 1);
        std::vector<UINT> high_want(width * height);
        high_want[width + 1] = width + 2;
        if (!read_pixels(high_want))
          return verdict();
        device->IASetInputLayout(layout.Get());
        device->IASetVertexBuffers(0, std::size(buffers), buffers, strides, offsets);
        for (auto index_format : {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R32_UINT}) {
          const UINT index_bytes = index_format == DXGI_FORMAT_R16_UINT ? sizeof(UINT16) : sizeof(UINT);
          std::vector<UINT16> short_indices(width + 2);
          std::vector<UINT> long_indices(short_indices.size());
          for (UINT i = 0; i < long_indices.size(); i++)
            short_indices[i] = UINT16(long_indices[i] = long_indices.size() - 1 - i);
          auto indices = make_buffer(
              index_bytes == sizeof(UINT16) ? static_cast<const void *>(short_indices.data())
                                            : static_cast<const void *>(long_indices.data()),
              long_indices.size() * index_bytes, D3D10_BIND_INDEX_BUFFER
          );
          if (!indices)
            return verdict();
          device->IASetIndexBuffer(indices.Get(), index_format, index_bytes);
          for (UINT operation = 0; operation < 4; operation++) {
            const bool indexed = operation % 2, instanced = operation >= 2;
            for (UINT count : {0u, 1u, width / 2 - 1, width / 2, width / 2 + 1, width - 1}) {
              for (UINT instance_count : {0u, 1u, height - 1}) {
                if (!instanced && instance_count != 1)
                  continue;
                for (INT base : {-1, 0, 1}) {
                  if (!indexed && base)
                    continue;
                  const UINT first = 1, first_instance = instanced ? 1 : 0;
                  step(
                      "level %#x SM%s GS %u draw %u format %u count %u "
                      "instances %u base %d",
                      level, profiles[profile], geometry, operation, index_format, count, instance_count, base
                  );
                  device->ClearRenderTargetView(rtv.Get(), clear);
                  switch (operation) {
                  case 0:
                    device->Draw(count, first);
                    break;
                  case 1:
                    device->DrawIndexed(count, first, base);
                    break;
                  case 2:
                    device->DrawInstanced(count, instance_count, first, first_instance);
                    break;
                  case 3:
                    device->DrawIndexedInstanced(count, instance_count, first, base, first_instance);
                    break;
                  }
                  std::vector<UINT> wanted(width * height);
                  for (UINT instance = 0; instance < instance_count; instance++) {
                    UINT row = rows[1 + first_instance + instance];
                    for (UINT v = 0; v < count; v++) {
                      UINT fetched = indexed ? UINT(INT(long_indices[1 + first + v]) + base) : first + v;
                      UINT column = 1 + fetched < columns.size() ? UINT(columns[1 + fetched]) : 0;
                      if (column < width && row < height)
                        wanted[row * width + column] = row * width + column + 1;
                    }
                  }
                  read_pixels(wanted);
                }
              }
            }
            for (bool visible : {false, true}) {
              D3D10_QUERY_DESC query_desc{D3D10_QUERY_OCCLUSION_PREDICATE};
              ComPtr<ID3D10Predicate> predicate;
              CHECK(device->CreatePredicate(&query_desc, &predicate));
              predicate->Begin();
              if (visible)
                device->Draw(1, 0);
              predicate->End();
              BOOL query_result = !visible;
              CHECK(result(predicate.Get(), &query_result, sizeof(query_result)));
              expect(!!query_result == visible, "occlusion predicate result");
              for (BOOL skip : {FALSE, TRUE}) {
                device->ClearRenderTargetView(rtv.Get(), clear);
                device->SetPredication(predicate.Get(), skip);
                switch (operation) {
                case 0:
                  device->Draw(1, 0);
                  break;
                case 1:
                  device->DrawIndexed(1, 1, 0);
                  break;
                case 2:
                  device->DrawInstanced(1, 1, 0, 0);
                  break;
                case 3:
                  device->DrawIndexedInstanced(1, 1, 1, 0, 0);
                  break;
                }
                device->SetPredication(nullptr, skip);
                ComPtr<ID3D10Predicate> bound;
                BOOL preserved = !skip;
                device->GetPredication(&bound, &preserved);
                expect(!bound && preserved == skip, "null predication did not preserve its value");
                std::vector<UINT> wanted(width * height);
                if (visible != !!skip) {
                  UINT column = indexed ? long_indices[2] : 0;
                  wanted[column] = column + 1;
                }
                read_pixels(wanted);
              }
            }
          }
        }
      }

      struct StreamVertex {
        float position[4];
        UINT value;
      };
      const UINT capacity = width + 1, stream_bytes = capacity * sizeof(StreamVertex);
      std::vector<UINT> poison(stream_bytes / sizeof(UINT), std::numeric_limits<UINT>::max());
      auto stream = make_buffer(poison.data(), stream_bytes, D3D10_BIND_STREAM_OUTPUT | D3D10_BIND_VERTEX_BUFFER);
      if (!stream)
        return verdict();
      D3D10_BUFFER_DESC read_desc{stream_bytes, D3D10_USAGE_STAGING, 0, D3D10_CPU_ACCESS_READ};
      ComPtr<ID3D10Buffer> readback;
      CHECK(device->CreateBuffer(&read_desc, nullptr, &readback));
      D3D10_SO_DECLARATION_ENTRY declaration[] = {{"SV_Position", 0, 0, 4, 0}, {"VALUE", 0, 0, 1, 0}};
      ComPtr<ID3D10GeometryShader> capture;
      CHECK(device->CreateGeometryShaderWithStreamOutput(
          code.gs->GetBufferPointer(), code.gs->GetBufferSize(), declaration, std::size(declaration),
          sizeof(StreamVertex), &capture
      ));
      D3D10_INPUT_ELEMENT_DESC replay_elements[] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0},
          {"VALUE", 0, DXGI_FORMAT_R32_UINT, 0, sizeof(StreamVertex::position), D3D10_INPUT_PER_VERTEX_DATA, 0}
      };
      CHECK(device->CreateInputLayout(
          replay_elements, std::size(replay_elements), code.replay->GetBufferPointer(), code.replay->GetBufferSize(),
          &replay_layout
      ));
      step(
          "level %#x SM%s newly created stream buffer has zero filled size "
          "(14.4)",
          level, profiles[profile]
      );
      device->GSSetShader(nullptr);
      device->VSSetShader(replay.Get());
      device->IASetInputLayout(replay_layout.Get());
      UINT replay_stride = sizeof(StreamVertex), zero = 0;
      device->IASetVertexBuffers(0, 1, stream.GetAddressOf(), &replay_stride, &zero);
      device->IASetVertexBuffers(1, 1, nullptr, &zero, &zero);
      device->ClearRenderTargetView(rtv.Get(), clear);
      device->DrawAuto();
      read_pixels(std::vector<UINT>(width * height));
      for (UINT count : {0u, 1u, width - 1, width, width + 1}) {
        step("level %#x SM%s stream output append/overflow/DrawAuto count %u", level, profiles[profile], count);
        device->VSSetShader(vs.Get());
        device->GSSetShader(capture.Get());
        device->IASetInputLayout(layout.Get());
        device->IASetVertexBuffers(0, std::size(buffers), buffers, strides, offsets);
        device->UpdateSubresource(stream.Get(), 0, nullptr, poison.data(), 0, 0);
        UINT offset = sizeof(StreamVertex);
        device->SOSetTargets(1, stream.GetAddressOf(), &offset);
        D3D10_QUERY_DESC statistics_desc{D3D10_QUERY_SO_STATISTICS}, overflow_desc{D3D10_QUERY_SO_OVERFLOW_PREDICATE};
        ComPtr<ID3D10Query> statistics;
        ComPtr<ID3D10Predicate> overflow;
        CHECK(device->CreateQuery(&statistics_desc, &statistics));
        CHECK(device->CreatePredicate(&overflow_desc, &overflow));
        statistics->Begin();
        overflow->Begin();
        const UINT first_count = count / 2;
        device->Draw(first_count, 0);
        offset = std::numeric_limits<UINT>::max();
        device->SOSetTargets(1, stream.GetAddressOf(), &offset);
        device->Draw(count - first_count, first_count);
        overflow->End();
        statistics->End();
        device->SOSetTargets(0, nullptr, nullptr);
        D3D10_QUERY_DATA_SO_STATISTICS data{};
        BOOL lost = FALSE;
        CHECK(result(statistics.Get(), &data, sizeof(data)));
        CHECK(result(overflow.Get(), &lost, sizeof(lost)));
        expect(
            data.NumPrimitivesWritten == std::min(count, width) && data.PrimitivesStorageNeeded == count,
            "SO statistics %llu/%llu expected %u/%u", data.NumPrimitivesWritten, data.PrimitivesStorageNeeded,
            std::min(count, width), count
        );
        expect(!!lost == (count > width), "SO overflow predicate");
        device->CopyResource(readback.Get(), stream.Get());
        void *mapped;
        CHECK(readback->Map(D3D10_MAP_READ, 0, &mapped));
        auto bytes = static_cast<const char *>(mapped);
        expect(!memcmp(bytes, poison.data(), sizeof(StreamVertex)), "SO overwrote bytes before its starting offset");
        const UINT written = std::min(count, width);
        for (UINT i = 0; i < written; i++) {
          StreamVertex got;
          memcpy(&got, bytes + (i + 1) * sizeof(got), sizeof(got));
          const float position[] = {(float(i) + 0.5f) * 2 / width - 1, 1 - 1.0f / height, 0, 1};
          // four arithmetic operations, each bounded by D3D11.3 3.1.3's float
          // precision, with intermediates <= 2.
          constexpr UINT position_operations = 4, maximum_intermediate = 2;
          constexpr float error = position_operations * maximum_intermediate * std::numeric_limits<float>::epsilon();
          for (UINT component = 0; component < std::size(position); component++)
            expect(
                std::abs(got.position[component] - position[component]) <= error, "SO vertex %u position component %u",
                i, component
            );
          expect(got.value == i + 1, "SO vertex %u value %u", i, got.value);
        }
        const UINT tail = (written + 1) * sizeof(StreamVertex);
        expect(
            !memcmp(bytes + tail, reinterpret_cast<const char *>(poison.data()) + tail, stream_bytes - tail),
            "SO overwrote bytes after its last vertex"
        );
        readback->Unmap();
        device->GSSetShader(nullptr);
        device->VSSetShader(replay.Get());
        device->IASetInputLayout(replay_layout.Get());
        UINT stride = sizeof(StreamVertex);
        for (UINT start : {0u, 1u, std::min(count, width), std::min(count, width) + 1}) {
          offset = (start + 1) * sizeof(StreamVertex);
          device->IASetVertexBuffers(0, 1, stream.GetAddressOf(), &stride, &offset);
          device->IASetVertexBuffers(1, 1, nullptr, &zero, &zero);
          device->ClearRenderTargetView(rtv.Get(), clear);
          device->DrawAuto();
          std::vector<UINT> wanted(width * height);
          for (UINT i = start; i < std::min(count, width); i++)
            wanted[i] = i + 1;
          read_pixels(wanted);
        }
      }
      device->ClearState();
    }
    device->ClearState();
    expect(device->GetDeviceRemovedReason() == S_OK, "device was removed after draws");
  }
  return verdict();
}
