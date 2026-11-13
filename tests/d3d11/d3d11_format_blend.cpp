// contract: "The min and max blend ops ignore D3D11_BLEND modes" (D3D11.3 17.1). all other equations use
// their selected source/destination factors (17.1/.2), including the second source (17.6). "Values entering
// Blending Hardware, including the BlendFactor value, are always clamped to the range of the RenderTarget"
// (17.5), with its small-float exception. "For all formats, there is a clamp to the RenderTarget range after
// blend, before writing values out to memory." (17.5). every advertised blendable format receives all legal
// factor pairs, all five operations and independent alpha pairs, then every channel mask. binary and exact
// small-integer inputs exercise clamps without depending on optional intermediate precision. unsigned small
// floats receive no negative inputs because input clamping is optional; signed zero is preserved by the CPU.
#include "d3d11_test.hpp"
#include "../depth_formats.hpp"
#include "../format_blend.hpp"

using namespace format_blend;

int
main() {
  auto vertex = compile(format_blend::hlsl, "vs", "vs"), pixel = compile(format_blend::hlsl, "ps", "ps");
  if (!expect(vertex && pixel, "blend HLSL did not compile"))
    return verdict();
  auto cases = equations();
  if (!expect(
          cases.size() <= D3D11_REQ_BLEND_OBJECT_COUNT_PER_DEVICE &&
              cases.size() <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION,
          "blend cases exceed API object/image limits"
      ))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate
  ));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &ps));
  std::vector<ComPtr<ID3D11BlendState>> states(cases.size());
  for (UINT row = 0; row < cases.size(); row++) {
    const auto &e = cases[row];
    D3D11_BLEND_DESC desc{};
    desc.RenderTarget[0] = {
        TRUE,
        D3D11_BLEND(e.source),
        D3D11_BLEND(e.destination),
        D3D11_BLEND_OP(e.operation),
        D3D11_BLEND(e.alpha_source),
        D3D11_BLEND(e.alpha_destination),
        D3D11_BLEND_OP(e.alpha_operation),
        UINT8(e.write_mask)
    };
    CHECK(device->CreateBlendState(&desc, &states[row]));
  }
  D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  rd.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&rd, &raster));
  UINT tested = 0;
  for (const auto &f : formats()) {
    UINT support = 0;
    HRESULT hr = device->CheckFormatSupport(f.id, &support);
    if (FAILED(hr)) {
      expect(hr == E_FAIL && !support, "%s unsupported query returned %08lx bits %#x", f.name.c_str(), hr, support);
      continue;
    }
    if (!(support & D3D11_FORMAT_SUPPORT_BLENDABLE))
      continue;
    if (!expect(
            f.kind != Unsigned && f.kind != Signed && f.kind != Shared &&
                (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET),
            "%s invalid blend capability", f.name.c_str()
        ))
      continue;
    const UINT height = UINT(cases.size());
    for (UINT phase = 0; phase < phase_count; phase++) {
      std::vector<Source> source;
      std::vector<uint8_t> destination;
      inputs(f, phase, source, destination);
      D3D11_BUFFER_DESC bd{UINT(source.size() * sizeof(Source)),  D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0,
                           D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(Source)};
      D3D11_SUBRESOURCE_DATA data{source.data()};
      ComPtr<ID3D11Buffer> input;
      ComPtr<ID3D11ShaderResourceView> srv;
      CHECK(device->CreateBuffer(&bd, &data, &input));
      CHECK(device->CreateShaderResourceView(input.Get(), nullptr, &srv));
      std::vector<uint8_t> initial(destination.size() * height);
      for (UINT row = 0; row < height; row++)
        memcpy(initial.data() + row * destination.size(), destination.data(), destination.size());
      D3D11_TEXTURE2D_DESC td{width, height, 1, 1, f.id, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
      data = {initial.data(), width * f.bytes};
      ComPtr<ID3D11Texture2D> target, staging;
      ComPtr<ID3D11RenderTargetView> rtv;
      CHECK(device->CreateTexture2D(&td, &data, &target));
      CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
      td.Usage = D3D11_USAGE_STAGING;
      td.BindFlags = 0;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&td, nullptr, &staging));
      for (bool listed : {false, true}) {
        step(
            "%s phase %u %s: %u blend equations/masks", f.name.c_str(), phase, listed ? "deferred" : "immediate", height
        );
        auto context = listed ? deferred.Get() : immediate.Get();
        context->ClearState();
        context->UpdateSubresource(target.Get(), 0, nullptr, initial.data(), width * f.bytes, 0);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, srv.GetAddressOf());
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        context->RSSetState(raster.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto constant = constants(phase);
        for (UINT row = 0; row < height; row++) {
          D3D11_VIEWPORT viewport{0, float(row), float(width), 1, 0, 1};
          context->RSSetViewports(1, &viewport);
          context->OMSetBlendState(states[row].Get(), constant.data(), ~0u);
          context->Draw(3, 0);
        }
        context->ClearState();
        context->CopyResource(staging.Get(), target.Get());
        if (listed) {
          ComPtr<ID3D11CommandList> list;
          CHECK(deferred->FinishCommandList(FALSE, &list));
          immediate->ExecuteCommandList(list.Get(), FALSE);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        bool same = true;
        for (UINT row = 0; same && row < height; row++)
          for (UINT x = 0; x < width; x++)
            if (!matches(
                    f, cases[row], phase, source[x], destination.data() + x * f.bytes,
                    static_cast<const uint8_t *>(mapped.pData) + row * mapped.RowPitch + x * f.bytes
                )) {
              const auto &e = cases[row];
              expect(
                  false, "row %u pixel %u: ops %u,%u factors %u,%u alpha %u,%u mask %#x differs", row, x, e.operation,
                  e.alpha_operation, e.source, e.destination, e.alpha_source, e.alpha_destination, e.write_mask
              );
              same = false;
              break;
            }
        immediate->Unmap(staging.Get(), 0);
        tested++;
        if (!same)
          return verdict();
      }
    }
  }
  expect(tested != 0, "no blendable format ran");
  return verdict();
}
