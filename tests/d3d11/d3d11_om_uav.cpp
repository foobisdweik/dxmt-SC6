// contract: OMSetRenderTargetsAndUnorderedAccessViews with
// KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL must "Unbind all render targets in
// binding slots >= UAVStartSlot" and "Unbind all render targets that conflict
// with the UAVs" (Microsoft Learn, case 2; quotations split to name the two
// independent hazards).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargetsandunorderedaccessviews
// the shared output namespace is defined by D3D11.3 5.3.9; surviving RTVs and
// the newly bound UAV must both write.
#include "d3d11_test.hpp"
#include <array>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv=float2((id << 1)&2,id&2); return float4(uv*float2(2,-2)+float2(-1,1),0,1);
}
RWBuffer<uint> words : register(u1);
RWTexture2D<uint> image : register(u2);
uint ps_slot(float4 pos : SV_Position) : SV_Target0 { words[uint(pos.x)]=UAV_VALUE+uint(pos.x); return RTV_VALUE; }
uint ps_alias(float4 pos : SV_Position) : SV_Target1 { image[uint2(pos.xy)]=UAV_VALUE+uint(pos.x); return RTV_VALUE; }
)hlsl";

int main() {
  const UINT width = 3, rtv_value = 71, uav_value = 113;
  const std::vector<std::string> defines{
      "UAV_VALUE=" + std::to_string(uav_value),
      "RTV_VALUE=" + std::to_string(rtv_value)};
  auto vertex = compile(hlsl, "vs", "vs", defines);
  ComPtr<ID3DBlob> pixels[] = {compile(hlsl, "ps_slot", "ps", defines),
                               compile(hlsl, "ps_alias", "ps", defines)};
  if (!expect(vertex && pixels[0] && pixels[1], "OM UAV HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level,
                          1, D3D11_SDK_VERSION, &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps[std::size(pixels)];
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(),
                                   vertex->GetBufferSize(), nullptr, &vs));
  for (UINT i = 0; i < std::size(pixels); i++)
    CHECK(device->CreatePixelShader(pixels[i]->GetBufferPointer(),
                                    pixels[i]->GetBufferSize(), nullptr,
                                    &ps[i]));
  for (bool recorded : {false, true})
    for (bool alias : {false, true}) {
      step("OM KEEP RTVs, deferred %u, resource alias %u", recorded, alias);
      immediate->ClearState();
      deferred->ClearState();
      auto context = recorded ? deferred.Get() : immediate.Get();
      const UINT initial[] = {17, 29};
      ComPtr<ID3D11Texture2D> textures[std::size(initial)], staging;
      ComPtr<ID3D11RenderTargetView> views[std::size(initial)];
      ID3D11RenderTargetView *bound[std::size(initial)];
      D3D11_TEXTURE2D_DESC desc{width,
                                1,
                                1,
                                1,
                                DXGI_FORMAT_R32_UINT,
                                {1, 0},
                                D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET |
                                    D3D11_BIND_UNORDERED_ACCESS};
      for (UINT i = 0; i < std::size(initial); i++) {
        std::vector<UINT> seed(width, initial[i]);
        D3D11_SUBRESOURCE_DATA data{seed.data(), width * sizeof(UINT)};
        CHECK(device->CreateTexture2D(&desc, &data, &textures[i]));
        CHECK(device->CreateRenderTargetView(textures[i].Get(), nullptr,
                                             &views[i]));
        bound[i] = views[i].Get();
      }
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
      D3D11_BUFFER_DESC bd{width * sizeof(UINT), D3D11_USAGE_DEFAULT,
                           D3D11_BIND_UNORDERED_ACCESS};
      std::vector<UINT> zeros(width);
      D3D11_SUBRESOURCE_DATA data{zeros.data()};
      ComPtr<ID3D11Buffer> words;
      CHECK(device->CreateBuffer(&bd, &data, &words));
      ComPtr<ID3D11UnorderedAccessView> uav;
      D3D11_UNORDERED_ACCESS_VIEW_DESC ud{DXGI_FORMAT_R32_UINT,
                                          alias ? D3D11_UAV_DIMENSION_TEXTURE2D
                                                : D3D11_UAV_DIMENSION_BUFFER};
      if (!alias)
        ud.Buffer = {0, width, 0};
      CHECK(device->CreateUnorderedAccessView(
          alias ? static_cast<ID3D11Resource *>(textures[0].Get())
                : words.Get(),
          &ud, &uav));
      const UINT slot = alias ? 2 : 1;
      for (UINT count : {0u, 1u}) {
        context->OMSetRenderTargets(std::size(bound), bound, nullptr);
        ID3D11UnorderedAccessView *none = nullptr;
        context->OMSetRenderTargetsAndUnorderedAccessViews(
            D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, slot, count, &none, nullptr);
        ID3D11RenderTargetView *kept[std::size(bound)]{};
        context->OMGetRenderTargets(std::size(kept), kept, nullptr);
        for (UINT i = 0; i < std::size(kept); i++) {
          expect(kept[i] == (i < slot ? views[i].Get() : nullptr), "empty UAV update retained RTV slot %u", i);
          if (kept[i])
            kept[i]->Release();
        }
      }
      context->OMSetRenderTargets(std::size(bound), bound, nullptr);
      context->OMSetRenderTargetsAndUnorderedAccessViews(
          D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, slot,
          1, uav.GetAddressOf(), nullptr);
      ComPtr<ID3D11RenderTargetView> got_rt[std::size(bound)];
      ComPtr<ID3D11UnorderedAccessView> got_uav;
      ID3D11RenderTargetView *raw[std::size(bound)]{};
      context->OMGetRenderTargetsAndUnorderedAccessViews(
          std::size(raw), raw, nullptr, slot, 1, &got_uav);
      bool bindings = true;
      for (UINT i = 0; i < std::size(raw); i++) {
        got_rt[i].Attach(raw[i]);
        bool retained = alias ? i != 0 : i < slot;
        bindings &=
            expect(got_rt[i].Get() == (retained ? views[i].Get() : nullptr),
                   "RTV %u was not %s", i, retained ? "retained" : "unbound");
      }
      bindings &= expect(got_uav == uav, "new UAV not bound");
      if (!bindings)
        return verdict();
      D3D11_VIEWPORT viewport{0, 0, float(width), 1, 0, 1};
      context->RSSetViewports(1, &viewport);
      context->VSSetShader(vs.Get(), nullptr, 0);
      context->PSSetShader(ps[alias].Get(), nullptr, 0);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context->Draw(3, 0);
      std::array<ID3D11UnorderedAccessView *, D3D11_PS_CS_UAV_REGISTER_COUNT>
          none{};
      context->OMSetRenderTargetsAndUnorderedAccessViews(
          0, nullptr, nullptr, 0, none.size(), none.data(), nullptr);
      if (recorded) {
        ComPtr<ID3D11CommandList> commands;
        CHECK(deferred->FinishCommandList(FALSE, &commands));
        immediate->ExecuteCommandList(commands.Get(), FALSE);
      }
      for (UINT i = 0; i < std::size(textures); i++) {
        immediate->CopyResource(staging.Get(), textures[i].Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        for (UINT x = 0; x < width; x++) {
          UINT want = alias ? (i ? rtv_value : uav_value + x)
                      : i   ? initial[i]
                            : rtv_value;
          expect(static_cast<const UINT *>(mapped.pData)[x] == want,
                 "texture %u pixel %u has %u, want %u", i, x,
                 static_cast<const UINT *>(mapped.pData)[x], want);
        }
        immediate->Unmap(staging.Get(), 0);
      }
      auto values = read(device.Get(), immediate.Get(), words.Get());
      if (expect(values.size() == width, "buffer UAV readback failed"))
        for (UINT x = 0; x < width; x++)
          expect(values[x] == (alias ? 0 : uav_value + x),
                 "buffer UAV element %u has %u", x, values[x]);
    }
  return verdict();
}
