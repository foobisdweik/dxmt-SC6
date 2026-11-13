// contract: each supported typed load/sample decodes the SDK layout, and stores
// and pixel outputs encode it. "The starting n-bit value is converted to float
// (0.0f, 1.0f, 2.0f, etc.) and then divided by (2n-1)." (D3D11.3 3.2.3.5).
// "Denorms: 16-bit floating point numbers must preserve denorms." (3.1.5).
// "Round-to-zero must be used during conversion to another float format."
// (3.2.2). normalized stores allow "0.6f ULP" (3.2.3.4/.6/.8); sRGB decode
// allows "0.5f ULP" on the encoded side (3.2.3.7). layouts, defaults, signed
// extension and RGB9E5 follow 19.1.3, 3.2.3.9/.12 and 19.3.2.1. all 8/16-bit
// codes, packed 16-bit words, small float component codes and RGB9E5
// exponent/mantissa pairs are enumerated. 32-bit values cross every bit and
// exponent boundary. CPU expectations do not use the translator's format table.
// "the input and the output data must be identical in memory" (19.1.2), except
// the extra SNORM minimum and NaN payloads (19.1.2.1). loads and point/linear
// samples at texel centers feed the same-format target without CPU conversion.
#include "d3d11_test.hpp"
#include "../depth_formats.hpp"
#include "../format_values.hpp"

using namespace format_values;

int
main() {
  ComPtr<ID3DBlob> codes[3][7], pixels[3][4];
  auto vertex = compile(hlsl, "vs", "vs", defines(0, 0));
  if (!expect(bool(vertex), "vertex HLSL did not compile"))
    return verdict();
  for (UINT kind = 0; kind < std::size(codes); kind++) {
    for (UINT mode = 0; mode < std::size(pixels[kind]); mode++) {
      if (kind && mode >= 2)
        continue;
      pixels[kind][mode] = compile(hlsl, "ps", "ps", defines(kind, mode + 7));
      if (!expect(bool(pixels[kind][mode]), "pixel HLSL did not compile"))
        return verdict();
    }
    for (UINT mode = 0; mode < std::size(codes[kind]); mode++) {
      if (kind && mode == 1)
        continue;
      codes[kind][mode] = compile(hlsl, "cs", "cs", defines(kind, mode));
      if (!expect(bool(codes[kind][mode]), "compute HLSL did not compile"))
        return verdict();
    }
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate
  ));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps[3][4];
  ComPtr<ID3D11ComputeShader> cs[3][7];
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs));
  for (UINT kind = 0; kind < std::size(cs); kind++) {
    for (UINT mode = 0; mode < std::size(ps[kind]); mode++)
      if (pixels[kind][mode])
        CHECK(device->CreatePixelShader(
            pixels[kind][mode]->GetBufferPointer(), pixels[kind][mode]->GetBufferSize(), nullptr, &ps[kind][mode]
        ));
    for (UINT mode = 0; mode < std::size(cs[kind]); mode++)
      if (codes[kind][mode])
        CHECK(device->CreateComputeShader(
            codes[kind][mode]->GetBufferPointer(), codes[kind][mode]->GetBufferSize(), nullptr, &cs[kind][mode]
        ));
  }
  D3D11_SAMPLER_DESC sampler_desc{
      D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
      D3D11_TEXTURE_ADDRESS_CLAMP
  };
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ComPtr<ID3D11SamplerState> sampler[2];
  CHECK(device->CreateSamplerState(&sampler_desc, &sampler[0]));
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  CHECK(device->CreateSamplerState(&sampler_desc, &sampler[1]));
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));

  UINT tested = 0;
  for (const auto &f : formats()) {
    UINT support = 0;
    HRESULT caps = device->CheckFormatSupport(f.id, &support);
    step("%s: capabilities", f.name.c_str());
    if (FAILED(caps)) {
      expect(caps == E_FAIL && !support, "unsupported format hr %08lx, bits %#x", caps, support);
      continue;
    }
    if (!(support & D3D11_FORMAT_SUPPORT_TEXTURE2D))
      continue;
    if (f.kind == Shared)
      expect(
          !(support & (D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)),
          "RGB9E5 advertises forbidden writes (D3D11.3 19.3.2)"
      );
    auto bytes = inputs(f);
    auto values = sources(f, bytes);
    const UINT width = 1u << CHAR_BIT, count = UINT(values.size()), height = std::bit_ceil((count + width - 1) / width);
    const UINT pixels_count = width * height, kind = f.shader_kind(), group = std::tuple_size_v<Pixel>;
    bytes.resize(pixels_count * f.bytes);
    values.resize(pixels_count);
    for (UINT i = count; i < pixels_count; i++)
      values[i] = decode(f, bytes.data() + i * f.bytes);
    D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, f.id, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    D3D11_SUBRESOURCE_DATA initial{bytes.data(), width * f.bytes};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> texture_srv;
    CHECK(device->CreateTexture2D(&desc, &initial, &texture));
    CHECK(device->CreateShaderResourceView(texture.Get(), nullptr, &texture_srv));
    auto filtered_bytes = linear_inputs(f, bytes);
    auto inverse_bytes = bytes;
    for (auto &byte : inverse_bytes)
      byte = ~byte;
    D3D11_SUBRESOURCE_DATA filtered_initial{filtered_bytes.data(), width * f.bytes};
    D3D11_SUBRESOURCE_DATA inverse_initial{inverse_bytes.data(), width * f.bytes};
    ComPtr<ID3D11Texture2D> filtered;
    ComPtr<ID3D11ShaderResourceView> filtered_srv;
    CHECK(device->CreateTexture2D(&desc, &filtered_initial, &filtered));
    CHECK(device->CreateShaderResourceView(filtered.Get(), nullptr, &filtered_srv));
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2{f.id};
    CHECK(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support2, sizeof(support2)));
    auto make_buffer = [&](UINT bind, bool structured, const void *data, UINT size, ComPtr<ID3D11Buffer> &out) {
      D3D11_BUFFER_DESC bd{
          size,
          D3D11_USAGE_DEFAULT,
          bind,
          0,
          structured ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED : 0u,
          structured ? UINT(sizeof(Pixel)) : 0u
      };
      D3D11_SUBRESOURCE_DATA bd_data{data};
      return device->CreateBuffer(&bd, data ? &bd_data : nullptr, &out);
    };
    ComPtr<ID3D11Buffer> source, result, shape;
    ComPtr<ID3D11ShaderResourceView> source_srv;
    ComPtr<ID3D11UnorderedAccessView> result_uav;
    CHECK(make_buffer(D3D11_BIND_SHADER_RESOURCE, true, values.data(), pixels_count * sizeof(Pixel), source));
    CHECK(device->CreateShaderResourceView(source.Get(), nullptr, &source_srv));
    std::vector<Pixel> poison(pixels_count, Pixel{~0u, ~0u, ~0u, ~0u});
    CHECK(make_buffer(D3D11_BIND_UNORDERED_ACCESS, true, poison.data(), pixels_count * sizeof(Pixel), result));
    CHECK(device->CreateUnorderedAccessView(result.Get(), nullptr, &result_uav));
    const UINT dimensions[4] = {width, pixels_count};
    CHECK(make_buffer(D3D11_BIND_CONSTANT_BUFFER, false, dimensions, sizeof(dimensions), shape));
    for (bool listed : {false, true})
      for (UINT mode = 0; mode <= 10; mode++) {
        const bool typed_buffer = mode == 2 || mode == 5 || mode == 6;
        const bool store = mode == 4 || mode == 6 || mode >= 7;
        const bool unordered = mode >= 3 && mode <= 6;
        if (f.kind == Shared && (unordered || store))
          continue;
        if ((mode == 0 || mode == 8) && !(support & D3D11_FORMAT_SUPPORT_SHADER_LOAD))
          continue;
        if ((mode == 1 || mode >= 9) && (kind || !(support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)))
          continue;
        if (typed_buffer && !(support & D3D11_FORMAT_SUPPORT_BUFFER))
          continue;
        if (unordered && !(support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW))
          continue;
        if ((mode == 3 || mode == 5) && !(support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
          continue;
        if ((mode == 4 || mode == 6) && !(support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE))
          continue;
        if (mode >= 7 && !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
          continue;
        step(
            "%s: mode %u, %s, %u values (%ux%u)", f.name.c_str(), mode, listed ? "deferred" : "immediate", pixels_count,
            width, height
        );
        auto context = listed ? deferred.Get() : immediate.Get();
        context->ClearState();
        ComPtr<ID3D11Texture2D> target;
        ComPtr<ID3D11Buffer> typed;
        ComPtr<ID3D11Resource> read_source;
        ComPtr<ID3D11ShaderResourceView> srv = mode == 10 ? filtered_srv : texture_srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (typed_buffer) {
          CHECK(make_buffer(
              unordered ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_SHADER_RESOURCE, false, bytes.data(),
              UINT(bytes.size()), typed
          ));
          if (unordered) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC view{f.id, D3D11_UAV_DIMENSION_BUFFER};
            view.Buffer.NumElements = pixels_count;
            CHECK(device->CreateUnorderedAccessView(typed.Get(), &view, &uav));
          } else {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{f.id, D3D11_SRV_DIMENSION_BUFFER};
            view.Buffer.NumElements = pixels_count;
            CHECK(device->CreateShaderResourceView(typed.Get(), &view, &srv));
          }
          CHECK(typed.As(&read_source));
        } else if (unordered || mode >= 7) {
          desc.BindFlags = mode >= 7 ? D3D11_BIND_RENDER_TARGET : D3D11_BIND_UNORDERED_ACCESS;
          CHECK(device->CreateTexture2D(&desc, mode >= 8 ? &inverse_initial : &initial, &target));
          if (mode >= 7) {
            CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
          } else {
            CHECK(device->CreateUnorderedAccessView(target.Get(), nullptr, &uav));
          }
          CHECK(target.As(&read_source));
        }
        if (mode >= 7) {
          context->VSSetShader(vs.Get(), nullptr, 0);
          context->PSSetShader(ps[kind][mode - 7].Get(), nullptr, 0);
          context->PSSetShaderResources(0, 1, srv.GetAddressOf());
          context->PSSetShaderResources(1, 1, source_srv.GetAddressOf());
          context->PSSetSamplers(mode == 10, 1, sampler[mode == 10].GetAddressOf());
          context->PSSetConstantBuffers(0, 1, shape.GetAddressOf());
          context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
          context->RSSetState(raster.Get());
          D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
          context->RSSetViewports(1, &viewport);
          context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          context->Draw(3, 0);
        } else {
          context->UpdateSubresource(result.Get(), 0, nullptr, poison.data(), 0, 0);
          ID3D11ShaderResourceView *srvs[] = {unordered ? nullptr : srv.Get(), source_srv.Get()};
          ID3D11UnorderedAccessView *uavs[] = {result_uav.Get(), uav.Get()};
          context->CSSetShader(cs[kind][mode].Get(), nullptr, 0);
          context->CSSetShaderResources(0, UINT(std::size(srvs)), srvs);
          context->CSSetUnorderedAccessViews(0, UINT(std::size(uavs)), uavs, nullptr);
          context->CSSetConstantBuffers(0, 1, shape.GetAddressOf());
          context->CSSetSamplers(0, 1, sampler[0].GetAddressOf());
          context->Dispatch((width + group - 1) / group, (height + group - 1) / group, 1);
        }
        context->ClearState();
        ComPtr<ID3D11Texture2D> staging;
        ComPtr<ID3D11Buffer> staged_buffer;
        if (store) {
          if (typed_buffer) {
            D3D11_BUFFER_DESC bd{UINT(bytes.size()), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
            CHECK(device->CreateBuffer(&bd, nullptr, &staged_buffer));
            context->CopyResource(staged_buffer.Get(), read_source.Get());
          } else {
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
            context->CopyResource(staging.Get(), read_source.Get());
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.CPUAccessFlags = 0;
          }
        }
        if (listed) {
          ComPtr<ID3D11CommandList> commands;
          CHECK(deferred->FinishCommandList(FALSE, &commands));
          immediate->ExecuteCommandList(commands.Get(), FALSE);
        }
        bool same = true;
        if (store) {
          auto resource = typed_buffer ? static_cast<ID3D11Resource *>(staged_buffer.Get())
                                       : static_cast<ID3D11Resource *>(staging.Get());
          D3D11_MAPPED_SUBRESOURCE mapped{};
          CHECK(immediate->Map(resource, 0, D3D11_MAP_READ, 0, &mapped));
          for (UINT i = 0; i < pixels_count; i++)
            if (!(mode >= 8
                      ? inverted(
                            f, (mode == 10 ? filtered_bytes : bytes).data() + i * f.bytes,
                            static_cast<const uint8_t *>(mapped.pData) + (i / width) * mapped.RowPitch +
                                (i % width) * f.bytes,
                            mode >= 9
                        )
                      : stored(
                            f, values[i],
                            static_cast<const uint8_t *>(mapped.pData) +
                                (typed_buffer ? i * f.bytes : (i / width) * mapped.RowPitch + (i % width) * f.bytes)
                        ))) {
              expect(false, "encoded value %u does not match the CPU conversion", i);
              same = false;
              break;
            }
          if (same && mode < 8)
            same = expect(
                ordered(
                    f, values, static_cast<const uint8_t *>(mapped.pData), typed_buffer ? pixels_count : width,
                    typed_buffer ? 0 : mapped.RowPitch
                ),
                "normalized conversion decreases or maps equal values differently"
            );
          immediate->Unmap(resource, 0);
        } else {
          auto have = read(device.Get(), immediate.Get(), result.Get());
          if (!expect(have.size() == pixels_count * std::tuple_size_v<Pixel>, "readback is incomplete"))
            return verdict();
          for (UINT i = 0; i < pixels_count; i++) {
            Pixel pixel;
            memcpy(pixel.data(), have.data() + i * pixel.size(), sizeof(pixel));
            if (!loaded(f, bytes.data() + i * f.bytes, pixel, mode == 1)) {
              auto want = decode(f, bytes.data() + i * f.bytes);
              expect(
                  false,
                  "value %u has %08x %08x %08x %08x, expected %08x %08x "
                  "%08x %08x",
                  i, pixel[0], pixel[1], pixel[2], pixel[3], want[0], want[1], want[2], want[3]
              );
              same = false;
              break;
            }
          }
        }
        tested++;
        if (!same)
          return verdict();
      }
  }
  expect(tested != 0, "no format operations ran");
  return verdict();
}
