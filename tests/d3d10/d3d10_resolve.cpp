// contract: ResolveSubresource reads the selected MSAA array slice and writes
// only its matching destination subresource, including typed/typeless
// combinations. D3D11.3 5.6.6: "Only a whole Subresource can be resolved, so
// both Subresources must be the same dimensions." 19.2.4: "a simple average of
// the samples within each pixel" defines resolve for a fixed sample pattern;
// center patterns cannot use sample-frequency execution (19.2.4.1). "Standard
// sample patterns are defined for the following sample counts: 1(trivial), 2,
// 4, 8, 16."
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// 19.2.4 requires fixed patterns whenever their sample count has nonzero
// quality levels on 10.1. The named pattern constants in d3d11.h have the
// values specified there for 10.1 as well. Vendor quality levels use constant
// colors, since their resolve algorithm is implementation dependent. Binary
// per-sample colors have exact UNORM8 inputs; the standard resolve's result
// has integer error bounded by the header's 0.6 ULP tolerance (3.2.3.6/19.1.2).
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

static const char hlsl[] = R"hlsl(
cbuffer Parameters : register(b0) { uint total; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 uniform_ps() : SV_Target { return float4(0, 1, 0, 1); }
float4 samples_ps(uint index : SV_SampleIndex) : SV_Target {
  return float4(index & 1, index == 0, index + 1 == total, 1);
}
)hlsl";

int
main() {
  auto vertex_code = compile(hlsl, "vs", "vs_4_0"), uniform_code = compile(hlsl, "uniform_ps", "ps_4_0"),
       samples_code = compile(hlsl, "samples_ps", "ps_4_1");
  if (!expect(vertex_code && uniform_code && samples_code, "resolve HLSL did not compile"))
    return verdict();
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")
  );
  if (!expect(create != nullptr, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  constexpr UINT width = D3D10_STANDARD_VECTOR_SIZE, height = 2;
  constexpr UINT destination_mips = 2, destination_arrays = 3, components = D3D10_STANDARD_VECTOR_SIZE;
  const float other_color[] = {1, 0, 1, 0};
  const auto format = DXGI_FORMAT_R8G8B8A8_UNORM;
  for (auto level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, level, D3D10_1_SDK_VERSION, &device));
    ComPtr<ID3D10VertexShader> vs;
    ComPtr<ID3D10PixelShader> uniform_ps, samples_ps;
    CHECK(device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), &vs));
    CHECK(device->CreatePixelShader(uniform_code->GetBufferPointer(), uniform_code->GetBufferSize(), &uniform_ps));
    if (level == D3D10_FEATURE_LEVEL_10_1)
      CHECK(device->CreatePixelShader(samples_code->GetBufferPointer(), samples_code->GetBufferSize(), &samples_ps));
    D3D10_BUFFER_DESC cb_desc{components * sizeof(UINT), D3D10_USAGE_DEFAULT, D3D10_BIND_CONSTANT_BUFFER};
    ComPtr<ID3D10Buffer> constants;
    CHECK(device->CreateBuffer(&cb_desc, nullptr, &constants));
    D3D10_RASTERIZER_DESC raster_desc{D3D10_FILL_SOLID, D3D10_CULL_NONE};
    raster_desc.DepthClipEnable = raster_desc.MultisampleEnable = TRUE;
    ComPtr<ID3D10RasterizerState> raster;
    CHECK(device->CreateRasterizerState(&raster_desc, &raster));
    device->RSSetState(raster.Get());
    for (UINT samples = 2; samples <= D3D10_MAX_MULTISAMPLE_SAMPLE_COUNT; samples++) {
      UINT qualities = 0;
      CHECK(device->CheckMultisampleQualityLevels(format, samples, &qualities));
      if (level == D3D10_FEATURE_LEVEL_10_1 && samples == 4)
        expect(qualities > 0, "10.1 requires standard 4x MSAA on RGBA8 (19.2.5)");
      if (!qualities)
        continue;
      std::vector<UINT> patterns;
      for (UINT quality = 0; quality < qualities; quality++)
        patterns.push_back(quality);
      const bool fixed =
          level == D3D10_FEATURE_LEVEL_10_1 && (samples == 2 || samples == 4 || samples == 8 || samples == 16);
      if (fixed) {
        patterns.push_back(D3D11_STANDARD_MULTISAMPLE_PATTERN);
        patterns.push_back(D3D11_CENTER_MULTISAMPLE_PATTERN);
      }
      for (UINT quality : patterns) {
        for (bool source_typeless : {false, true}) {
          for (bool destination_typeless : {false, true}) {
            step(
                "level %#x resolve samples %u quality %#x typeless %u/%u", level, samples, quality, source_typeless,
                destination_typeless
            );
            D3D10_TEXTURE2D_DESC source_desc{
                width,
                height,
                1,
                2,
                source_typeless ? DXGI_FORMAT_R8G8B8A8_TYPELESS : format,
                {samples, quality},
                D3D10_USAGE_DEFAULT,
                D3D10_BIND_RENDER_TARGET
            };
            D3D10_TEXTURE2D_DESC destination_desc{
                width * 2,
                height * 2,
                destination_mips,
                destination_arrays,
                destination_typeless ? DXGI_FORMAT_R8G8B8A8_TYPELESS : format,
                {1, 0},
                D3D10_USAGE_DEFAULT,
                D3D10_BIND_RENDER_TARGET
            };
            ComPtr<ID3D10Texture2D> source, destination, readback;
            CHECK(device->CreateTexture2D(&source_desc, nullptr, &source));
            CHECK(device->CreateTexture2D(&destination_desc, nullptr, &destination));
            auto read_desc = destination_desc;
            read_desc.Usage = D3D10_USAGE_STAGING;
            read_desc.BindFlags = 0;
            read_desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
            CHECK(device->CreateTexture2D(&read_desc, nullptr, &readback));
            ComPtr<ID3D10RenderTargetView> views[2];
            for (UINT array = 0; array < std::size(views); array++) {
              D3D10_RENDER_TARGET_VIEW_DESC view_desc{format, D3D10_RTV_DIMENSION_TEXTURE2DMSARRAY};
              view_desc.Texture2DMSArray = {array, 1};
              CHECK(device->CreateRenderTargetView(source.Get(), &view_desc, &views[array]));
            }
            device->ClearRenderTargetView(views[0].Get(), other_color);
            device->VSSetShader(vs.Get());
            bool per_sample = quality == UINT(D3D11_STANDARD_MULTISAMPLE_PATTERN);
            device->PSSetShader(per_sample ? samples_ps.Get() : uniform_ps.Get());
            UINT data[components] = {samples};
            device->UpdateSubresource(constants.Get(), 0, nullptr, data, 0, 0);
            device->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
            D3D10_VIEWPORT viewport{0, 0, width, height, 0, 1};
            device->RSSetViewports(1, &viewport);
            device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            device->OMSetRenderTargets(1, views[1].GetAddressOf(), nullptr);
            device->Draw(3, 0);
            device->OMSetRenderTargets(0, nullptr, nullptr);
            std::vector<std::vector<UINT8>> poison(destination_mips * destination_arrays);
            for (UINT sub = 0; sub < poison.size(); sub++) {
              UINT w = destination_desc.Width >> (sub % destination_mips);
              UINT h = destination_desc.Height >> (sub % destination_mips);
              poison[sub].assign(w * h * components, UINT8(sub + 1));
            }
            for (UINT source_slice = 0; source_slice < std::size(views); source_slice++) {
              for (UINT destination_slice = 0; destination_slice < destination_arrays; destination_slice++) {
                for (UINT sub = 0; sub < poison.size(); sub++) {
                  UINT w = destination_desc.Width >> (sub % destination_mips);
                  device->UpdateSubresource(destination.Get(), sub, nullptr, poison[sub].data(), w * components, 0);
                }
                UINT selected = D3D10CalcSubresource(1, destination_slice, destination_mips);
                device->ResolveSubresource(destination.Get(), selected, source.Get(), source_slice, format);
                device->CopyResource(readback.Get(), destination.Get());
                for (UINT sub = 0; sub < poison.size(); sub++) {
                  D3D10_MAPPED_TEXTURE2D mapped{};
                  CHECK(readback->Map(sub, D3D10_MAP_READ, 0, &mapped));
                  UINT w = destination_desc.Width >> (sub % destination_mips);
                  UINT h = destination_desc.Height >> (sub % destination_mips);
                  bool same = true;
                  for (UINT y = 0; y < h; y++) {
                    auto row = static_cast<const UINT8 *>(mapped.pData) + y * mapped.RowPitch;
                    for (UINT x = 0; x < w; x++) {
                      for (UINT c = 0; c < components; c++) {
                        double expected = source_slice ? (c == 1 || c == 3 ? 1 : 0) : other_color[c];
                        if (source_slice && per_sample) {
                          UINT sum = 0;
                          for (UINT i = 0; i < samples; i++)
                            sum += c == 0 ? i & 1 : c == 1 ? i == 0 : c == 2 ? i + 1 == samples : 1;
                          expected = double(sum) / samples;
                        }
                        expected *= std::numeric_limits<UINT8>::max();
                        UINT8 actual = row[x * components + c];
                        same &= sub == selected
                                    ? std::abs(actual - expected) <= D3D10_FLOAT32_TO_INTEGER_TOLERANCE_IN_ULP
                                    : actual == poison[sub][(y * w + x) * components + c];
                      }
                    }
                  }
                  expect(same, "resolve %u -> %u changed wrong pixels in subresource %u", source_slice, selected, sub);
                  readback->Unmap(sub);
                }
              }
            }
          }
        }
      }
    }
    device->ClearState();
  }
  return verdict();
}
