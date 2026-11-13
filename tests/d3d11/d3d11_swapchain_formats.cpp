// contract: a bitblt swap chain takes exactly the formats the device reports as DISPLAY at its feature level.
// DXGI_MODE_DESC: "The valid values depend on the feature level that you are working with." "If
// ID3D11Device::CheckFormatSupport returns D3D11_FORMAT_SUPPORT_DISPLAY in the bit field to which the pFormatSupport
// parameter points, the format is valid for displaying on screen."
// https://learn.microsoft.com/en-us/previous-versions/windows/desktop/legacy/bb173064(v=vs.85)
// creation and a resize refuse any other format with E_INVALIDARG (what Windows returns, by Wine's
// test_swapchain_formats), and a refused resize keeps the format. ResizeBuffers: "Set this value to
// DXGI_FORMAT_UNKNOWN to preserve the existing format of the back buffer."
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers
// no other format may reach the layer. CAMetalLayer.pixelFormat: "You must use one of the following formats"
// https://developer.apple.com/documentation/quartzcore/cametallayer/pixelformat
// every chain that is made is cleared and presented to a real window.
#include "d3d11_test.hpp"
#include <dxgi1_4.h>
#include "../depth_formats.hpp"

int
main() {
  const struct { DXGI_FORMAT format; const char *name; } formats[] = {DXGI_FORMATS};
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                    D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2,
                                    D3D_FEATURE_LEVEL_9_1};
  ComPtr<ID3D11Device> probe;
  D3D_FEATURE_LEVEL maximum;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
                         D3D11_SDK_VERSION, &probe, &maximum, nullptr));
  probe.Reset();
  HWND window = CreateWindowA("static", "swapchain_formats", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, 64, 64, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  if (!expect(window != nullptr, "CreateWindow: %lu", GetLastError()))
    return verdict();
  UpdateWindow(window);
  for (auto level : levels) {
    if (level > maximum)
      continue;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                           &device, nullptr, &context));
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    CHECK(device.As(&dxgi));
    CHECK(dxgi->GetAdapter(&adapter));
    CHECK(adapter->GetParent(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 desc{3, 5, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0},
                               DXGI_USAGE_RENDER_TARGET_OUTPUT, 1, DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_DISCARD};
    auto present = [&](IDXGISwapChain *chain, DXGI_FORMAT wanted) {
      DXGI_SWAP_CHAIN_DESC got{};
      expect(chain->GetDesc(&got) == S_OK && got.BufferDesc.Format == wanted, "swap-chain format differs");
      ComPtr<ID3D11Texture2D> texture;
      if (!expect(chain->GetBuffer(0, IID_PPV_ARGS(&texture)) == S_OK, "GetBuffer failed"))
        return;
      D3D11_TEXTURE2D_DESC buffer_desc{};
      texture->GetDesc(&buffer_desc);
      expect(buffer_desc.Format == wanted, "back-buffer format %#x, expected %#x", buffer_desc.Format, wanted);
      ComPtr<ID3D11RenderTargetView> view;
      D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{wanted == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM
                                               ? DXGI_FORMAT_R10G10B10A2_UNORM
                                               : wanted,
                                           D3D11_RTV_DIMENSION_TEXTURE2D};
      if (!expect(device->CreateRenderTargetView(texture.Get(), &rtv_desc, &view) == S_OK, "CreateRTV failed"))
        return;
      const float colour[] = {1, 0, 0, 1};
      context->ClearRenderTargetView(view.Get(), colour);
      view.Reset();
      texture.Reset();
      HRESULT hr = chain->Present(0, 0);
      expect(hr == S_OK, "Present: %#lx", hr);
    };
    for (bool legacy : {true, false}) {
      auto create = [&](DXGI_FORMAT format, ComPtr<IDXGISwapChain> &chain) {
        desc.Format = format;
        if (legacy) {
          DXGI_SWAP_CHAIN_DESC old{{desc.Width, desc.Height, {}, format}, desc.SampleDesc, desc.BufferUsage,
                                   desc.BufferCount, window, TRUE, desc.SwapEffect};
          return factory->CreateSwapChain(device.Get(), &old, &chain);
        }
        ComPtr<IDXGISwapChain1> made;
        HRESULT hr = factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &made);
        if (made)
          expect(made.As(&chain) == S_OK, "swap-chain QueryInterface failed");
        return hr;
      };
      for (bool resize : {false, true}) {
        ComPtr<IDXGISwapChain> resizing;
        ComPtr<IDXGISwapChain3> resizing3;
        DXGI_FORMAT current = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (resize) {
          step("level %#x %s: resize control", level, legacy ? "CreateSwapChain" : "CreateSwapChainForHwnd");
          CHECK(create(current, resizing));
          CHECK(resizing.As(&resizing3));
        }
        for (auto [format, name] : formats) {
          UINT support = 0;
          HRESULT query = device->CheckFormatSupport(format, &support);
          const bool display = SUCCEEDED(query) && (support & D3D11_FORMAT_SUPPORT_DISPLAY);
          if (!resize) {
            step("level %#x %s %s: create", level, name, legacy ? "CreateSwapChain" : "CreateSwapChainForHwnd");
            ComPtr<IDXGISwapChain> chain;
            HRESULT hr = create(format, chain);
            const HRESULT wanted = display ? S_OK : E_INVALIDARG;
            expect(hr == wanted, "Create: %#lx, expected %#lx (support %#x, query %#lx)", hr, wanted, support, query);
            expect(bool(chain) == display, "creation output differs from DISPLAY");
            if (chain)
              present(chain.Get(), format);
            continue;
          }
          for (bool extended : {false, true}) {
            step("level %#x %s %s: ResizeBuffers%s", level, name,
                 legacy ? "CreateSwapChain" : "CreateSwapChainForHwnd", extended ? "1" : "");
            HRESULT hr = extended ? resizing3->ResizeBuffers1(0, desc.Width, desc.Height, format, 0, nullptr, nullptr)
                                  : resizing->ResizeBuffers(0, desc.Width, desc.Height, format, 0);
            const bool valid = display || format == DXGI_FORMAT_UNKNOWN;
            expect(hr == (valid ? S_OK : E_INVALIDARG), "Resize: %#lx, expected %#lx", hr,
                   valid ? S_OK : E_INVALIDARG);
            if (valid && format != DXGI_FORMAT_UNKNOWN)
              current = format;
            present(resizing.Get(), current);
          }
        }
      }
      context->ClearState();
      context->Flush();
    }
  }
  DestroyWindow(window);
  return verdict();
}
