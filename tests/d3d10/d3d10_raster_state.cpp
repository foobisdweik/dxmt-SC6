// contract: D3D11.3 15.1: "Rasterizer state is encapsulated in a object, which once created can not be edited."
// CreateRasterizerState permits 4096 unique live descriptions and reuses equivalent objects. RSGetViewports and
// RSGetScissorRects zero unused requested entries. this checks the D3D10 interfaces at both10.0 and10.1.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-createrasterizerstate
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-rsgetviewports
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include <array>
#include <climits>
#include <cmath>
#include <limits>

int main() {
  auto references = [](IUnknown *object) {
    object->AddRef();
    return object->Release();
  };
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  for (auto level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, level, D3D10_1_SDK_VERSION, &device));
    step("Direct3D10 level %#x unique raster state limit and released-slot reuse", level);
    {
      std::vector<ComPtr<ID3D10RasterizerState>> states(D3D10_REQ_RASTERIZER_OBJECT_COUNT_PER_CONTEXT);
      D3D10_RASTERIZER_DESC desc{D3D10_FILL_SOLID, D3D10_CULL_NONE};
      desc.DepthClipEnable = TRUE;
      for (UINT i = 0; i < states.size(); i++) {
        desc.DepthBias = INT(i);
        CHECK(device->CreateRasterizerState(&desc, &states[i]));
      }
      desc.DepthBias = INT(states.size());
      ComPtr<ID3D10RasterizerState> extra;
      HRESULT hr = device->CreateRasterizerState(&desc, &extra);
      expect(hr == D3D10_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS && !extra, "unique-object overflow returned %#lx", hr);
      extra.Reset();
      states.back().Reset();
      CHECK(device->CreateRasterizerState(&desc, &extra));
    }
    step("Direct3D10 level %#x all fill/cull/Boolean state fields", level);
    const INT biases[] = {INT_MIN, -1, 0, 1, INT_MAX};
    for (auto fill : {D3D10_FILL_WIREFRAME, D3D10_FILL_SOLID})
      for (auto cull : {D3D10_CULL_NONE, D3D10_CULL_FRONT, D3D10_CULL_BACK})
        for (unsigned bits = 0; bits < (1u << 5); bits++) {
          D3D10_RASTERIZER_DESC desc{};
          desc.FillMode = fill;
          desc.CullMode = cull;
          desc.FrontCounterClockwise = bits & 1;
          desc.DepthClipEnable = bits >> 1 & 1;
          desc.ScissorEnable = bits >> 2 & 1;
          desc.MultisampleEnable = bits >> 3 & 1;
          desc.AntialiasedLineEnable = bits >> 4 & 1;
          desc.DepthBias = biases[bits % std::size(biases)];
          desc.DepthBiasClamp = (bits & 1 ? -1 : 1) * 0.25f;
          desc.SlopeScaledDepthBias = (bits & 2 ? -1 : 1) * 0.5f;
          ComPtr<ID3D10RasterizerState> state, again;
          CHECK(device->CreateRasterizerState(&desc, &state));
          CHECK(device->CreateRasterizerState(&desc, &again));
          expect(state.Get() == again.Get(), "equivalent raster descriptions have distinct identity");
          D3D10_RASTERIZER_DESC got{};
          state->GetDesc(&got);
          expect(!memcmp(&got, &desc, sizeof(desc)), "GetDesc changed fields for bits %#x", bits);
          ComPtr<ID3D10DeviceChild> child;
          ComPtr<IUnknown> a, b;
          CHECK(state.As(&child));
          CHECK(state.As(&a));
          CHECK(child.As(&b));
          expect(a.Get() == b.Get(), "state and device-child IUnknown identities differ");
          device->RSSetState(state.Get());
          ComPtr<ID3D10RasterizerState> bound;
          auto before = references(state.Get());
          device->RSGetState(&bound);
          expect(references(state.Get()) == before + 1, "RSGetState did not AddRef its output");
          expect(bound.Get() == state.Get(), "RSGetState did not retain bound identity");
          bound.Reset();
          expect(references(state.Get()) == before, "RSGetState output Release did not balance AddRef");
          device->RSSetState(nullptr);
          device->RSGetState(bound.ReleaseAndGetAddressOf());
          expect(!bound, "default raster state getter did not return null");
        }
    step("Direct3D10 level %#x floating state fields retain IEEE categories", level);
    for (float value : {-0.0f, 0.0f, std::numeric_limits<float>::denorm_min(),
                        -std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
      for (bool slope : {false, true}) {
        D3D10_RASTERIZER_DESC desc{D3D10_FILL_SOLID, D3D10_CULL_NONE};
        if (slope)
          desc.SlopeScaledDepthBias = value;
        else
          desc.DepthBiasClamp = value;
        ComPtr<ID3D10RasterizerState> state;
        CHECK(device->CreateRasterizerState(&desc, &state));
        D3D10_RASTERIZER_DESC got;
        state->GetDesc(&got);
        auto equal = [](float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); };
        expect(equal(got.DepthBiasClamp, desc.DepthBiasClamp) &&
                   equal(got.SlopeScaledDepthBias, desc.SlopeScaledDepthBias),
               "floating state value changed");
        got.DepthBiasClamp = desc.DepthBiasClamp;
        got.SlopeScaledDepthBias = desc.SlopeScaledDepthBias;
        expect(!memcmp(&got, &desc, sizeof(desc)), "non-floating state fields changed");
      }
    step("Direct3D10 level %#x invalid fill/cull values", level);
    for (bool fill : {false, true})
      for (UINT value : {0u, 1u, 4u, UINT_MAX}) {
        if (!fill && value == D3D10_CULL_NONE)
          continue;
        D3D10_RASTERIZER_DESC desc{D3D10_FILL_SOLID, D3D10_CULL_NONE};
        if (fill)
          desc.FillMode = D3D10_FILL_MODE(value);
        else
          desc.CullMode = D3D10_CULL_MODE(value);
        ComPtr<ID3D10RasterizerState> state;
        HRESULT hr = device->CreateRasterizerState(&desc, &state);
        expect(hr == E_INVALIDARG && !state, "invalid %s %#x returned %#lx", fill ? "fill" : "cull", value, hr);
      }
    // 15.6.1: "The runtime validates the parameters to be in valid range, skipping the call if there is an error".
    step("Direct3D10 level %#x invalid viewport fields preserve preceding state", level);
    D3D10_VIEWPORT valid{0, 0, 16, 16, 0, 1};
    std::vector<D3D10_VIEWPORT> invalid;
    for (INT D3D10_VIEWPORT::*field : {&D3D10_VIEWPORT::TopLeftX, &D3D10_VIEWPORT::TopLeftY})
      for (INT value : {D3D10_VIEWPORT_BOUNDS_MIN - 1, D3D10_VIEWPORT_BOUNDS_MAX}) {
        auto v = valid;
        v.*field = value;
        invalid.push_back(v);
      }
    for (UINT D3D10_VIEWPORT::*field : {&D3D10_VIEWPORT::Width, &D3D10_VIEWPORT::Height}) {
      auto v = valid;
      v.*field = UINT_MAX;
      invalid.push_back(v);
    }
    for (FLOAT D3D10_VIEWPORT::*field : {&D3D10_VIEWPORT::MinDepth, &D3D10_VIEWPORT::MaxDepth})
      for (float value :
           {-std::numeric_limits<float>::epsilon(), std::nextafter(1.0f, 2.0f), std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
        auto v = valid;
        v.*field = value;
        invalid.push_back(v);
      }
    auto reversed = valid;
    reversed.MinDepth = 1;
    reversed.MaxDepth = 0;
    invalid.push_back(reversed);
    for (const auto &v : invalid) {
      device->RSSetViewports(1, &valid);
      device->RSSetViewports(1, &v);
      D3D10_VIEWPORT got;
      UINT count = 1;
      device->RSGetViewports(&count, &got);
      expect(count == 1 && !memcmp(&got, &valid, sizeof(valid)), "invalid viewport replaced preceding state");
    }
    step("Direct3D10 level %#x viewport/scissor getter capacities and canaries", level);
    constexpr UINT capacity = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    std::array<D3D10_VIEWPORT, capacity + 1> vps{};
    std::array<D3D10_RECT, capacity + 1> rects{};
    for (UINT i = 0; i < vps.size(); i++) {
      vps[i] = {INT(i), INT(i), i + 1, i + 2, 0.25f, 0.75f};
      rects[i] = {LONG(i), LONG(i), LONG(i + 1), LONG(i + 2)};
    }
    for (UINT bound : {0u, 1u, capacity - 1, capacity}) {
      device->RSSetViewports(bound, vps.data());
      device->RSSetScissorRects(bound, rects.data());
      UINT number = UINT_MAX;
      device->RSGetViewports(&number, nullptr);
      expect(number == bound, "viewport count query %u", number);
      number = UINT_MAX;
      device->RSGetScissorRects(&number, nullptr);
      expect(number == bound, "scissor count query %u", number);
      for (UINT requested : {0u, 1u, capacity - 1, capacity}) {
        decltype(vps) got_vp;
        decltype(rects) got_rect;
        memset(got_vp.data(), 0x7f, sizeof(got_vp));
        memset(got_rect.data(), 0x7f, sizeof(got_rect));
        auto before_vp = got_vp;
        auto before_rect = got_rect;
        number = requested;
        device->RSGetViewports(&number, got_vp.data());
        UINT rect_number = requested;
        device->RSGetScissorRects(&rect_number, got_rect.data());
        if (requested >= bound)
          expect(number == bound && rect_number == bound, "getter bound count mismatch");
        for (UINT i = 0; i <= capacity; i++) {
          auto want_vp = i >= requested ? before_vp[i] : i < bound ? vps[i] : D3D10_VIEWPORT{};
          auto want_rect = i >= requested ? before_rect[i] : i < bound ? rects[i] : D3D10_RECT{};
          expect(!memcmp(&got_vp[i], &want_vp, sizeof(want_vp)), "viewport entry %u requested %u bound %u", i,
                 requested, bound);
          expect(!memcmp(&got_rect[i], &want_rect, sizeof(want_rect)), "scissor entry %u requested %u bound %u", i,
                 requested, bound);
        }
      }
    }
    device->RSSetViewports(capacity + 1, vps.data());
    device->RSSetScissorRects(capacity + 1, rects.data());
    std::array<D3D10_VIEWPORT, capacity> got_vps;
    std::array<D3D10_RECT, capacity> got_rects;
    UINT count = capacity;
    device->RSGetViewports(&count, got_vps.data());
    expect(count == capacity && !memcmp(got_vps.data(), vps.data(), sizeof(got_vps)),
           "excess viewport count replaced valid state");
    count = capacity;
    device->RSGetScissorRects(&count, got_rects.data());
    expect(count == capacity && !memcmp(got_rects.data(), rects.data(), sizeof(got_rects)),
           "excess scissor count replaced valid state");
    D3D10_RASTERIZER_DESC nondefault{D3D10_FILL_WIREFRAME, D3D10_CULL_FRONT};
    ComPtr<ID3D10RasterizerState> state;
    CHECK(device->CreateRasterizerState(&nondefault, &state));
    device->RSSetState(state.Get());
    device->ClearState();
    device->RSGetState(state.ReleaseAndGetAddressOf());
    expect(!state, "ClearState retained raster state");
    count = UINT_MAX;
    device->RSGetViewports(&count, nullptr);
    expect(!count, "ClearState retained viewports");
    count = UINT_MAX;
    device->RSGetScissorRects(&count, nullptr);
    expect(!count, "ClearState retained scissors");
  }
  return verdict();
}
