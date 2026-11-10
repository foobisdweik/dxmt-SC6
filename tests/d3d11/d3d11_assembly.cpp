// contract: see ../assembly.hpp, through D3D10.0/10.1 and D3D11 immediate/deferred contexts.
// D3D11.3 8.15: "The only place in the Pipeline where adjacency information is visible to the application is in the Geometry Shader."
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#include "d3d11_test.hpp"
#include "../assembly.hpp"
#include <d3d10_1.h>

using namespace assembly;

int
main() {
  ComPtr<ID3DBlob> codes[2][2][2];
  step("compile every shader before creating devices");
  for (UINT legacy = 0; legacy < 2; legacy++)
    for (bool cull : {false, true}) {
      codes[legacy][cull][0] = compile(hlsl, "vs", legacy ? "vs_4_0" : "vs_5_0", defines(cull));
      codes[legacy][cull][1] = compile(hlsl, "ps", legacy ? "ps_4_0" : "ps_5_0", defines(cull));
      if (!expect(codes[legacy][cull][0] && codes[legacy][cull][1], "HLSL did not compile"))
        return verdict();
    }
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create10 != nullptr, "no D3D10CreateDevice1"))
    return verdict();
  for (UINT api : {11u, 10u, 101u}) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate, deferred;
    ComPtr<ID3D10Device1> device10;
    if (api == 11) {
      const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
      CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                              &device, nullptr, &immediate));
      CHECK(device->CreateDeferredContext(0, &deferred));
    } else {
      CHECK(create10(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
                     api == 10 ? D3D10_FEATURE_LEVEL_10_0 : D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &device10));
      CHECK(device10.As(&device));
      device->GetImmediateContext(&immediate);
    }
    ComPtr<ID3D11VertexShader> vertex[2];
    ComPtr<ID3D11GeometryShader> stream_out[2];
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D10VertexShader> vertex10[2];
    ComPtr<ID3D10GeometryShader> stream_out10[2];
    ComPtr<ID3D10PixelShader> pixel10;
    const D3D11_SO_DECLARATION_ENTRY entries[] = {{0, "VALUE", 0, 0, 1, 0}};
    const UINT stride = sizeof(uint32_t), sentinel = 0xa5a5a5a5u;
    for (bool cull : {false, true}) {
      auto vs = codes[api != 11][cull][0].Get(), ps = codes[api != 11][cull][1].Get();
      if (device10) {
        const D3D10_SO_DECLARATION_ENTRY entry{"VALUE", 0, 0, 1, 0};
        CHECK(device10->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), &vertex10[cull]));
        CHECK(device10->CreateGeometryShaderWithStreamOutput(vs->GetBufferPointer(), vs->GetBufferSize(), &entry,
                                                              1, stride, &stream_out10[cull]));
        if (!cull) CHECK(device10->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), &pixel10));
      } else {
        CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex[cull]));
        CHECK(device->CreateGeometryShaderWithStreamOutput(vs->GetBufferPointer(), vs->GetBufferSize(), entries,
            1, &stride, 1, 0, nullptr, &stream_out[cull]));
        if (!cull) CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_shader));
      }
    }
    // both passes with stream output append
    std::vector<uint32_t> streamed;
    for (int pass = 0; pass < 2; pass++)
      stream(streamed);
    auto stream_buffer = buffer(device.Get(), stride * (streamed.size() + 1), D3D11_BIND_STREAM_OUTPUT, sentinel);
    D3D11_BUFFER_DESC constants_desc{sizeof(Constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    ComPtr<ID3D11Buffer> constant_buffer;
    CHECK(device->CreateBuffer(&constants_desc, nullptr, &constant_buffer));

    D3D11_TEXTURE2D_DESC texture_desc{width, height, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> texture, texture_read;
    ComPtr<ID3D11RenderTargetView> rtv;
    CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture));
    CHECK(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv));
    texture_desc.Usage = D3D11_USAGE_STAGING;
    texture_desc.BindFlags = 0;
    texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture_read));
    D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    raster_desc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    CHECK(device->CreateRasterizerState(&raster_desc, &raster));
    D3D11_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
    const float clear[4] = {};
    ComPtr<ID3D10Buffer> constant10, stream_buffer10;
    ComPtr<ID3D10RenderTargetView> rtv10;
    ComPtr<ID3D10RasterizerState> raster10;
    if (device10) {
      CHECK(constant_buffer.As(&constant10));
      CHECK(stream_buffer.As(&stream_buffer10));
      CHECK(rtv.As(&rtv10));
      CHECK(raster.As(&raster10));
    }
    for (UINT recorded = 0; recorded < (api == 11 ? 2u : 1u); recorded++) {
      step("D3D%u recorded=%u: all topology pixels and stream-output bytes", api, recorded);
      auto context = recorded ? deferred.Get() : immediate.Get();
      context->UpdateSubresource(stream_buffer.Get(), 0, nullptr, std::vector<UINT>(streamed.size() + 1, sentinel).data(), 0, 0);
      if (device10) {
        const D3D10_VIEWPORT viewport10{0, 0, width, height, 0, 1};
        device10->ClearRenderTargetView(rtv10.Get(), clear);
        device10->RSSetState(raster10.Get());
        device10->RSSetViewports(1, &viewport10);
        device10->OMSetRenderTargets(1, rtv10.GetAddressOf(), nullptr);
        device10->VSSetConstantBuffers(0, 1, constant10.GetAddressOf());
        device10->PSSetShader(pixel10.Get());
      } else {
        context->ClearRenderTargetView(rtv.Get(), clear);
        context->RSSetState(raster.Get());
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        context->VSSetConstantBuffers(0, 1, constant_buffer.GetAddressOf());
        context->PSSetShader(pixel_shader.Get(), nullptr, 0);
      }
      auto draw = [&](D3D_PRIMITIVE_TOPOLOGY topology, UINT count) {
        if (device10) { device10->IASetPrimitiveTopology(topology); device10->Draw(count, 0); }
        else { context->IASetPrimitiveTopology(topology); context->Draw(count, 0); }
      };
      for (UINT pass = 0; pass < passes; pass++) {
        bool cull = pass & 1, with_stream = pass & 2;
        if (device10) {
          device10->VSSetShader(vertex10[cull].Get());
          device10->GSSetShader(with_stream ? stream_out10[cull].Get() : nullptr);
        } else {
          context->VSSetShader(vertex[cull].Get(), nullptr, 0);
          context->GSSetShader(with_stream ? stream_out[cull].Get() : nullptr, nullptr, 0);
        }
        ID3D11Buffer *target = with_stream ? stream_buffer.Get() : nullptr;
        // the first pass that streams starts the buffer, the next appends
        UINT offset = pass == 2 ? 0 : ~0u;
        if (device10) {
          ID3D10Buffer *target10 = with_stream ? stream_buffer10.Get() : nullptr;
          device10->SOSetTargets(1, &target10, &offset);
        } else context->SOSetTargets(1, &target, &offset);
        for (UINT t = 0; t < case_count; t++) {
          auto values = constants(cases[t], pass_rows * pass + rows * t);
          context->UpdateSubresource(constant_buffer.Get(), 0, nullptr, &values, 0, 0);
          draw(cases[t].topology, cases[t].vertices);
        }
        auto values = constants(turned, pass_rows * pass + turn_row);
        context->UpdateSubresource(constant_buffer.Get(), 0, nullptr, &values, 0, 0);
        draw(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, turn_vertices);
        draw(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ, turned_vertices);
      }
      if (device10) device10->SOSetTargets(0, nullptr, nullptr);
      else context->SOSetTargets(0, nullptr, nullptr);
      if (recorded) {
        ComPtr<ID3D11CommandList> list;
        CHECK(context->FinishCommandList(FALSE, &list));
        immediate->ExecuteCommandList(list.Get(), TRUE);
      }
      auto compare = [](const char *what, UINT a, UINT b, uint64_t value, uint64_t want) {
        expect(value == want, "%s %u,%u: %llu, want %llu", what, a, b, (unsigned long long)value, (unsigned long long)want);
      };
      immediate->CopyResource(texture_read.Get(), texture.Get());
      D3D11_MAPPED_SUBRESOURCE mapped;
      CHECK(immediate->Map(texture_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
      for (UINT y = 0; y < height; y++) {
        UINT pass = y / pass_rows, t = y % pass_rows / rows;
        bool turn = t == case_count;
        auto assembled = assemble(cases[turn ? 0 : t], pass & 1);
        for (UINT x = 0; x < width; x++)
          compare(
              "pixel", x, y, reinterpret_cast<const uint32_t *>((const uint8_t *)mapped.pData + mapped.RowPitch * y)[x],
              turn ? turned_pixel(pass & 1, x, y % rows) : pixel(cases[t], assembled, x, y % rows)
          );
      }
      immediate->Unmap(texture_read.Get(), 0);
      auto got = read(device.Get(), immediate.Get(), stream_buffer.Get());
      if (!expect(got.size() == streamed.size() + 1, "wrong stream-output readback size"))
        return verdict();
      for (size_t i = 0; i < streamed.size(); i++)
        compare("streamed vertex", (UINT)i, 0, got[i], streamed[i]);
      compare("past the stream", (UINT)streamed.size(), 0, got[streamed.size()], sentinel);
    }
  }
  return verdict();
}
