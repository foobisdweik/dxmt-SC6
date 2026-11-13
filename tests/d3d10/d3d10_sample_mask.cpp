// contract: D3D11.3 17.17: "SampleMask is always applied, regardless of whether
// Multisample rendertargets are bound or not." 17.18 fixes A2C endpoints and
// disables A2C with shader output coverage (D3D10.1). load every sample through
// a pixel shader into integer bytes, then count the stored ones independently
// of the allowed mask-bit mapping. every reported count and numeric quality
// runs through native D3D10 and D3D10.1 devices.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// SM4.0 needs a declared multisample count; all readers compile before device
// creation.
// https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-type
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include "../om_samples.hpp"

int main() {
  struct Code {
    ComPtr<ID3DBlob> vs, ps[2], reads[D3D10_MAX_MULTISAMPLE_SAMPLE_COUNT + 1];
  } code[2];
  const char *profiles[] = {"4_0", "4_1"};
  for (UINT profile = 0; profile < std::size(code); profile++) {
    auto defines = om::sample_defines();
    defines.push_back("SINGLE=1");
    code[profile].vs =
        compile(om::sample_hlsl, "vs",
                (std::string("vs_") + profiles[profile]).c_str(), defines);
    const char *entries[] = {"ps", "ps_coverage"};
    for (UINT i = 0; i <= profile; i++)
      code[profile].ps[i] =
          compile(om::sample_hlsl, entries[i],
                  (std::string("ps_") + profiles[profile]).c_str(), defines);
    for (UINT samples = 1; samples <= D3D10_MAX_MULTISAMPLE_SAMPLE_COUNT;
         samples++) {
      auto d = om::sample_defines();
      d.push_back("SINGLE=" + std::to_string(samples == 1));
      d.push_back("STATIC_SAMPLES=1");
      d.push_back("STATIC_SAMPLE_INDEX=" + std::to_string(profile == 0));
      d.push_back("PIXEL_READ=1");
      d.push_back("SAMPLES=" + std::to_string(samples));
      code[profile].reads[samples] =
          compile(om::sample_hlsl, "ps_read",
                  (std::string("ps_") + profiles[profile]).c_str(), d);
      if (!expect(bool(code[profile].reads[samples]),
                  "sample reader %u did not compile", samples))
        return verdict();
    }
    if (!expect(code[profile].vs && code[profile].ps[0] &&
                    (!profile || code[profile].ps[1]),
                "sample-mask HLSL did not compile"))
      return verdict();
  }
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>((
      void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create, "D3D10CreateDevice1 unavailable"))
    return verdict();
  const D3D10_FEATURE_LEVEL1 levels[] = {D3D10_FEATURE_LEVEL_10_0,
                                         D3D10_FEATURE_LEVEL_10_1};
  for (UINT profile = 0; profile < std::size(code); profile++) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
                 levels[profile], D3D10_1_SDK_VERSION, &device));
    ComPtr<ID3D10VertexShader> vs;
    ComPtr<ID3D10PixelShader> ps[2];
    CHECK(device->CreateVertexShader(code[profile].vs->GetBufferPointer(),
                                     code[profile].vs->GetBufferSize(), &vs));
    for (UINT i = 0; i <= profile; i++)
      CHECK(device->CreatePixelShader(code[profile].ps[i]->GetBufferPointer(),
                                      code[profile].ps[i]->GetBufferSize(),
                                      &ps[i]));
    ComPtr<ID3D10BlendState> states[2];
    for (UINT alpha = 0; alpha < std::size(states); alpha++) {
      D3D10_BLEND_DESC desc{};
      desc.AlphaToCoverageEnable = alpha;
      desc.SrcBlend = desc.SrcBlendAlpha = D3D10_BLEND_ONE;
      desc.DestBlend = desc.DestBlendAlpha = D3D10_BLEND_ZERO;
      desc.BlendOp = desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
      for (auto &mask : desc.RenderTargetWriteMask)
        mask = D3D10_COLOR_WRITE_ENABLE_ALL;
      CHECK(device->CreateBlendState(&desc, &states[alpha]));
    }
    D3D10_RASTERIZER_DESC rd{D3D10_FILL_SOLID, D3D10_CULL_NONE};
    rd.DepthClipEnable = rd.ScissorEnable = rd.MultisampleEnable = TRUE;
    ComPtr<ID3D10RasterizerState> raster;
    CHECK(device->CreateRasterizerState(&rd, &raster));
    for (UINT samples = 1; samples <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT;
         samples++) {
      UINT qualities = UINT_MAX;
      HRESULT hr = device->CheckMultisampleQualityLevels(DXGI_FORMAT_R32_FLOAT,
                                                         samples, &qualities);
      if (!expect(
              (samples != 1 && !(profile && samples == 4)) ||
                  (hr == S_OK && qualities),
              "required R32_FLOAT sample count %u returned %08lx, %u qualities",
              samples, hr, qualities))
        return verdict();
      if (FAILED(hr) || !qualities) {
        printf("unsupported R32_FLOAT sample count %u, level %u: %08lx, %u "
               "qualities\n",
               samples, levels[profile], hr, qualities);
        continue;
      }
      ComPtr<ID3D10PixelShader> reader;
      CHECK(device->CreatePixelShader(
          code[profile].reads[samples]->GetBufferPointer(),
          code[profile].reads[samples]->GetBufferSize(), &reader));
      const auto cases = om::sample_cases(samples);
      const UINT width = om::sample_width, height = UINT(cases.size());
      for (UINT quality = 0; quality < qualities; quality++) {
        step("sample count %u, quality %u, level %u: initialize", samples,
             quality, levels[profile]);
        device->ClearState();
        D3D10_TEXTURE2D_DESC td{width,
                                height,
                                1,
                                1,
                                DXGI_FORMAT_R32_FLOAT,
                                {samples, quality},
                                D3D10_USAGE_DEFAULT,
                                D3D10_BIND_RENDER_TARGET |
                                    D3D10_BIND_SHADER_RESOURCE};
        ComPtr<ID3D10Texture2D> target, expanded, staging;
        CHECK(device->CreateTexture2D(&td, nullptr, &target));
        ComPtr<ID3D10RenderTargetView> rtv, read_rtv;
        ComPtr<ID3D10ShaderResourceView> srv;
        CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        CHECK(device->CreateShaderResourceView(target.Get(), nullptr, &srv));
        td.Width *= samples;
        td.Format = DXGI_FORMAT_R32_UINT;
        td.SampleDesc = {1, 0};
        td.BindFlags = D3D10_BIND_RENDER_TARGET;
        CHECK(device->CreateTexture2D(&td, nullptr, &expanded));
        CHECK(
            device->CreateRenderTargetView(expanded.Get(), nullptr, &read_rtv));
        td.Usage = D3D10_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        CHECK(device->CreateTexture2D(&td, nullptr, &staging));
        const float black[4]{};
        device->ClearRenderTargetView(rtv.Get(), black);
        device->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        device->VSSetShader(vs.Get());
        device->RSSetState(raster.Get());
        device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (UINT y = 0; y < cases.size(); y++) {
          const auto &k = cases[y];
          if (!profile && k.coverage)
            continue;
          step("sample count %u, quality %u, level %u, case %u", samples,
               quality, levels[profile], y);
          D3D10_VIEWPORT viewport{0, INT(y), width, 1, 0, 1};
          D3D10_RECT scissor{1, LONG(y), LONG(width - 1), LONG(y + 1)};
          device->RSSetViewports(1, &viewport);
          device->RSSetScissorRects(1, &scissor);
          device->PSSetShader(ps[k.coverage].Get());
          device->OMSetBlendState(states[k.alpha].Get(), nullptr, k.mask);
          device->Draw(3, 0);
        }
        device->OMSetRenderTargets(1, read_rtv.GetAddressOf(), nullptr);
        device->OMSetBlendState(nullptr, nullptr, ~0u);
        device->PSSetShaderResources(0, 1, srv.GetAddressOf());
        device->PSSetShader(reader.Get());
        D3D10_VIEWPORT viewport{0, 0, width * samples, height, 0, 1};
        D3D10_RECT scissor{0, 0, LONG(width * samples), LONG(height)};
        device->RSSetViewports(1, &viewport);
        device->RSSetScissorRects(1, &scissor);
        device->Draw(3, 0);
        device->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D10ShaderResourceView *none = nullptr;
        device->PSSetShaderResources(0, 1, &none);
        device->CopyResource(staging.Get(), expanded.Get());
        D3D10_MAPPED_TEXTURE2D got{};
        CHECK(staging->Map(0, D3D10_MAP_READ, 0, &got));
        for (UINT y = 0; y < cases.size(); y++) {
          step("sample count %u, quality %u, level %u, case %u: readback",
               samples, quality, levels[profile], y);
          for (UINT x = 0; x < width; x++) {
            UINT count = 0;
            for (UINT sid = 0; sid < samples; sid++) {
              UINT value;
              memcpy(&value,
                     static_cast<const char *>(got.pData) + y * got.RowPitch +
                         (x * samples + sid) * sizeof(UINT),
                     sizeof(value));
              expect(value == 0 || value == std::bit_cast<UINT>(1.0f),
                     "pixel %u sample %u has %#x", x, sid, value);
              count += value == std::bit_cast<UINT>(1.0f);
            }
            UINT want = !profile && cases[y].coverage
                            ? 0
                            : om::sample_result(cases[y], samples, x);
            expect(count == want,
                   "pixel %u has %u covered samples, want %u, mask %#x, A2C "
                   "%u, coverage %u",
                   x, count, want, cases[y].mask, cases[y].alpha,
                   cases[y].coverage);
          }
        }
        staging->Unmap(0);
      }
    }
  }
  return verdict();
}
