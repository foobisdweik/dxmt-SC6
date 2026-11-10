// contract: D3D11.3 16.2: "the Pixel Shader is invoked once for each pixel covered by the primitive (pixel-frequency),
// or once per sample (sample-frequency)". 16.13: "After the discard is issued, further operations on UAVs do not
// change the UAV memory". 3.5.6.4: UAV-only rendering rasterizes at ForcedSampleCount. source:
// D3D11_3_FunctionalSpec.htm. each run counts its pixel before a discard of every third pixel and after it, and
// appends past it; a pixel is covered when any sample of the standard pattern (../raster_samples.hpp) is, by
// ../raster.hpp's coverage. the triangle lies inside the viewport, so no clipping splits it.
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include "../raster_samples.hpp"
#include <numeric>

static const char writes[] = R"hlsl(
RWBuffer<uint> counts : register(u1);
RWStructuredBuffer<uint> list : register(u2);
float4 vs(float4 p : POSITION) : SV_Position { return p; }
uint run(float4 p) {
  uint pixel = uint(p.y) * WIDTH + uint(p.x);
  InterlockedAdd(counts[pixel], 1);
  if (pixel % 3 == 0)
    discard;
  InterlockedAdd(counts[WIDTH * HEIGHT + pixel], 1);
  list[list.IncrementCounter()] = pixel;
  return pixel;
}
float4 ps_pixel(float4 p : SV_Position) : SV_Target { return run(p); }
float4 ps_sample(float4 p : SV_Position, uint sample : SV_SampleIndex) : SV_Target { return run(p) + sample; }
)hlsl";

int
main() {
  using namespace raster;
  const std::vector<std::string> defines{"WIDTH=" + std::to_string(width), "HEIGHT=" + std::to_string(height)};
  auto vs = compile(writes, "vs", "vs", defines), pixel = compile(writes, "ps_pixel", "ps", defines),
       sample = compile(writes, "ps_sample", "ps", defines);
  if (!expect(vs && pixel && sample, "shaders did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  ComPtr<ID3D11Device1> device1;
  CHECK(device.As(&device1));
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11PixelShader> shaders[2];
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_shader));
  CHECK(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &shaders[0]));
  CHECK(device->CreatePixelShader(sample->GetBufferPointer(), sample->GetBufferSize(), nullptr, &shaders[1]));
  D3D11_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
  ComPtr<ID3D11InputLayout> layout;
  CHECK(device->CreateInputLayout(&element, 1, vs->GetBufferPointer(), vs->GetBufferSize(), &layout));
  auto c = base("inner triangle with partly covered edge pixels");
  c.vertices = {vertex(1.3f, 1.1f), vertex(width - 1.3f, 2.2f), vertex(2.1f, height - 1.4f)};
  std::vector<Color> positions;
  for (auto &v : c.vertices)
    positions.push_back(v.position);
  D3D11_BUFFER_DESC bd{UINT(positions.size() * sizeof(Color)), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
  D3D11_SUBRESOURCE_DATA vertices{positions.data()};
  ComPtr<ID3D11Buffer> vb;
  CHECK(device->CreateBuffer(&bd, &vertices, &vb));
  const UINT pixels = width * height;
  auto counts = buffer(device.Get(), 2 * pixels * 4, D3D11_BIND_UNORDERED_ACCESS);
  bd = {pixels * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
  ComPtr<ID3D11Buffer> list, appended;
  CHECK(device->CreateBuffer(&bd, nullptr, &list));
  bd = {4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS};
  CHECK(device->CreateBuffer(&bd, nullptr, &appended));
  ComPtr<ID3D11UnorderedAccessView> uavs[2];
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud{DXGI_FORMAT_R32_UINT, D3D11_UAV_DIMENSION_BUFFER};
  ud.Buffer.NumElements = 2 * pixels;
  CHECK(device->CreateUnorderedAccessView(counts.Get(), &ud, &uavs[0]));
  ud = {DXGI_FORMAT_UNKNOWN, D3D11_UAV_DIMENSION_BUFFER};
  ud.Buffer.NumElements = pixels;
  ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_COUNTER;
  CHECK(device->CreateUnorderedAccessView(list.Get(), &ud, &uavs[1]));
  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)));
  struct Mode {
    UINT samples;
    bool uav_only, per_sample;
  };
  std::vector<Mode> modes;
  for (UINT samples : {1u, 4u, 8u})
    for (bool per_sample : {false, true})
      modes.push_back({samples, false, per_sample});
  // sample-frequency runs are invalid without render targets (3.5.6.4)
  for (UINT samples : {0u, 4u, 8u})
    if (!samples || options.UAVOnlyRenderingForcedSampleCount)
      modes.push_back({samples, true, false});
  for (auto mode : modes) {
    step("%u samples, %s, %s-frequency", mode.samples, mode.uav_only ? "UAVs alone" : "render target",
         mode.per_sample ? "sample" : "pixel");
    UINT quality = 0;
    CHECK(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, std::max(mode.samples, 1u), &quality));
    // 4x is required of every render target format, 8x is optional (format support for feature level 11_0
    // hardware): Metal rasterizes 8 samples on Apple10 alone (Metal feature set tables)
    if (!quality && mode.samples > 4)
      continue;
    if (!expect(quality, "%ux RGBA8 is not supported", mode.samples))
      continue;
    D3D11_TEXTURE2D_DESC td{width,
                            height,
                            1,
                            1,
                            DXGI_FORMAT_R8G8B8A8_UNORM,
                            {mode.samples, mode.samples > 1 ? UINT(D3D11_STANDARD_MULTISAMPLE_PATTERN) : 0u},
                            D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (!mode.uav_only) {
      CHECK(device->CreateTexture2D(&td, nullptr, &target));
      CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    }
    D3D11_RASTERIZER_DESC1 rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    rd.DepthClipEnable = TRUE;
    rd.ForcedSampleCount = mode.uav_only ? mode.samples : 0;
    ComPtr<ID3D11RasterizerState1> state;
    CHECK(device1->CreateRasterizerState1(&rd, &state));
    const UINT zero[4] = {};
    context->ClearUnorderedAccessViewUint(uavs[0].Get(), zero);
    const UINT stride = sizeof(Color), offset = 0, initial[2] = {~0u, 0};
    context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
    context->IASetInputLayout(layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_shader.Get(), nullptr, 0);
    context->PSSetShader(shaders[mode.per_sample].Get(), nullptr, 0);
    context->RSSetState(state.Get());
    D3D11_VIEWPORT vp{0, 0, width, height, 0, 1};
    context->RSSetViewports(1, &vp);
    ID3D11UnorderedAccessView *views[2] = {uavs[0].Get(), uavs[1].Get()};
    context->OMSetRenderTargetsAndUnorderedAccessViews(!mode.uav_only, rtv.GetAddressOf(), nullptr, 1, 2, views,
                                                       initial);
    context->Draw(UINT(positions.size()), 0);
    context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);
    context->CopyStructureCount(appended.Get(), 0, uavs[1].Get());
    auto got = read(device.Get(), context.Get(), counts.Get());
    auto total = read(device.Get(), context.Get(), appended.Get());

    std::vector<std::vector<Color>> coverage;
    for (auto p : raster_samples::pattern(std::max(mode.samples, 1u)))
      coverage.push_back(expected(c, double(p[0]) / sample_grid, double(p[1]) / sample_grid));
    unsigned wrong = 0, kept = 0, partial = 0;
    for (UINT p = 0; p < pixels; p++) {
      unsigned covered = 0;
      for (auto &samples : coverage)
        covered += samples[p][0] != 0;
      partial += covered && covered < coverage.size();
      unsigned runs = mode.per_sample ? covered : covered != 0, after = p % 3 ? runs : 0;
      kept += after;
      if ((got[p] != runs || got[pixels + p] != after) && wrong++ < 4)
        expect(false, "pixel %u,%u: %u runs and %u after the discard, want %u and %u", p % width, p / width, got[p],
               got[pixels + p], runs, after);
    }
    expect(!wrong, "%u pixels ran or wrote a wrong number of times", wrong);
    expect(total[0] == kept, "%u appended past the discard, want %u", total[0], kept);
    expect(mode.samples < 2 || partial, "no pixel is partly covered at %u samples", mode.samples);
  }
  return verdict();
}
