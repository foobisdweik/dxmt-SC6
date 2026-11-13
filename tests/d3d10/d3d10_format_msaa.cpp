// contract: native Direct3D 10/10.1 multisample caps agree with creation, sample loads and resolve bytes.
// "SampleMask is a 32-bit coverage mask applied to the Multisample coverage" (D3D11.3 17.17).
// "as of D3D10.1, fixed sample patterns are defined for certain sample counts" and their resolve is
// "a simple average of the samples within each pixel" (19.2.4). each sample receives a native CPU palette
// value through a separate masked draw. vendor-defined resolves use identical sample values (19.2.3).
// the four fixed-pattern counts, first/last quality, both feature levels and every advertised color format
// share the conversion and sRGB error bounds of 3.2 and the 19.1.2 invertibility contract.
// SM4.0 requires a declared sample count and literal sample indices (22.4.7); SM4.1 removes both restrictions.
// unsupported counts and qualities at/past the advertised range must fail creation (19.2.3/.4).
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include "../depth_formats.hpp"
#include "../format_msaa.hpp"

using namespace format_values;

static const char shaders[] = R"hlsl(
cbuffer Shape : register(b0) { uint samples; uint only; };
#ifndef SAMPLE_COUNT
#define SAMPLE_COUNT 2
#endif
#if SAMPLE_COUNT
Texture2DMS<TYPE,SAMPLE_COUNT> tex : register(t0);
#else
Texture2DMS<TYPE> tex : register(t0);
#endif
Buffer<uint4> source : register(t1);
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id==2?3:-1,id==1?3:-1,0,1); }
TYPE ps() : SV_Target { return UNPACK(source[only]); }
uint4 read_sample(float4 p : SV_Position) : SV_Target {
  uint x = uint(p.x);
  int2 coords = int2(x/samples,uint(p.y)); uint index = x%samples;
#if SAMPLE_COUNT
#define LOAD(I) if(index == I) return asuint(tex.Load(coords, I < SAMPLE_COUNT ? I : 0));
  LOAD(0) LOAD(1) LOAD(2) LOAD(3) LOAD(4) LOAD(5) LOAD(6) LOAD(7)
  LOAD(8) LOAD(9) LOAD(10) LOAD(11) LOAD(12) LOAD(13) LOAD(14) LOAD(15)
  LOAD(16) LOAD(17) LOAD(18) LOAD(19) LOAD(20) LOAD(21) LOAD(22) LOAD(23)
  LOAD(24) LOAD(25) LOAD(26) LOAD(27) LOAD(28) LOAD(29) LOAD(30) LOAD(31)
  return 0;
#else
  return asuint(tex.Load(coords,index));
#endif
}
)hlsl";

int
main() {
  auto vertex = compile(shaders, "vs", "vs_4_0", defines(0, 0));
  static_assert(D3D10_MAX_MULTISAMPLE_SAMPLE_COUNT == 32);
  ComPtr<ID3DBlob> writes[3], reads[3][D3D10_MAX_MULTISAMPLE_SAMPLE_COUNT + 1];
  for (UINT kind = 0; kind < std::size(writes); kind++) {
    writes[kind] = compile(shaders, "ps", "ps_4_0", defines(kind, 0));
    if (!expect(bool(writes[kind]), "MSAA write HLSL did not compile"))
      return verdict();
    for (UINT count = 0; count <= D3D10_MAX_MULTISAMPLE_SAMPLE_COUNT; count++) {
      if (count == 1)
        continue;
      auto macros = defines(kind, 0);
      macros.push_back("SAMPLE_COUNT=" + std::to_string(count));
      reads[kind][count] = compile(shaders, "read_sample", count ? "ps_4_0" : "ps_4_1", macros);
      if (!expect(bool(reads[kind][count]), "MSAA read HLSL did not compile for %u samples", count))
        return verdict();
    }
  }
  if (!expect(bool(vertex), "vertex HLSL did not compile"))
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
    ComPtr<ID3D10PixelShader> ps[3], read_ps[3][D3D10_MAX_MULTISAMPLE_SAMPLE_COUNT + 1];
    CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), &vs));
    for (UINT kind = 0; kind < std::size(ps); kind++) {
      CHECK(device->CreatePixelShader(writes[kind]->GetBufferPointer(), writes[kind]->GetBufferSize(), &ps[kind]));
      for (UINT count = 0; count <= D3D10_MAX_MULTISAMPLE_SAMPLE_COUNT; count++) {
        if (count == 1 || (level == D3D10_FEATURE_LEVEL_10_0 ? count == 0 : count != 0))
          continue;
        auto code = reads[kind][count].Get();
        CHECK(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), &read_ps[kind][count]));
      }
    }
    D3D10_RASTERIZER_DESC rd{D3D10_FILL_SOLID, D3D10_CULL_NONE};
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    ComPtr<ID3D10RasterizerState> raster;
    CHECK(device->CreateRasterizerState(&rd, &raster));
    for (const auto &f : formats()) {
      UINT caps = 0;
      if (FAILED(device->CheckFormatSupport(f.id, &caps)) || !(caps & D3D10_FORMAT_SUPPORT_RENDER_TARGET))
        continue;
      const UINT kind = f.shader_kind();
      const bool load = caps & D3D10_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
      const bool resolve = caps & D3D10_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE;
      if (!expect(f.kind != Shared, "RGB9E5 advertises forbidden rendering (19.3.2)"))
        continue;
      if (!expect(!kind || !resolve, "integer format advertises fixed-function resolve (19.2.2)"))
        continue;
      auto refused = [&](UINT samples, UINT quality) {
        D3D10_TEXTURE2D_DESC td{1, 1, 1, 1, f.id, {samples, quality}, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET};
        ComPtr<ID3D10Texture2D> target;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, &target);
        return expect(
            FAILED(hr) && !target, "%s accepted invalid count %u quality %#x: %08lx", f.name.c_str(), samples, quality,
            hr
        );
      };
      for (UINT samples : {0u, D3D10_MAX_MULTISAMPLE_SAMPLE_COUNT + 1u, UINT_MAX})
        refused(samples, 0);
      for (UINT samples = 1; samples <= D3D10_MAX_MULTISAMPLE_SAMPLE_COUNT; samples++) {
        UINT quality = 0;
        CHECK(device->CheckMultisampleQualityLevels(f.id, samples, &quality));
        if (samples == 1)
          expect(quality != 0, "%s has no single-sample quality level", f.name.c_str());
        if (!quality) {
          refused(samples, 0);
          continue;
        }
        for (UINT q : {quality, quality + (quality != UINT_MAX)})
          if (q != D3D11_STANDARD_MULTISAMPLE_PATTERN && q != D3D11_CENTER_MULTISAMPLE_PATTERN)
            refused(samples, q);
        if (samples == 1)
          continue;
        expect(caps & D3D10_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET, "quality levels without multisample target cap");
        std::vector<UINT> qualities{0, quality - 1};
        constexpr UINT max_fixed_samples = 16; // 19.2.4 defines 1, 2, 4, 8 and 16
        if (level == D3D10_FEATURE_LEVEL_10_1 && samples <= max_fixed_samples && std::has_single_bit(samples)) {
          qualities.push_back(D3D11_STANDARD_MULTISAMPLE_PATTERN);
          qualities.push_back(D3D11_CENTER_MULTISAMPLE_PATTERN);
        }
        std::sort(qualities.begin(), qualities.end());
        qualities.erase(std::unique(qualities.begin(), qualities.end()), qualities.end());
        auto raw = format_msaa::palette(f, samples);
        auto values = format_msaa::values(f, raw);
        std::array<float, 4> clear;
        if (!expect(format_msaa::clear_value(f, raw, clear), "target seed aliases a sample value"))
          return verdict();
        for (UINT q : qualities)
          for (bool uniform : {false, true}) {
            const UINT width = 3, height = 3;
            const bool fixed = q == D3D11_STANDARD_MULTISAMPLE_PATTERN || q == D3D11_CENTER_MULTISAMPLE_PATTERN;
            step(
                "D3D10 level %#x %s samples %u quality %#x %s", level, f.name.c_str(), samples, q,
                uniform ? "uniform" : "distinct"
            );
            device->ClearState();
            D3D10_TEXTURE2D_DESC td{
                width,
                height,
                1,
                1,
                f.id,
                {samples, q},
                D3D10_USAGE_DEFAULT,
                D3D10_BIND_RENDER_TARGET | (load ? D3D10_BIND_SHADER_RESOURCE : 0u)
            };
            ComPtr<ID3D10Texture2D> target, output, staging, resolved, resolved_staging;
            ComPtr<ID3D10RenderTargetView> rtv, output_rtv;
            ComPtr<ID3D10ShaderResourceView> srv, source_srv;
            CHECK(device->CreateTexture2D(&td, nullptr, &target));
            CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
            if (load)
              CHECK(device->CreateShaderResourceView(target.Get(), nullptr, &srv));
            device->ClearRenderTargetView(rtv.Get(), clear.data());
            D3D10_BUFFER_DESC bd{
                UINT(values.size() * sizeof(Pixel)), D3D10_USAGE_IMMUTABLE, D3D10_BIND_SHADER_RESOURCE
            };
            D3D10_SUBRESOURCE_DATA data{values.data()};
            ComPtr<ID3D10Buffer> source, constants;
            CHECK(device->CreateBuffer(&bd, &data, &source));
            D3D10_SHADER_RESOURCE_VIEW_DESC sv{DXGI_FORMAT_R32G32B32A32_UINT, D3D10_SRV_DIMENSION_BUFFER};
            sv.Buffer.NumElements = samples;
            CHECK(device->CreateShaderResourceView(source.Get(), &sv, &source_srv));
            UINT shape[4] = {samples};
            bd = {sizeof(shape), D3D10_USAGE_DEFAULT, D3D10_BIND_CONSTANT_BUFFER};
            CHECK(device->CreateBuffer(&bd, nullptr, &constants));
            device->VSSetShader(vs.Get());
            device->PSSetShader(ps[kind].Get());
            device->PSSetShaderResources(1, 1, source_srv.GetAddressOf());
            device->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
            device->RSSetState(raster.Get());
            D3D10_VIEWPORT viewport{0, 0, width, height, 0, 1};
            device->RSSetViewports(1, &viewport);
            device->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (UINT s = 0; s < samples; s++) {
              shape[1] = uniform ? 0 : s;
              device->UpdateSubresource(constants.Get(), 0, nullptr, shape, 0, 0);
              device->OMSetBlendState(nullptr, nullptr, 1u << s);
              device->Draw(3, 0);
            }
            device->OMSetRenderTargets(0, nullptr, nullptr);
            device->OMSetBlendState(nullptr, nullptr, D3D10_DEFAULT_SAMPLE_MASK);
            if (load) {
              td = {width * samples,         height, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D10_USAGE_DEFAULT,
                    D3D10_BIND_RENDER_TARGET};
              CHECK(device->CreateTexture2D(&td, nullptr, &output));
              CHECK(device->CreateRenderTargetView(output.Get(), nullptr, &output_rtv));
              const float poison[] = {float(UINT_MAX), float(UINT_MAX), float(UINT_MAX), float(UINT_MAX)};
              device->ClearRenderTargetView(output_rtv.Get(), poison);
              viewport.Width *= samples;
              device->RSSetViewports(1, &viewport);
              device->OMSetRenderTargets(1, output_rtv.GetAddressOf(), nullptr);
              device->PSSetShader(read_ps[kind][level == D3D10_FEATURE_LEVEL_10_0 ? samples : 0].Get());
              device->PSSetShaderResources(0, 1, srv.GetAddressOf());
              device->Draw(3, 0);
              device->ClearState();
              td.Usage = D3D10_USAGE_STAGING;
              td.BindFlags = 0;
              td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
              CHECK(device->CreateTexture2D(&td, nullptr, &staging));
              device->CopyResource(staging.Get(), output.Get());
              D3D10_MAPPED_TEXTURE2D mapped{};
              CHECK(staging->Map(0, D3D10_MAP_READ, 0, &mapped));
              bool same = true;
              for (UINT y = 0; y < height && same; y++)
                for (UINT x = 0; x < width * samples; x++) {
                  Pixel have;
                  memcpy(
                      have.data(), static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch + x * sizeof(Pixel),
                      sizeof(have)
                  );
                  if (!expect(
                          loaded(f, raw.data() + (uniform ? 0 : x % samples) * f.bytes, have),
                          "pixel %u,%u sample %u changed value/index", x / samples, y, x % samples
                      )) {
                    same = false;
                    break;
                  }
                }
              staging->Unmap(0);
              if (!same)
                return verdict();
            }
            device->ClearState();
            if (resolve && (fixed || uniform)) {
              auto expected_raw = raw;
              if (uniform)
                for (UINT s = 0; s < samples; s++)
                  memcpy(expected_raw.data() + s * f.bytes, raw.data(), f.bytes);
              auto expected_values = format_msaa::values(f, expected_raw);
              std::vector<uint8_t> seed(width * height * f.bytes);
              if (format_msaa::resolved(f, expected_raw, expected_values, seed.data()))
                std::fill(seed.begin(), seed.end(), UINT8_MAX);
              if (!expect(
                      !format_msaa::resolved(f, expected_raw, expected_values, seed.data()),
                      "resolve destination seed aliases the expected result"
                  ))
                return verdict();
              data = {seed.data(), width * f.bytes};
              td = {width, height, 1, 1, f.id, {1, 0}, D3D10_USAGE_DEFAULT};
              CHECK(device->CreateTexture2D(&td, &data, &resolved));
              td.Usage = D3D10_USAGE_STAGING;
              td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
              CHECK(device->CreateTexture2D(&td, nullptr, &resolved_staging));
              device->ResolveSubresource(resolved.Get(), 0, target.Get(), 0, f.id);
              device->CopyResource(resolved_staging.Get(), resolved.Get());
              D3D10_MAPPED_TEXTURE2D mapped{};
              CHECK(resolved_staging->Map(0, D3D10_MAP_READ, 0, &mapped));
              bool same = true;
              for (UINT y = 0; y < height && same; y++)
                for (UINT x = 0; x < width; x++)
                  if (!expect(
                          format_msaa::resolved(
                              f, expected_raw, expected_values,
                              static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch + x * f.bytes
                          ),
                          "resolved pixel %u,%u differs from CPU result", x, y
                      )) {
                    same = false;
                    break;
                  }
              resolved_staging->Unmap(0);
              if (!same)
                return verdict();
            }
            tested++;
          }
      }
    }
  }
  FreeLibrary(dll);
  expect(tested != 0, "no D3D10 multisample format ran");
  return verdict();
}
