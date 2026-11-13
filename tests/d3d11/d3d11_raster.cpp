// contract: D3D11.3 15.13: "Only triangles reaching the rasterizer are affected by fill mode; line and point
// primitives reaching the rasterizer are not affected at all." 15.12: "For lines and points, IsFrontFace has the
// value 0xFFFFFFFF." clipping, edge and viewport contracts and the independent reference are in ../raster.hpp.
// every pixel is compared through immediate and deferred contexts; all shaders compile before device creation.
#include "d3d11_test.hpp"
#include "../raster.hpp"
#include "../raster_conservative.hpp"
#include <d3d11_3.h>
#include <cstddef>

int main() {
  if (!raster::reference_contract() || !raster_conservative::reference_contract())
    return 1;
  step("compile raster vertex and pixel shaders");
  ComPtr<ID3DBlob> codes[raster::ModeCount], gs_code[raster::ModeCount][raster::Triangle];
  for (unsigned mode = raster::None; mode < raster::ModeCount; mode++) {
    codes[mode] = compile(raster::hlsl, "vs", "vs", {"DISTANCES=" + std::to_string(mode)});
    if (!codes[mode])
      return 1;
    const char *entries[] = {"gs_point", "gs_line", "gs_triangle"};
    for (unsigned shape = 0; shape < std::size(entries); shape++) {
      gs_code[mode][shape] = compile(raster::hlsl, entries[shape], "gs", {"DISTANCES=" + std::to_string(mode)});
      if (!gs_code[mode][shape])
        return 1;
    }
  }
  auto ps_code = compile(raster::hlsl, "ps", "ps", {"DISTANCES=0"});
  auto inner_code = compile(raster_conservative::hlsl, "ps_inner", "ps");
  if (!ps_code || !inner_code)
    return 1;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, UINT(std::size(levels)),
                          D3D11_SDK_VERSION, &device, nullptr, &immediate));
  ComPtr<ID3D11Device3> device3;
  CHECK(device.As(&device3));
  D3D11_FEATURE_DATA_D3D11_OPTIONS2 options{};
  CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options, sizeof(options)));
  unsigned tier = options.ConservativeRasterizationTier;
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs[raster::ModeCount];
  ComPtr<ID3D11GeometryShader> gs[raster::ModeCount][raster::Triangle];
  ComPtr<ID3D11InputLayout> layouts[raster::ModeCount];
  D3D11_INPUT_ELEMENT_DESC elements[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                                          offsetof(raster::Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
                                         {"DISTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                                          offsetof(raster::Vertex, distance), D3D11_INPUT_PER_VERTEX_DATA, 0},
                                         {"DISTANCE", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                                          offsetof(raster::Vertex, distance) + sizeof(raster::Color),
                                          D3D11_INPUT_PER_VERTEX_DATA, 0}};
  for (unsigned mode = raster::None; mode < raster::ModeCount; mode++) {
    for (unsigned shape = 0; shape < raster::Triangle; shape++)
      CHECK(device->CreateGeometryShader(gs_code[mode][shape]->GetBufferPointer(),
                                         gs_code[mode][shape]->GetBufferSize(), nullptr, &gs[mode][shape]));
    CHECK(
        device->CreateVertexShader(codes[mode]->GetBufferPointer(), codes[mode]->GetBufferSize(), nullptr, &vs[mode]));
    CHECK(device->CreateInputLayout(elements, std::size(elements), codes[mode]->GetBufferPointer(),
                                    codes[mode]->GetBufferSize(), &layouts[mode]));
  }
  ComPtr<ID3D11PixelShader> ps, inner;
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  HRESULT inner_hr =
      device->CreatePixelShader(inner_code->GetBufferPointer(), inner_code->GetBufferSize(), nullptr, &inner);
  expect(tier >= D3D11_CONSERVATIVE_RASTERIZATION_TIER_3 ? inner_hr == S_OK && inner : FAILED(inner_hr) && !inner,
         "InnerCoverage creation at tier%u returned %#lx", tier, inner_hr);
  auto all = raster::cases();
  auto boundaries = raster::viewport_boundaries(D3D11_VIEWPORT_BOUNDS_MIN, D3D11_VIEWPORT_BOUNDS_MAX);
  all.insert(all.end(), boundaries.begin(), boundaries.end());
  auto resets = raster::reset_cases();
  all.insert(all.end(), resets.begin(), resets.end());
  auto conservative = raster_conservative::cases(tier);
  all.insert(all.end(), conservative.begin(), conservative.end());
  const auto capacity = std::max_element(all.begin(), all.end(), [](const auto &a, const auto &b) {
                          return a.vertices.size() < b.vertices.size();
                        })->vertices.size();
  D3D11_BLEND_DESC blend_desc{};
  auto &blend = blend_desc.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.SrcBlend = blend.DestBlend = blend.SrcBlendAlpha = blend.DestBlendAlpha = D3D11_BLEND_ONE;
  blend.BlendOp = blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ComPtr<ID3D11BlendState> additive;
  CHECK(device->CreateBlendState(&blend_desc, &additive));
  D3D11_BUFFER_DESC vb_desc{UINT(sizeof(raster::Vertex) * capacity), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
  ComPtr<ID3D11Buffer> vb;
  CHECK(device->CreateBuffer(&vb_desc, nullptr, &vb));
  std::vector<UINT> indices(capacity);
  std::iota(indices.begin(), indices.end(), 0);
  D3D11_BUFFER_DESC index_desc{UINT(indices.size() * sizeof(UINT)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER};
  D3D11_SUBRESOURCE_DATA index_data{indices.data()};
  ComPtr<ID3D11Buffer> index_buffer, arguments;
  CHECK(device->CreateBuffer(&index_desc, &index_data, &index_buffer));
  // Draw[Indexed]InstancedIndirect reads its documented four/five UINT argument fields from byte offset zero.
  D3D11_BUFFER_DESC args_desc{5 * sizeof(UINT), D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS};
  CHECK(device->CreateBuffer(&args_desc, nullptr, &arguments));
  D3D11_TEXTURE2D_DESC desc{raster::width,       raster::height,          1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0},
                            D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
  for (auto context : {immediate.Get(), deferred.Get()})
    for (unsigned path = 0; path < 4; path++)
      for (size_t i = 0; i < all.size(); i++) {
        auto &c = all[i];
        step("%s path %u case %zu: %s", context == immediate.Get() ? "immediate" : "deferred", path, i, c.name.c_str());
        D3D11_RASTERIZER_DESC rd{c.wire ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID,
                                 c.cull == raster::NoCull      ? D3D11_CULL_NONE
                                 : c.cull == raster::CullFront ? D3D11_CULL_FRONT
                                                               : D3D11_CULL_BACK};
        rd.FrontCounterClockwise = c.ccw;
        rd.DepthClipEnable = c.depth_clip;
        rd.ScissorEnable = c.scissor;
        rd.MultisampleEnable = c.multisample;
        rd.AntialiasedLineEnable = c.aa;
        rd.SlopeScaledDepthBias = c.bias_scale;
        rd.DepthBiasClamp = c.bias_clamp;
        ComPtr<ID3D11RasterizerState> state;
        if (c.conservative) {
          D3D11_RASTERIZER_DESC2 desc{};
          memcpy(&desc, &rd, sizeof(rd));
          desc.ConservativeRaster = D3D11_CONSERVATIVE_RASTERIZATION_MODE_ON;
          ComPtr<ID3D11RasterizerState2> state2;
          CHECK(device3->CreateRasterizerState2(&desc, &state2));
          CHECK(state2.As(&state));
        } else {
          CHECK(device->CreateRasterizerState(&rd, &state));
        }
        D3D11_RASTERIZER_DESC got{};
        state->GetDesc(&got);
        expect(!memcmp(&got, &rd, sizeof(rd)), "GetDesc changed raster fields");
        if (c.clear_state) {
          D3D11_RASTERIZER_DESC dirty{D3D11_FILL_WIREFRAME, D3D11_CULL_FRONT};
          dirty.ScissorEnable = TRUE;
          ComPtr<ID3D11RasterizerState> previous;
          CHECK(device->CreateRasterizerState(&dirty, &previous));
          context->RSSetState(previous.Get());
          D3D11_VIEWPORT old_viewport{0, 0, raster::width, raster::height, 0, 1};
          D3D11_RECT old_scissor{0, 0, 0, 0};
          context->RSSetViewports(1, &old_viewport);
          context->RSSetScissorRects(1, &old_scissor);
          context->ClearState();
        } else {
          context->RSSetState(c.default_state ? nullptr : state.Get());
          context->RSSetState(c.default_state ? nullptr : state.Get());
        }
        ComPtr<ID3D11RasterizerState> bound;
        context->RSGetState(&bound);
        expect(bound.Get() == (c.default_state ? nullptr : state.Get()), "RSGetState did not return the bound object");
        D3D11_VIEWPORT vp{c.viewport.x, c.viewport.y, c.viewport.w, c.viewport.h, c.viewport.low, c.viewport.high};
        D3D11_RECT rect{c.rect.left, c.rect.top, c.rect.right, c.rect.bottom};
        if (!c.clear_state || c.viewport.w != 0)
          context->RSSetViewports(1, &vp);
        if (!c.clear_state)
          context->RSSetScissorRects(1, &rect);
        std::vector<raster::Vertex> vertices(capacity);
        std::copy(c.vertices.begin(), c.vertices.end(), vertices.begin());
        context->UpdateSubresource(vb.Get(), 0, nullptr, vertices.data(), 0, 0);
        const UINT stride = sizeof(raster::Vertex), offset = 0;
        context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
        context->IASetInputLayout(layouts[c.mode].Get());
        context->IASetIndexBuffer(index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        context->IASetPrimitiveTopology(
            c.shape == raster::Point ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
            : c.shape == raster::Line
                ? (c.strip ? D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D11_PRIMITIVE_TOPOLOGY_LINELIST)
                : (c.strip ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST));
        context->VSSetShader(vs[c.mode].Get(), nullptr, 0);
        context->GSSetShader(c.geometry ? gs[c.mode][c.shape - 1].Get() : nullptr, nullptr, 0);
        context->PSSetShader(c.inner ? inner.Get() : ps.Get(), nullptr, 0);
        context->OMSetBlendState(c.additive ? additive.Get() : nullptr, nullptr, ~0u);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        const float clear[raster::lanes] = {};
        context->ClearRenderTargetView(rtv.Get(), clear);
        if (path >= 2) {
          const UINT fields[] = {UINT(c.vertices.size()), c.instances, 0, 0, 0};
          context->UpdateSubresource(arguments.Get(), 0, nullptr, fields, 0, 0);
          if (path == 2)
            context->DrawInstancedIndirect(arguments.Get(), 0);
          else
            context->DrawIndexedInstancedIndirect(arguments.Get(), 0);
        } else if (path == 1)
          context->DrawIndexedInstanced(c.vertices.size(), c.instances, 0, 0, 0);
        else
          context->DrawInstanced(c.vertices.size(), c.instances, 0, 0);
        context->CopyResource(staging.Get(), target.Get());
        if (context == deferred.Get()) {
          ComPtr<ID3D11CommandList> commands;
          CHECK(deferred->FinishCommandList(FALSE, &commands));
          immediate->ExecuteCommandList(commands.Get(), FALSE);
        }
        D3D11_MAPPED_SUBRESOURCE mapped;
        CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        if (c.conservative)
          raster_conservative::compare(c, tier, static_cast<const char *>(mapped.pData), mapped.RowPitch);
        else
          raster::compare(c, static_cast<const char *>(mapped.pData), mapped.RowPitch, false, true);
        immediate->Unmap(staging.Get(), 0);
      }
  return verdict();
}
