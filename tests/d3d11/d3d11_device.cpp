// contract: what a Direct3D 11 device answers about itself, as the documentation has it.
// - ID3D11Device::CheckMultisampleQualityLevels: "if this method returns S_OK and pNumQualityLevels receives a positive
//   value, then the format and sample count combination is supported for the device": a texture of every format and
//   count it grants can be created; one sample is every creatable format's; a tiled multisampled texture is none's.
// - IDXGIDevice1::SetMaximumFrameLatency: "the value defaults to 3, but can range from 1 to 16. A value of 0 will
//   reset latency to the default"; outside that it is DXGI_ERROR_INVALID_CALL.
// - the device has no counters (D3D11_COUNTER_INFO: LastDeviceDependentCounter 0), so none can be created
//   (ID3D11Device::CreateCounter: E_INVALIDARG for an out-of-range counter), and keeps the exception mode it is given.
// - a query is ID3D11Asynchronous; the queries that are predicates are ID3D11Predicate, and only those are made by
//   CreatePredicate.
// - a staging texture mapped with D3D11_MAP_FLAG_DO_NOT_WAIT while a copy to it is pending gives
//   DXGI_ERROR_WAS_STILL_DRAWING until the copy is done, and then its data: asking again is enough
//   (ID3D11DeviceContext::Map).
// - optional creation outputs validate the description: "the method will return S_FALSE if the other input
//   parameters pass validation" (ID3D11Device3::CreateRasterizerState2, Microsoft Learn).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11device3-createrasterizerstate2
// CreateQuery1 has the same validation-only contract; D3D11_QUERY ends at SO_OVERFLOW_PREDICATE_STREAM3.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11device3-createquery1
// - "If the Pixel Shader outputs oMask (output coverage)(16.9.4), Alpha-to-Coverage is disabled." (D3D11.3 17.18).
//   alpha zero with coverage one still writes red; coverage zero and ordinary A2C still write nothing.
//   all three native pipeline paths must apply the same rule.
#include "d3d11_test.hpp"
#include <d3d11_3.h>
#include <dxgi1_2.h>

int
main() {
  const char coverage_hlsl[] = R"hlsl(
struct V { float4 pos : SV_Position; };
V vs(uint id : SV_VertexID) {
  V v; v.pos = float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); return v;
}
struct C { float4 pos : POSITION; };
C vs_patch(uint id : SV_VertexID) { C v; v.pos = vs(id).pos; return v; }
[maxvertexcount(3)] void gs(triangle V verts[3], inout TriangleStream<V> stream) {
  for (uint i = 0; i < 3; i++) { V v = verts[i]; v.pos.xy *= 0.75; stream.Append(v); }
}
struct T { float edges[3] : SV_TessFactor; float inner : SV_InsideTessFactor; };
T tf(InputPatch<C, 3> verts) { T t; t.edges[0] = t.edges[1] = t.edges[2] = t.inner = 1; return t; }
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")]
[outputcontrolpoints(3)] [patchconstantfunc("tf")]
C hs(InputPatch<C, 3> verts, uint id : SV_OutputControlPointID) { return verts[id]; }
[domain("tri")] V ds(T t, OutputPatch<C, 3> verts, float3 bary : SV_DomainLocation) {
  V v; v.pos = verts[0].pos * bary.x + verts[1].pos * bary.y + verts[2].pos * bary.z; return v;
}
float4 ps_plain() : SV_Target { return float4(1, 0, 0, 0); }
struct O { float4 color : SV_Target; uint coverage : SV_Coverage; };
O ps_mask() { O o; o.color = ps_plain(); o.coverage = 1; return o; }
O ps_none() { O o = ps_mask(); o.coverage = 0; return o; }
)hlsl";
  auto vs_code = compile(coverage_hlsl, "vs", "vs"), gs_code = compile(coverage_hlsl, "gs", "gs"),
       hs_code = compile(coverage_hlsl, "hs", "hs"), ds_code = compile(coverage_hlsl, "ds", "ds"),
       patch_code = compile(coverage_hlsl, "vs_patch", "vs");
  ComPtr<ID3DBlob> ps_code[] = {compile(coverage_hlsl, "ps_plain", "ps"), compile(coverage_hlsl, "ps_mask", "ps"),
                               compile(coverage_hlsl, "ps_none", "ps")};
  if (!expect(vs_code && gs_code && hs_code && ds_code && patch_code && ps_code[0] && ps_code[1] && ps_code[2],
              "coverage HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const std::string &what) {
    checks++;
    if (!ok && wrong++ < 16)
      ::expect(false, "%s", what.c_str());
  };

  {
    ComPtr<ID3D11VertexShader> vs, vs_patch;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11PixelShader> ps[std::size(ps_code)];
    CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
    CHECK(device->CreateVertexShader(patch_code->GetBufferPointer(), patch_code->GetBufferSize(), nullptr, &vs_patch));
    CHECK(device->CreateGeometryShader(gs_code->GetBufferPointer(), gs_code->GetBufferSize(), nullptr, &gs));
    CHECK(device->CreateHullShader(hs_code->GetBufferPointer(), hs_code->GetBufferSize(), nullptr, &hs));
    CHECK(device->CreateDomainShader(ds_code->GetBufferPointer(), ds_code->GetBufferSize(), nullptr, &ds));
    for (UINT i = 0; i < std::size(ps); i++)
      CHECK(device->CreatePixelShader(ps_code[i]->GetBufferPointer(), ps_code[i]->GetBufferSize(), nullptr, &ps[i]));
    D3D11_TEXTURE2D_DESC desc{1, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                             D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    CHECK(device->CreateTexture2D(&desc, nullptr, &target));
    CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
    D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    ComPtr<ID3D11RasterizerState> raster;
    CHECK(device->CreateRasterizerState(&raster_desc, &raster));
    const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
    const float clear[4]{};
    context->RSSetState(raster.Get());
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    for (bool alpha : {false, true}) {
      D3D11_BLEND_DESC blend_desc{};
      blend_desc.AlphaToCoverageEnable = alpha;
      blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      ComPtr<ID3D11BlendState> blend;
      CHECK(device->CreateBlendState(&blend_desc, &blend));
      context->OMSetBlendState(blend.Get(), nullptr, ~0u);
      for (UINT path = 0; path < 3; path++) {
        context->VSSetShader(path == 2 ? vs_patch.Get() : vs.Get(), nullptr, 0);
        context->GSSetShader(path == 1 ? gs.Get() : nullptr, nullptr, 0);
        context->HSSetShader(path == 2 ? hs.Get() : nullptr, nullptr, 0);
        context->DSSetShader(path == 2 ? ds.Get() : nullptr, nullptr, 0);
        context->IASetPrimitiveTopology(path == 2 ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                                : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (UINT kind = 0; kind < std::size(ps); kind++) {
          step("coverage pipeline %u alpha %u pixel shader %u", path, alpha, kind);
          context->PSSetShader(ps[kind].Get(), nullptr, 0);
          context->ClearRenderTargetView(rtv.Get(), clear);
          context->Draw(3, 0);
          context->CopyResource(staging.Get(), target.Get());
          D3D11_MAPPED_SUBRESOURCE mapped{};
          CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
          const float wanted[] = {kind == 1 || (!kind && !alpha) ? 1.0f : 0.0f, 0, 0, 0};
          expect(!memcmp(mapped.pData, wanted, sizeof(wanted)), "A2C and shader coverage changed the pixel");
          context->Unmap(staging.Get(), 0);
        }
      }
    }
    context->ClearState();
  }

  step("creation descriptions and optional outputs");
  {
    ComPtr<ID3D11Device3> device3;
    CHECK(device.As(&device3));
    ID3D11BlendState *blend = reinterpret_cast<ID3D11BlendState *>(device.Get());
    ID3D11RasterizerState *raster = reinterpret_cast<ID3D11RasterizerState *>(device.Get());
    ID3D11RasterizerState1 *raster1 = reinterpret_cast<ID3D11RasterizerState1 *>(device.Get());
    ID3D11Query *query = reinterpret_cast<ID3D11Query *>(device.Get());
    expect(device->CreateBlendState(nullptr, &blend) == E_INVALIDARG && !blend,
           "null blend description retained output");
    expect(device->CreateRasterizerState(nullptr, &raster) == E_INVALIDARG && !raster,
           "null raster description retained output");
    expect(device3->CreateRasterizerState1(nullptr, &raster1) == E_INVALIDARG && !raster1,
           "null raster1 description retained output");
    expect(device->CreateQuery(nullptr, &query) == E_INVALIDARG && !query, "null query description retained output");
    for (UINT kind : {UINT(D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3) + 1, ~0u}) {
      D3D11_QUERY_DESC desc{D3D11_QUERY(kind)};
      D3D11_QUERY_DESC1 desc1{D3D11_QUERY(kind)};
      query = reinterpret_cast<ID3D11Query *>(device.Get());
      ID3D11Query1 *query1 = reinterpret_cast<ID3D11Query1 *>(device.Get());
      expect(FAILED(device->CreateQuery(&desc, &query)) && !query, "invalid query retained output");
      expect(FAILED(device3->CreateQuery1(&desc1, &query1)) && !query1, "invalid query1 retained output");
      expect(FAILED(device->CreateQuery(&desc, nullptr)), "invalid query validation-only call");
      expect(FAILED(device3->CreateQuery1(&desc1, nullptr)), "invalid query1 validation-only call");
    }
    for (auto kind : {D3D11_QUERY_EVENT, D3D11_QUERY_OCCLUSION, D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3}) {
      D3D11_QUERY_DESC query_desc{kind};
      D3D11_QUERY_DESC1 query_desc1{kind};
      expect(device->CreateQuery(&query_desc, nullptr) == S_FALSE, "query validation-only call");
      expect(device3->CreateQuery1(&query_desc1, nullptr) == S_FALSE, "query1 validation-only call");
    }
    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    expect(device->CreateBlendState(&blend_desc, nullptr) == S_FALSE, "blend validation-only call");
    D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    D3D11_RASTERIZER_DESC1 raster_desc1{};
    memcpy(&raster_desc1, &raster_desc, sizeof(raster_desc));
    expect(device->CreateRasterizerState(&raster_desc, nullptr) == S_FALSE, "raster validation-only call");
    expect(device3->CreateRasterizerState1(&raster_desc1, nullptr) == S_FALSE, "raster1 validation-only call");
    raster_desc.FillMode = D3D11_FILL_MODE(0);
    raster_desc1.CullMode = D3D11_CULL_MODE(0);
    expect(device->CreateRasterizerState(&raster_desc, nullptr) == E_INVALIDARG, "invalid raster validation-only call");
    expect(device3->CreateRasterizerState1(&raster_desc1, nullptr) == E_INVALIDARG,
           "invalid raster1 validation-only call");
  }

  // multisample quality levels, over every format the header names
  unsigned granted = 0;
  for (UINT f = DXGI_FORMAT_UNKNOWN + 1; f <= DXGI_FORMAT_B4G4R4A4_UNORM; f++) {
    auto format = DXGI_FORMAT(f);
    UINT support = 0;
    if (FAILED(device->CheckFormatSupport(format, &support)) || !(support & D3D11_FORMAT_SUPPORT_TEXTURE2D))
      continue;
    // bound as what the format can be multisampled as
    UINT bind = support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL ? D3D11_BIND_DEPTH_STENCIL
                : support & D3D11_FORMAT_SUPPORT_RENDER_TARGET ? D3D11_BIND_RENDER_TARGET
                                                               : 0;
    for (UINT samples : {1u, 2u, 4u, 8u}) {
      UINT levels = ~0u;
      HRESULT hr = device->CheckMultisampleQualityLevels(format, samples, &levels);
      if (FAILED(hr) || !levels || (samples > 1 && !bind))
        continue;
      granted++;
      D3D11_TEXTURE2D_DESC desc{16, 16, 1, 1, format, {samples, 0}, D3D11_USAGE_DEFAULT, samples > 1 ? bind : D3D11_BIND_SHADER_RESOURCE};
      if (samples == 1 && !(support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) && !(support & D3D11_FORMAT_SUPPORT_SHADER_LOAD))
        desc.BindFlags = bind;
      ComPtr<ID3D11Texture2D> texture;
      expect(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)),
             "format " + std::to_string(f) + " has quality levels for " + std::to_string(samples) + " samples, and no texture of them");
    }
  }
  expect(granted > 0, "no format has quality levels");
  {
    UINT levels = ~0u;
    expect(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, nullptr) == E_INVALIDARG,
           "quality levels are written to no pointer");
    expect(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &levels) == E_FAIL && levels == 0,
           "no samples have quality levels");
    ComPtr<ID3D11Device2> device2;
    if (SUCCEEDED(device.As(&device2))) {
      levels = ~0u;
      expect(SUCCEEDED(device2->CheckMultisampleQualityLevels1(
                 DXGI_FORMAT_R8G8B8A8_UNORM, 4, D3D11_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE, &levels
             )) && levels == 0,
             "a tiled multisampled texture has quality levels");
    }
  }

  // frame latency
  {
    ComPtr<IDXGIDevice1> dxgi;
    CHECK(device.As(&dxgi));
    const UINT documented_default = 3, most = 16;
    UINT latency = 0;
    expect(SUCCEEDED(dxgi->GetMaximumFrameLatency(&latency)) && latency == documented_default, "the frame latency does not default to 3");
    expect(SUCCEEDED(dxgi->SetMaximumFrameLatency(most)) && SUCCEEDED(dxgi->GetMaximumFrameLatency(&latency)) && latency == most,
           "the largest frame latency is not kept");
    expect(dxgi->SetMaximumFrameLatency(most + 1) == DXGI_ERROR_INVALID_CALL &&
               SUCCEEDED(dxgi->GetMaximumFrameLatency(&latency)) && latency == most,
           "a frame latency past the largest is taken");
    expect(SUCCEEDED(dxgi->SetMaximumFrameLatency(0)) && SUCCEEDED(dxgi->GetMaximumFrameLatency(&latency)) && latency == documented_default,
           "a frame latency of 0 is not the default again");
    expect(dxgi->GetMaximumFrameLatency(nullptr) == DXGI_ERROR_INVALID_CALL, "the frame latency is written to no pointer");
  }

  // counters and the exception mode
  {
    D3D11_COUNTER_INFO info{D3D11_COUNTER_DEVICE_DEPENDENT_0, 1, 1};
    device->CheckCounterInfo(&info);
    expect(info.LastDeviceDependentCounter == 0 && info.NumSimultaneousCounters == 0, "the device says it has counters");
    D3D11_COUNTER_DESC counter_desc{D3D11_COUNTER_DEVICE_DEPENDENT_0};
    ComPtr<ID3D11Counter> counter;
    expect(device->CreateCounter(&counter_desc, &counter) == E_INVALIDARG && !counter, "a counter the device does not have is created");
    expect(device->SetExceptionMode(D3D11_RAISE_FLAG_DRIVER_INTERNAL_ERROR) == S_OK &&
               device->GetExceptionMode() == D3D11_RAISE_FLAG_DRIVER_INTERNAL_ERROR && device->SetExceptionMode(0) == S_OK &&
               device->GetExceptionMode() == 0,
           "the exception mode set is not the one got");
  }

  // queries and predicates
  {
    D3D11_QUERY_DESC occlusion{D3D11_QUERY_OCCLUSION}, is_predicate{D3D11_QUERY_OCCLUSION_PREDICATE};
    ComPtr<ID3D11Query> query;
    ComPtr<ID3D11Predicate> predicate, none;
    ComPtr<ID3D11Asynchronous> asynchronous;
    CHECK(device->CreateQuery(&occlusion, &query));
    expect(SUCCEEDED(query.As(&asynchronous)), "a query is not ID3D11Asynchronous");
    expect(FAILED(query.As(&none)), "an occlusion query is a predicate");
    expect(device->CreatePredicate(&occlusion, &none) == E_INVALIDARG, "an occlusion query is made a predicate");
    expect(SUCCEEDED(device->CreatePredicate(&is_predicate, &predicate)) && predicate, "no predicate of an occlusion predicate query");
    ComPtr<ID3D11Query> as_query;
    expect(predicate && SUCCEEDED(predicate.As(&as_query)), "a predicate is not a query");
  }

  // a staging texture whose copy is pending
  {
    const UINT size = 64, texel = 0x11223344;
    std::vector<UINT> texels(size * size, texel);
    D3D11_TEXTURE2D_DESC desc{size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    D3D11_SUBRESOURCE_DATA initial{texels.data(), size * 4};
    ComPtr<ID3D11Texture2D> source, staging;
    CHECK(device->CreateTexture2D(&desc, &initial, &source));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), source.Get());
    // asked again without a flush, for no longer than a slow copy takes
    const UINT attempts = 5000;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = DXGI_ERROR_WAS_STILL_DRAWING;
    UINT asked = 0;
    for (; asked < attempts && hr == DXGI_ERROR_WAS_STILL_DRAWING; asked++) {
      hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
      if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        Sleep(1);
    }
    expect(hr == S_OK, "a staging texture asked for without waiting is never ready");
    if (hr == S_OK) {
      expect(*(const UINT *)mapped.pData == texel && *((const UINT *)((const char *)mapped.pData + (size - 1) * mapped.RowPitch) + size - 1) == texel,
             "a staging texture mapped without waiting does not hold the copy");
      context->Unmap(staging.Get(), 0);
    }
  }
  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
