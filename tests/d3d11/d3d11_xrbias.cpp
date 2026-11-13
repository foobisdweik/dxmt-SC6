// contract: XR_BIAS stores R:G:B:A in bits 9:0, 19:10, 29:20, 31:30 (Microsoft
// Learn, XR Layout), and is viewed through R10G10B10A2_UNORM or UINT, not as an
// XR render target or shader resource. "Scan-out hardware must be able to apply
// the bias and scale" (DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM). XR_BIAS to
// Float Conversion Rules gives "return (float)( (XRComponent & 0x3ff) - 0x180 )
// / 510.f;"; alpha is ordinary 2-bit UNORM (XR Format Alpha Content).
// https://learn.microsoft.com/en-us/windows-hardware/drivers/display/xr-bias-to-float-conversion-rules
// https://learn.microsoft.com/en-us/windows-hardware/drivers/display/casting-ability-of-xr-formats
// the format tables require Texture2D, CPU lock, display and casting at 11_0
// and above and permit it at 10.x. DXGI_SWAP_CHAIN_DESC1 excludes XR and sRGB from flip.
// D3D11.3 19.1.3.4: "The _SRGB format modifier is ignored for display scan-out"; an sRGB
// view still encodes a linear clear (3.2.3.8), on a bitblt sRGB buffer or a flip UNORM buffer.
// https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/format-support-for-direct3d-11-0-feature-level-hardware
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_swap_chain_desc1
// every RGB code and alpha code is rendered and copied; expectations are the
// packed codes, not sampled output. window pixels exercise the shared
// presentation shader through GDI, clamped to SDR. native layer output and wide
// color need an external capture; Apple BGR10_XR documents the same decode and
// SDR clamp, but has BGR order and padding instead of alpha:
// https://developer.apple.com/documentation/metal/mtlpixelformat/bgr10_xr
// GDI color stretching uses COLORONCOLOR: "This mode deletes all eliminated lines of pixels without trying to
// preserve their information" (SetStretchBltMode). DXGI_SCALING_STRETCH says "scale to fit the presentation target
// size", not which texel survives. GDI stretches independently decoded BGRA pixels as the oracle; dimensions
// straddle the client size in both axes, and RGB varies across rows as well as columns.
// https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-setstretchbltmode
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ne-dxgi1_2-dxgi_scaling
#include "d3d11_test.hpp"
#include <algorithm>
#include <cmath>
#include <dxgi1_4.h>
#include <limits>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint4 codes(float4 pos) {
  uint r = (uint(pos.x) + uint(pos.y)) & COLOR_MAX;
  return uint4(r, (r + BIAS) & COLOR_MAX, COLOR_MAX - r, uint(pos.y) & ALPHA_MAX);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
  uint4 q = codes(pos);
  float3 colour = (float3(q.rgb) - BIAS) / SCALE;
  return float4((colour * SCALE + BIAS) / COLOR_MAX, float(q.a) / ALPHA_MAX);
}
uint4 ps_uint(float4 pos : SV_Position) : SV_Target {
  return codes(pos) ^ uint4(COLOR_MAX, COLOR_MAX, COLOR_MAX, ALPHA_MAX);
}
Texture2D<float4> source : register(t0);
Texture2D<uint4> integer_source : register(t0);
SamplerState nearest : register(s0);
float4 ps_sample(float4 pos : SV_Position) : SV_Target {
  uint width, height; source.GetDimensions(width, height);
  return source.SampleLevel(nearest, pos.xy / float2(width, height), 0);
}
float4 ps_load(float4 pos : SV_Position) : SV_Target { return integer_source.Load(int3(uint2(pos.xy), 0)); }
)hlsl";

int
main() {
  constexpr UINT colour_bits = 10, alpha_bits = 2;
  constexpr UINT colour_max = (1u << colour_bits) - 1, alpha_max = (1u << alpha_bits) - 1;
  constexpr UINT bias = 0x180;
  constexpr float scale = 510;
  const std::string values[] = {std::to_string(colour_max), std::to_string(alpha_max), std::to_string(bias),
                                std::to_string(scale)};
  const D3D_SHADER_MACRO defines[] = {{"COLOR_MAX", values[0].c_str()},
                                      {"ALPHA_MAX", values[1].c_str()},
                                      {"BIAS", values[2].c_str()},
                                      {"SCALE", values[3].c_str()},
                                      {}};
  ComPtr<ID3DBlob> shaders[5];
  const char *entries[] = {"vs", "ps", "ps_uint", "ps_sample", "ps_load"};
  for (UINT i = 0; i < ARRAYSIZE(shaders); i++) {
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(hlsl, sizeof(hlsl) - 1, nullptr, defines, nullptr, entries[i], i ? "ps_4_0" : "vs_4_0", 0,
                            0, &shaders[i], &errors);
    if (FAILED(hr) && errors)
      printf("%s: %.*s\n", entries[i], int(errors->GetBufferSize()), (const char *)errors->GetBufferPointer());
    if (!expect(SUCCEEDED(hr) && shaders[i], "%s did not compile: %#lx", entries[i], hr))
      return verdict();
  }
  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  ComPtr<ID3D11Device> probe;
  D3D_FEATURE_LEVEL maximum;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                          &probe, &maximum, nullptr));
  probe.Reset();
  constexpr DXGI_FORMAT xr = DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM;
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d11_xrbias"};
  if (!expect(RegisterClassA(&wc) != 0, "RegisterClass: %lu", GetLastError()))
    return verdict();
  HWND window = CreateWindowA(wc.lpszClassName, wc.lpszClassName, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 128, 128,
                              nullptr, nullptr, wc.hInstance, nullptr);
  if (!expect(window != nullptr, "CreateWindow: %lu", GetLastError()))
    return verdict();
  for (auto level : levels) {
    if (level > maximum) {
      printf("not exercised: feature level %#x exceeds device maximum %#x\n", level, maximum);
      continue;
    }
    step("feature level %#x: XR support", level);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                            nullptr, &context));
    UINT support = 0;
    HRESULT hr = device->CheckFormatSupport(xr, &support);
    const UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_CPU_LOCKABLE |
                          D3D11_FORMAT_SUPPORT_DISPLAY | D3D11_FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT;
    const UINT allowed = required | D3D11_FORMAT_SUPPORT_BACK_BUFFER_CAST |
                         D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT | D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT;
    expect(hr == S_OK && (support & required) == required && !(support & ~allowed),
           "XR support %#x, hr %#lx", support, hr);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    CHECK(device.As(&dxgi));
    CHECK(dxgi->GetAdapter(&adapter));
    CHECK(adapter->GetParent(IID_PPV_ARGS(&factory)));
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps, ps_uint, ps_sample, ps_load;
    CHECK(device->CreateVertexShader(shaders[0]->GetBufferPointer(), shaders[0]->GetBufferSize(), nullptr, &vs));
    CHECK(device->CreatePixelShader(shaders[1]->GetBufferPointer(), shaders[1]->GetBufferSize(), nullptr, &ps));
    CHECK(device->CreatePixelShader(shaders[2]->GetBufferPointer(), shaders[2]->GetBufferSize(), nullptr, &ps_uint));
    CHECK(
        device->CreatePixelShader(shaders[3]->GetBufferPointer(), shaders[3]->GetBufferSize(), nullptr, &ps_sample));
    CHECK(device->CreatePixelShader(shaders[4]->GetBufferPointer(), shaders[4]->GetBufferSize(), nullptr, &ps_load));
    for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
      for (auto effect : {DXGI_SWAP_EFFECT_DISCARD, DXGI_SWAP_EFFECT_SEQUENTIAL, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
                          DXGI_SWAP_EFFECT_FLIP_DISCARD}) {
        step("sRGB format %u effect %u", format, effect);
        const bool flip = effect >= DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        DXGI_SWAP_CHAIN_DESC1 desc{8, 8, format, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2,
                                   DXGI_SCALING_STRETCH, effect};
        ComPtr<IDXGISwapChain1> chain;
        expect(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &chain) ==
                   (flip ? DXGI_ERROR_INVALID_CALL : S_OK), "sRGB creation has wrong result");
        if (flip) {
          desc.Format = format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                                 : DXGI_FORMAT_B8G8R8A8_UNORM;
          CHECK(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &chain));
          ComPtr<IDXGISwapChain3> chain3;
          CHECK(chain.As(&chain3));
          for (auto refused : {format, xr}) {
            expect(chain->ResizeBuffers(0, 0, 0, refused, 0) == DXGI_ERROR_INVALID_CALL,
                   "flip resize accepted format %u", refused);
            expect(chain3->ResizeBuffers1(0, 0, 0, refused, 0, nullptr, nullptr) == DXGI_ERROR_INVALID_CALL,
                   "flip ResizeBuffers1 accepted format %u", refused);
          }
        }
        ComPtr<ID3D11Texture2D> back;
        CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&back)));
        D3D11_RENDER_TARGET_VIEW_DESC view{format, D3D11_RTV_DIMENSION_TEXTURE2D};
        ComPtr<ID3D11RenderTargetView> rtv;
        CHECK(device->CreateRenderTargetView(back.Get(), &view, &rtv));
        const float colour[] = {0.25f, 0.25f, 0.25f, 1};
        context->ClearRenderTargetView(rtv.Get(), colour);
        D3D11_TEXTURE2D_DESC td;
        back->GetDesc(&td);
        expect(td.Format == desc.Format, "sRGB view changed the buffer format");
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = td.MiscFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        CHECK(device->CreateTexture2D(&td, nullptr, &readback));
        context->CopyResource(readback.Get(), back.Get());
        D3D11_MAPPED_SUBRESOURCE mapped;
        CHECK(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const UINT encoded = UINT(std::round((1.055f * std::pow(colour[0], 1.f / 2.4f) - 0.055f) * UINT8_MAX));
        for (UINT y = 0; y < td.Height; y++)
          for (UINT x = 0; x < td.Width; x++)
            for (UINT c = 0; c < 4; c++)
              expect(((const uint8_t *)mapped.pData)[y * mapped.RowPitch + x * 4 + c] ==
                         (c == 3 ? UINT8_MAX : encoded),
                     "sRGB view clear differs at %u,%u channel %u", x, y, c);
        context->Unmap(readback.Get(), 0);
        CHECK(chain->Present(0, 0));
        bool shown = false;
        for (UINT attempt = 0; attempt < 250 && !shown; attempt++) {
          MSG msg;
          while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
          Sleep(20);
          HDC dc = GetDC(window);
          auto pixel = GetPixel(dc, 0, 0);
          shown = pixel != CLR_INVALID && std::abs(int(GetRValue(pixel)) - int(encoded)) <= 1 &&
                  std::abs(int(GetGValue(pixel)) - int(encoded)) <= 1 &&
                  std::abs(int(GetBValue(pixel)) - int(encoded)) <= 1;
          ReleaseDC(window, dc);
        }
        expect(shown, "sRGB bitblt buffer or flip view did not present its encoded value");
      }
    for (auto effect : {DXGI_SWAP_EFFECT_DISCARD, DXGI_SWAP_EFFECT_SEQUENTIAL, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
                        DXGI_SWAP_EFFECT_FLIP_DISCARD})
      for (bool legacy : {true, false}) {
        step("level %#x effect %u %s", level, effect, legacy ? "CreateSwapChain" : "CreateSwapChainForHwnd");
        const bool flip = effect >= DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        DXGI_SWAP_CHAIN_DESC1 desc{colour_max + 2,
                                   alpha_max + 1,
                                   xr,
                                   FALSE,
                                   {1, 0},
                                   DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT,
                                   2,
                                   DXGI_SCALING_STRETCH,
                                   effect};
        ComPtr<IDXGISwapChain> chain;
        if (legacy) {
          DXGI_SWAP_CHAIN_DESC old{{desc.Width, desc.Height, {}, xr},
                                   desc.SampleDesc,
                                   desc.BufferUsage,
                                   desc.BufferCount,
                                   window,
                                   TRUE,
                                   effect};
          hr = factory->CreateSwapChain(device.Get(), &old, &chain);
        } else {
          ComPtr<IDXGISwapChain1> made;
          hr = factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &made);
          if (made)
            CHECK(made.As(&chain));
        }
        const HRESULT wanted = flip ? DXGI_ERROR_INVALID_CALL : S_OK;
        expect(hr == wanted && bool(chain) == !flip, "creation %#lx, expected %#lx", hr, wanted);
        if (!chain || flip)
          continue;
        ComPtr<IDXGISwapChain3> chain3;
        CHECK(chain.As(&chain3));
        RECT client_size{};
        if (!expect(GetClientRect(window, &client_size), "GetClientRect: %lu", GetLastError()))
          return verdict();
        const SIZE dimensions[] = {{LONG(desc.Width), LONG(desc.Height)},
                                   {client_size.right, client_size.bottom},
                                   {7, LONG(desc.Height)},
                                   {client_size.right - 1, LONG(desc.Height)},
                                   {client_size.right + 1, LONG(desc.Height)},
                                   {client_size.right, client_size.bottom - 1},
                                   {client_size.right, client_size.bottom + 1}};
        for (auto buffer_format : {xr, DXGI_FORMAT_R10G10B10A2_UNORM, xr})
          for (auto size : dimensions) {
            const UINT width = size.cx, height = size.cy;
            CHECK(chain3->ResizeBuffers1(0, width, height, buffer_format, 0, nullptr, nullptr));
            CHECK(chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));
            ComPtr<ID3D11Texture2D> back;
            CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&back)));
            D3D11_TEXTURE2D_DESC td{};
            back->GetDesc(&td);
            expect(td.Format == buffer_format && td.Width == width && td.Height == height,
                   "GetBuffer descriptor differs");
            std::vector<uint32_t> words(width * td.Height);
            for (UINT y = 0; y < td.Height; y++)
              for (UINT x = 0; x < width; x++) {
                UINT r = (x + y) & colour_max;
                words[y * width + x] = r | (((r + bias) & colour_max) << colour_bits) |
                                       ((colour_max - r) << (2 * colour_bits)) | ((y & alpha_max) << (3 * colour_bits));
              }
            auto bytes_are = [&](ID3D11Texture2D *source) -> HRESULT {
              auto staging_desc = td;
              staging_desc.Format = xr;
              staging_desc.Usage = D3D11_USAGE_STAGING;
              staging_desc.BindFlags = staging_desc.MiscFlags = 0;
              staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
              ComPtr<ID3D11Texture2D> staging;
              HRESULT result = device->CreateTexture2D(&staging_desc, nullptr, &staging);
              if (FAILED(result))
                return result;
              context->CopyResource(staging.Get(), source);
              D3D11_MAPPED_SUBRESOURCE mapped;
              if (FAILED(result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
                return result;
              for (UINT y = 0; y < td.Height; y++)
                for (UINT x = 0; x < width; x++) {
                  auto got = ((const uint32_t *)((const char *)mapped.pData + y * mapped.RowPitch))[x];
                  expect(got == words[y * width + x], "pixel %u,%u: %#x, expected %#x", x, y, got,
                         words[y * width + x]);
                }
              context->Unmap(staging.Get(), 0);
              return S_OK;
            };
            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{xr, D3D11_RTV_DIMENSION_TEXTURE2D};
            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{xr, D3D11_SRV_DIMENSION_TEXTURE2D};
            srv_desc.Texture2D.MipLevels = 1;
            ComPtr<ID3D11RenderTargetView> rtv;
            ComPtr<ID3D11ShaderResourceView> srv;
            D3D11_VIEWPORT viewport{0, 0, float(width), float(td.Height), 0, 1};
            context->RSSetViewports(1, &viewport);
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (auto format : {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UINT}) {
              step("width %u: render through format %u", width, format);
              if (format == DXGI_FORMAT_R10G10B10A2_UINT)
                for (auto &word : words)
                  word = ~word;
              rtv_desc.Format = format;
              CHECK(device->CreateRenderTargetView(back.Get(), &rtv_desc, rtv.ReleaseAndGetAddressOf()));
              ID3D11RenderTargetView *target = rtv.Get();
              context->OMSetRenderTargets(1, &target, nullptr);
              context->PSSetShader(format == DXGI_FORMAT_R10G10B10A2_UINT ? ps_uint.Get() : ps.Get(), nullptr, 0);
              context->Draw(3, 0);
              context->OMSetRenderTargets(0, nullptr, nullptr);
              CHECK(bytes_are(back.Get()));
            }
            for (auto format :
                 {xr, DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UINT}) {
              step("width %u: copy from and to format %u", width, format);
              for (auto &word : words)
                word = ~word;
              auto copy_desc = td;
              copy_desc.Format = format;
              copy_desc.MiscFlags = 0;
              D3D11_SUBRESOURCE_DATA data{words.data(), UINT(width * sizeof(words[0]))};
              ComPtr<ID3D11Texture2D> copy;
              CHECK(device->CreateTexture2D(&copy_desc, &data, &copy));
              context->CopyResource(back.Get(), copy.Get());
              CHECK(bytes_are(back.Get()));
              std::vector<uint32_t> different = words;
              for (auto &word : different)
                word = ~word;
              context->UpdateSubresource(copy.Get(), 0, nullptr, different.data(), width * sizeof(words[0]), 0);
              context->CopyResource(copy.Get(), back.Get());
              CHECK(bytes_are(copy.Get()));
            }
            for (auto sampled_format : {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UINT}) {
              step("width %u: shader read through format %u", width, sampled_format);
              auto float_desc = td;
              float_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
              float_desc.MiscFlags = 0;
              ComPtr<ID3D11Texture2D> sampled;
              CHECK(device->CreateTexture2D(&float_desc, nullptr, &sampled));
              ComPtr<ID3D11RenderTargetView> float_rtv;
              CHECK(device->CreateRenderTargetView(sampled.Get(), nullptr, &float_rtv));
              srv_desc.Format = sampled_format;
              CHECK(device->CreateShaderResourceView(back.Get(), &srv_desc, srv.ReleaseAndGetAddressOf()));
              D3D11_SAMPLER_DESC sd{D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP,
                                    D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP};
              sd.MaxLOD = D3D11_FLOAT32_MAX;
              ComPtr<ID3D11SamplerState> sampler;
              CHECK(device->CreateSamplerState(&sd, &sampler));
              auto sample_view = srv.Get();
              auto sample_state = sampler.Get();
              auto sample_target = float_rtv.Get();
              context->PSSetShaderResources(0, 1, &sample_view);
              context->PSSetSamplers(0, 1, &sample_state);
              context->PSSetShader(sampled_format == DXGI_FORMAT_R10G10B10A2_UNORM ? ps_sample.Get() : ps_load.Get(),
                                   nullptr, 0);
              context->OMSetRenderTargets(1, &sample_target, nullptr);
              context->Draw(3, 0);
              context->OMSetRenderTargets(0, nullptr, nullptr);
              ID3D11ShaderResourceView *none = nullptr;
              context->PSSetShaderResources(0, 1, &none);
              float_desc.Usage = D3D11_USAGE_STAGING;
              float_desc.BindFlags = 0;
              float_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
              ComPtr<ID3D11Texture2D> sampled_read;
              CHECK(device->CreateTexture2D(&float_desc, nullptr, &sampled_read));
              context->CopyResource(sampled_read.Get(), sampled.Get());
              D3D11_MAPPED_SUBRESOURCE mapped;
              CHECK(context->Map(sampled_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
              for (UINT y = 0; y < td.Height; y++)
                for (UINT x = 0; x < width; x++)
                  for (UINT c = 0; c < 4; c++) {
                    auto got = ((const float *)((const char *)mapped.pData + y * mapped.RowPitch))[x * 4 + c];
                    UINT mask = c == 3 ? alpha_max : colour_max;
                    float wanted_value = float((words[y * width + x] >> (c * colour_bits)) & mask);
                    if (sampled_format == DXGI_FORMAT_R10G10B10A2_UNORM)
                      wanted_value /= mask;
                    expect(std::fabs(got - wanted_value) <= 1.f / (colour_max * colour_max),
                           "typed sample %u,%u channel %u: %g, expected %g", x, y, c, got, wanted_value);
                  }
              context->Unmap(sampled_read.Get(), 0);
            }
            step("level %#x effect %u %s, %u by %u: present format %u", level, effect,
                 legacy ? "CreateSwapChain" : "CreateSwapChainForHwnd", width, height, buffer_format);
            RECT client{};
            if (!expect(GetClientRect(window, &client), "GetClientRect: %lu", GetLastError()))
              return verdict();
            std::vector<RGBQUAD> decoded(words.size()), expected(client.right * client.bottom);
            for (size_t i = 0; i < words.size(); i++) {
              BYTE *channels[] = {&decoded[i].rgbRed, &decoded[i].rgbGreen, &decoded[i].rgbBlue};
              for (UINT c = 0; c < ARRAYSIZE(channels); c++) {
                int q = (words[i] >> (c * colour_bits)) & colour_max;
                float displayed =
                    buffer_format == xr ? std::clamp((q - int(bias)) / scale, 0.f, 1.f) : float(q) / colour_max;
                *channels[c] = BYTE(std::round(displayed * UINT8_MAX));
              }
            }
            BITMAPINFO bitmap{{sizeof(BITMAPINFOHEADER), client.right, -client.bottom, 1,
                               sizeof(RGBQUAD) * std::numeric_limits<BYTE>::digits, BI_RGB}};
            void *pixels = nullptr;
            HDC reference = CreateCompatibleDC(nullptr);
            HBITMAP image =
                reference ? CreateDIBSection(reference, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
            HGDIOBJ previous = image ? SelectObject(reference, image) : nullptr;
            bitmap.bmiHeader.biWidth = width;
            bitmap.bmiHeader.biHeight = -LONG(height);
            bool ready = previous && previous != HGDI_ERROR && SetStretchBltMode(reference, COLORONCOLOR) &&
                         StretchDIBits(reference, 0, 0, client.right, client.bottom, 0, 0, width, height,
                                       decoded.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY) > 0 && GdiFlush();
            DWORD error = GetLastError();
            if (ready)
              memcpy(expected.data(), pixels, expected.size() * sizeof(expected[0]));
            if (previous && previous != HGDI_ERROR)
              SelectObject(reference, previous);
            if (image)
              DeleteObject(image);
            if (reference)
              DeleteDC(reference);
            if (!expect(ready, "GDI reference could not be drawn: %lu", error))
              return verdict();
            CHECK(chain->Present(0, 0));
            bool same = false;
            for (UINT attempt = 0; attempt < 250 && !same; attempt++) {
              MSG msg;
              while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
                DispatchMessageA(&msg);
              Sleep(20);
              HDC dc = GetDC(window);
              same = true;
              for (LONG y : {LONG(0), client.bottom / 2, client.bottom - 1})
                for (LONG x = 0; x < client.right; x++) {
                  auto got = GetPixel(dc, x, y);
                  const auto &wanted_pixel = expected[y * client.right + x];
                  same &= got != CLR_INVALID && std::abs(int(GetRValue(got)) - wanted_pixel.rgbRed) <= 1 &&
                          std::abs(int(GetGValue(got)) - wanted_pixel.rgbGreen) <= 1 &&
                          std::abs(int(GetBValue(got)) - wanted_pixel.rgbBlue) <= 1;
                }
              ReleaseDC(window, dc);
            }
            expect(same, "presented pixels differ from XR decode and SDR clamp");
            context->ClearState();
          }
        context->ClearState();
        context->Flush();
      }
  }
  DestroyWindow(window);
  UnregisterClassA(wc.lpszClassName, wc.hInstance);
  return verdict();
}
