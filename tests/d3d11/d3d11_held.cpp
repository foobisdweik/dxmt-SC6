// the workloads tests/held.py profiles, as tests/d3d12/d3d12_held.cpp has them, through Direct3D 11. argv:
// <gpu|app|hitch|events> <frames>. a frame's GPU time is its two timestamps' difference over the disjoint query's frequency
// (D3D11_QUERY_DATA_TIMESTAMP_DISJOINT); the hitch frame makes new pixel shaders and draws with each, as D3D11 makes
// a pipeline when it is first drawn with. events: frame f draws in the annotation's event "frame" and, inside it,
// ID3D11DeviceContext2's event "draw f".
#include "d3d11_test.hpp"
#include <d3d11_2.h>
#include <dxgi.h>
#include <algorithm>

static const char hlsl[] = R"hlsl(
#ifndef SEED
#define SEED 0
#define TURNS 1
#endif
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
  uint value = uint(pos.x) * 7919u + uint(pos.y) + SEED;
  [loop] for (uint i = 0; i < TURNS; i++) value = value * 1664525u + 1013904223u;
  return float4(value & 255, 0, 0, 255) / 255;
}
)hlsl";
// the most pipelines the hitch frame may make: it makes them until it lasts three of the frames before it, whose
// length the display can set (a frame waits for a drawable), so a hitch is one under any display
static constexpr UINT made = 64, size = 64;
static constexpr DWORD sleep_ms = 20;

template <typename T>
static void
wait(ID3D11DeviceContext *context, ID3D11Query *query, T &data) {
  while (context->GetData(query, &data, sizeof(data), 0) != S_OK)
    ;
}

int
main(int argc, char **argv) {
  std::string mode = argc > 1 ? argv[1] : "gpu";
  const UINT frames = argc > 2 ? atoi(argv[2]) : 48, hitch = frames / 2;
  auto turns = std::to_string(mode == "gpu" ? 1 << 20 : 1);
  auto vs = compile(hlsl, "vs", "vs");
  std::vector<ComPtr<ID3DBlob>> ps;
  // the run's own shaders, which no cache of an earlier run has
  const UINT run = GetCurrentProcessId() ^ GetTickCount();
  for (UINT seed = 0; seed <= (mode == "hitch" ? made : 0); seed++)
    ps.push_back(compile(hlsl, "ps", "ps", {"TURNS=" + turns, "SEED=" + std::to_string(run + seed)}));
  for (auto &code : ps)
    if (!vs || !code) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "held"};
  RegisterClassA(&wc);
  auto hwnd = CreateWindowA("held", "held", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, size, size, nullptr, nullptr,
                            wc.hInstance, nullptr);
  DXGI_SWAP_CHAIN_DESC sc_desc{{size, size, {}, DXGI_FORMAT_R8G8B8A8_UNORM}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT,
                               2, hwnd, TRUE, DXGI_SWAP_EFFECT_FLIP_DISCARD};
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGISwapChain> swapchain;
  CHECK(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                      &sc_desc, &swapchain, &device, nullptr, &context));
  ComPtr<ID3D11VertexShader> vertex;
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
  auto shader = [&](ID3DBlob *code, ComPtr<ID3D11PixelShader> &out) {
    return device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &out);
  };
  ComPtr<ID3D11PixelShader> pixel;
  CHECK(shader(ps[0].Get(), pixel));
  ComPtr<ID3D11Texture2D> back;
  CHECK(swapchain->GetBuffer(0, IID_PPV_ARGS(&back)));
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateRenderTargetView(back.Get(), nullptr, &rtv));
  ComPtr<ID3D11Query> disjoint, begin, end;
  D3D11_QUERY_DESC query_desc{D3D11_QUERY_TIMESTAMP_DISJOINT};
  CHECK(device->CreateQuery(&query_desc, &disjoint));
  query_desc.Query = D3D11_QUERY_TIMESTAMP;
  CHECK(device->CreateQuery(&query_desc, &begin));
  CHECK(device->CreateQuery(&query_desc, &end));
  LARGE_INTEGER qpc;
  QueryPerformanceFrequency(&qpc);

  D3D11_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  auto draw = [&] {
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    context->Draw(3, 0);
  };
  ComPtr<ID3DUserDefinedAnnotation> annotation;
  ComPtr<ID3D11DeviceContext2> context2;
  CHECK(context.As(&annotation));
  CHECK(context.As(&context2));
  auto now = [&] {
    LARGE_INTEGER time;
    QueryPerformanceCounter(&time);
    return time.QuadPart / qpc.QuadPart * 1000000000 + time.QuadPart % qpc.QuadPart * 1000000000 / qpc.QuadPart;
  };
  auto presented = now();
  std::vector<long long> lengths;
  for (UINT f = 0; f < frames; f++) {
    if (mode == "app")
      Sleep(sleep_ms);
    if (mode == "hitch" && f == hitch) {
      std::nth_element(lengths.begin(), lengths.begin() + lengths.size() / 2, lengths.end());
      auto began = now(), median = lengths[lengths.size() / 2];
      for (UINT seed = 1; seed <= made && now() - began < 3 * median; seed++) {
        CHECK(shader(ps[seed].Get(), pixel));
        draw();
        // a pipeline is made once its draw is encoded: the GPU finishing the draw says it was
        context->End(end.Get());
        UINT64 done;
        wait(context.Get(), end.Get(), done);
      }
      if (now() - began < 3 * median) {
        printf("failed: %u pipelines took less than three frames of %lld ns\n", made, median);
        return 1;
      }
      printf("fact hitch %u %lld\n", f, now() - began);
    }
    auto submitted = now();
    context->Begin(disjoint.Get());
    context->End(begin.Get());
    if (mode == "events") {
      if (!annotation->GetStatus()) {
        printf("failed: the annotation is off under a tool\n");
        return 1;
      }
      annotation->BeginEvent(L"frame");
      context2->BeginEventInt((L"draw " + std::to_wstring(f)).c_str(), 0);
      printf("fact event %u %u\n", f, f);
    }
    draw();
    if (mode == "events") {
      context2->EndEvent();
      annotation->EndEvent();
    }
    context->End(end.Get());
    context->End(disjoint.Get());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency;
    UINT64 first, last;
    wait(context.Get(), disjoint.Get(), frequency);
    wait(context.Get(), begin.Get(), first);
    wait(context.Get(), end.Get(), last);
    auto waited = now() - submitted;
    if (!frequency.Disjoint)
      printf("fact gpu %u %lld\n", f, (long long)((last - first) * 1000000000 / frequency.Frequency));
    printf("fact app %u %lld\n", f, now() - presented - waited);
    CHECK(swapchain->Present(0, 0));
    lengths.push_back(now() - presented);
    printf("fact frame %u %lld\n", f, lengths.back());
    presented = now();
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
      DispatchMessageA(&msg);
  }
  if (mode == "app")
    printf("fact sleep %lu\n", sleep_ms * 1000000ul);
  printf("passed: %u frames\n", frames);
  return 0;
}
