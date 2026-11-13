// contract: every draw uses its current viewport and scissor, however often they change within one frame.
// "Viewports map clip-space vertex positions into RenderTarget space." (D3D11.3 15.6).
// "There is always an implicit scissoring by the Viewport x/y extents, orthogonal to other Scissor (15.7) state."
// "Scissor cuts out a rectangle in RenderTarget space where pixels are permitted to appear. Any pixel outside
// these extents is discarded." (15.7). "If disabled, any pixel on the RenderTarget(s) can be drawn to." (15.7).
// source: D3D11_3_FunctionalSpec.htm, 15.6 and 15.7. whole-pixel edges and ONE/ONE ADD make each pixel's exact
// value the number of draws whose viewport and enabled scissor contain its centre.
// the measured Metal totals, 65,536 scissors and a little before 87,381 viewports, are not Direct3D limits on
// state changes. the unrepaired path sends 16 rectangles per change; three crossings plus a target's worth of
// draws exercise repeated pass splits, their preserved sums, and the rectangle restored at each split.
#include "d3d11_test.hpp"
#include <algorithm>
#include <limits>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps() : SV_Target { return 1; }
)hlsl";

// a stuck API call or teardown must not outlive the test, even if DO_NOT_WAIT itself is broken
static DWORD WINAPI
deadline(void *) {
  const DWORD test_ms = 5 * 60 * 1000;
  Sleep(test_ms);
  printf("failed: 1 timeout after %lu ms\n", test_ms);
  ExitProcess(1);
}

int
main() {
  auto watchdog = CreateThread(nullptr, 0, deadline, nullptr, 0, nullptr);
  if (!expect(watchdog != nullptr, "watchdog could not start: %lu", GetLastError()))
    return verdict();
  CloseHandle(watchdog);
  step("compile the triangle and additive pixel shader");
  auto vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps");
  if (!expect(vs_code && ps_code, "HLSL did not compile"))
    return verdict();

  const UINT width = 8, height = 4;
  constexpr UINT rectangles_per_change = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  constexpr UINT measured_metal_scissors = 65536, measured_metal_viewports = 87381, crossings = 3;
  constexpr UINT draws =
      crossings * (std::max(measured_metal_scissors, measured_metal_viewports) / rectangles_per_change + 1) +
      width * height;
  static_assert(draws < (UINT64(1) << std::numeric_limits<float>::digits));
  const UINT report_limit = 8;
  const DWORD wait_ms = 30 * 1000;
  const DXGI_FORMAT format = DXGI_FORMAT_R32_FLOAT;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                        &device, nullptr, &context));
  UINT support = 0;
  CHECK(device->CheckFormatSupport(format, &support));
  if (!expect((support & D3D11_FORMAT_SUPPORT_BLENDABLE) != 0, "R32_FLOAT does not support blending: %#x", support))
    return verdict();
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  D3D11_TEXTURE2D_DESC texture_desc{
      width, height, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET
  };
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &staging));
  ComPtr<ID3D11RasterizerState> raster[2];
  for (BOOL enabled : {FALSE, TRUE}) {
    D3D11_RASTERIZER_DESC desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    desc.DepthClipEnable = TRUE;
    desc.ScissorEnable = enabled;
    CHECK(device->CreateRasterizerState(&desc, &raster[enabled]));
  }
  D3D11_BLEND_DESC blend_desc{};
  auto &blend_target = blend_desc.RenderTarget[0];
  blend_target.BlendEnable = TRUE;
  blend_target.SrcBlend = blend_target.DestBlend = D3D11_BLEND_ONE;
  blend_target.SrcBlendAlpha = blend_target.DestBlendAlpha = D3D11_BLEND_ONE;
  blend_target.BlendOp = blend_target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend_target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ComPtr<ID3D11BlendState> blend;
  CHECK(device->CreateBlendState(&blend_desc, &blend));

  enum { Scissors, Viewports, Both, Unscissored, Deferred, Split, Cases };
  const char *names[Cases] = {
      "scissors", "viewports", "both", "viewports without scissor", "deferred scissors", "split deferred scissors"
  };
  const D3D11_VIEWPORT whole_viewport{0, 0, float(width), float(height), 0, 1};
  const D3D11_RECT whole_scissor{0, 0, LONG(width), LONG(height)};
  for (UINT c = 0; c < Cases; c++) {
    step("%s: %u draws in one frame", names[c], draws);
    const bool change_viewport = c == Viewports || c == Both || c == Unscissored;
    const bool change_scissor = !change_viewport || c == Both;
    const bool enabled = c != Unscissored;
    const UINT parts = c == Split ? 2 : 1;
    auto *record = c == Deferred || c == Split ? deferred.Get() : context.Get();
    std::vector<UINT> want(width * height);
    ComPtr<ID3D11CommandList> lists[2];
    const float clear[4] = {};
    context->ClearRenderTargetView(rtv.Get(), clear);
    for (UINT part = 0; part < parts; part++) {
      record->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      record->VSSetShader(vs.Get(), nullptr, 0);
      record->PSSetShader(ps.Get(), nullptr, 0);
      record->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      record->OMSetBlendState(blend.Get(), nullptr, ~0u);
      record->RSSetState(raster[enabled].Get());
      record->RSSetViewports(1, &whole_viewport);
      record->RSSetScissorRects(1, &whole_scissor);
      for (UINT i = draws * part / parts; i < draws * (part + 1) / parts; i++) {
        const LONG left = i % width, top = i / width % height;
        D3D11_VIEWPORT viewport = whole_viewport;
        D3D11_RECT scissor = whole_scissor;
        if (change_viewport) {
          viewport = {float(left), float(top), float(1 + i / (width * height) % width),
                      float(1 + i / (width * height * width) % height), 0, 1};
          record->RSSetViewports(1, &viewport);
        }
        if (change_scissor) {
          scissor = {left, top, left + 1 + LONG(i / (width * height) % (width + 1)),
                     top + 1 + LONG(i / (width * height * (width + 1)) % (height + 1))};
          if (i % (width * height + 1) == width * height)
            scissor.right = scissor.left;
          record->RSSetScissorRects(1, &scissor);
        }
        record->Draw(3, 0);
        for (UINT y = 0; y < height; y++)
          for (UINT x = 0; x < width; x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            want[y * width + x] +=
                px >= viewport.TopLeftX && px < viewport.TopLeftX + viewport.Width &&
                py >= viewport.TopLeftY && py < viewport.TopLeftY + viewport.Height &&
                (!enabled || (px >= scissor.left && px < scissor.right && py >= scissor.top && py < scissor.bottom));
          }
      }
      if (record == deferred.Get()) {
        CHECK(deferred->FinishCommandList(FALSE, &lists[part]));
      }
    }
    if (record == deferred.Get())
      for (UINT part = 0; part < parts; part++)
        context->ExecuteCommandList(lists[part].Get(), FALSE);

    step("%s: pixel counts after %u draws", names[c], draws);
    context->CopyResource(staging.Get(), target.Get());
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto until = GetTickCount64() + wait_ms;
    HRESULT hr;
    do {
      hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
      if (hr != DXGI_ERROR_WAS_STILL_DRAWING || GetTickCount64() >= until)
        break;
      Sleep(1);
    } while (true);
    if (!expect(hr == S_OK, "%s: readback returned %#lx within %lu ms", names[c], hr, wait_ms))
      ExitProcess(verdict());
    UINT wrong = 0;
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        const float got = ((const float *)((const char *)mapped.pData + y * mapped.RowPitch))[x];
        if (got != float(want[y * width + x]) && wrong++ < report_limit)
          expect(false, "%s: pixel %u,%u is %g, expected %u", names[c], x, y, got, want[y * width + x]);
      }
    trace::wrong += wrong - std::min(wrong, report_limit);
    context->Unmap(staging.Get(), 0);
    if (!expect(device->GetDeviceRemovedReason() == S_OK, "%s: device removed during readback", names[c]))
      ExitProcess(verdict());
  }
  step("release rectangle test resources");
  return verdict();
}
