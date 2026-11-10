// contract: D3D11.3 3.4.6: "Cull modes do not apply to points." 15.13: "Only triangles reaching the rasterizer
// are affected by fill mode; line and point primitives reaching the rasterizer are not affected at all."
// ../raster.hpp gives the clipping and coverage contracts shared with Direct3D 10. all pixels are read back
// through the D3D10 interfaces, at feature levels 10.0 and 10.1; shader profiles are 4.0.
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include "../raster.hpp"
#include <cstddef>

int main() {
  if (!raster::reference_contract())
    return 1;
  step("compile Direct3D 10 raster shaders before device creation");
  ComPtr<ID3DBlob> codes[raster::ModeCount], gs_code[raster::ModeCount][raster::Triangle];
  for (unsigned mode = raster::None; mode < raster::ModeCount; mode++) {
    codes[mode] = compile(raster::hlsl, "vs", "vs_4_0", {"DISTANCES=" + std::to_string(mode)});
    if (!codes[mode])
      return 1;
    const char *entries[] = {"gs_point", "gs_line", "gs_triangle"};
    for (unsigned shape = 0; shape < std::size(entries); shape++) {
      gs_code[mode][shape] = compile(raster::hlsl, entries[shape], "gs_4_0", {"DISTANCES=" + std::to_string(mode)});
      if (!gs_code[mode][shape])
        return 1;
    }
  }
  auto ps_code = compile(raster::hlsl, "ps", "ps_4_0", {"DISTANCES=0"});
  if (!ps_code)
    return 1;
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  auto all = raster::cases(false);
  auto boundaries = raster::viewport_boundaries(D3D10_VIEWPORT_BOUNDS_MIN, D3D10_VIEWPORT_BOUNDS_MAX);
  all.insert(all.end(), boundaries.begin(), boundaries.end());
  auto resets = raster::reset_cases();
  all.insert(all.end(), resets.begin(), resets.end());
  const auto capacity = std::max_element(all.begin(), all.end(), [](const auto &a, const auto &b) {
                          return a.vertices.size() < b.vertices.size();
                        })->vertices.size();
  for (auto level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, level, D3D10_1_SDK_VERSION, &device));
    ComPtr<ID3D10VertexShader> vs[raster::ModeCount];
    ComPtr<ID3D10GeometryShader> gs[raster::ModeCount][raster::Triangle];
    ComPtr<ID3D10InputLayout> layouts[raster::ModeCount];
    D3D10_INPUT_ELEMENT_DESC elements[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                                            offsetof(raster::Vertex, position), D3D10_INPUT_PER_VERTEX_DATA, 0},
                                           {"DISTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                                            offsetof(raster::Vertex, distance), D3D10_INPUT_PER_VERTEX_DATA, 0},
                                           {"DISTANCE", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                                            offsetof(raster::Vertex, distance) + sizeof(raster::Color),
                                            D3D10_INPUT_PER_VERTEX_DATA, 0}};
    for (unsigned mode = raster::None; mode < raster::ModeCount; mode++) {
      for (unsigned shape = 0; shape < raster::Triangle; shape++)
        CHECK(device->CreateGeometryShader(gs_code[mode][shape]->GetBufferPointer(),
                                           gs_code[mode][shape]->GetBufferSize(), &gs[mode][shape]));
      CHECK(device->CreateVertexShader(codes[mode]->GetBufferPointer(), codes[mode]->GetBufferSize(), &vs[mode]));
      CHECK(device->CreateInputLayout(elements, std::size(elements), codes[mode]->GetBufferPointer(),
                                      codes[mode]->GetBufferSize(), &layouts[mode]));
    }
    ComPtr<ID3D10PixelShader> ps;
    CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), &ps));
    D3D10_BLEND_DESC blend_desc{};
    blend_desc.BlendEnable[0] = TRUE;
    blend_desc.SrcBlend = blend_desc.DestBlend = blend_desc.SrcBlendAlpha = blend_desc.DestBlendAlpha = D3D10_BLEND_ONE;
    blend_desc.BlendOp = blend_desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
    blend_desc.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D10BlendState> additive;
    CHECK(device->CreateBlendState(&blend_desc, &additive));
    D3D10_BUFFER_DESC bd{UINT(sizeof(raster::Vertex) * capacity), D3D10_USAGE_DEFAULT, D3D10_BIND_VERTEX_BUFFER};
    ComPtr<ID3D10Buffer> vb;
    CHECK(device->CreateBuffer(&bd, nullptr, &vb));
    D3D10_TEXTURE2D_DESC td{raster::width,       raster::height,          1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0},
                            D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET};
    ComPtr<ID3D10Texture2D> target, staging;
    ComPtr<ID3D10RenderTargetView> rtv;
    CHECK(device->CreateTexture2D(&td, nullptr, &target));
    CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    td.Usage = D3D10_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&td, nullptr, &staging));
    for (size_t i = 0; i < all.size(); i++) {
      const auto &c = all[i];
      // D3D10 viewport origins and extents are integers; fractional-origin cases are exercised by 11 and 12.
      if (c.viewport.x != std::floor(c.viewport.x) || c.viewport.y != std::floor(c.viewport.y) ||
          c.viewport.w != std::floor(c.viewport.w) || c.viewport.h != std::floor(c.viewport.h))
        continue;
      // zero-area faces became defined as back-facing in D3D10.1 (D3D11.3 chapter 15 change list).
      if (level == D3D10_FEATURE_LEVEL_10_0 && c.wire && c.vertices.size() == raster::Triangle) {
        auto a = raster::project(c.vertices[0], c.viewport), b = raster::project(c.vertices[1], c.viewport),
             d = raster::project(c.vertices[2], c.viewport);
        if (raster::edge(a, b, d.x, d.y) == 0)
          continue;
      }
      step("feature level %#x case %zu: %s", level, i, c.name.c_str());
      D3D10_RASTERIZER_DESC rd{c.wire ? D3D10_FILL_WIREFRAME : D3D10_FILL_SOLID,
                               c.cull == raster::NoCull      ? D3D10_CULL_NONE
                               : c.cull == raster::CullFront ? D3D10_CULL_FRONT
                                                             : D3D10_CULL_BACK};
      rd.FrontCounterClockwise = c.ccw;
      rd.DepthClipEnable = c.depth_clip;
      rd.ScissorEnable = c.scissor;
      rd.MultisampleEnable = c.multisample;
      rd.AntialiasedLineEnable = c.aa;
      rd.SlopeScaledDepthBias = c.bias_scale;
      rd.DepthBiasClamp = c.bias_clamp;
      ComPtr<ID3D10RasterizerState> state;
      CHECK(device->CreateRasterizerState(&rd, &state));
      D3D10_RASTERIZER_DESC got{};
      state->GetDesc(&got);
      expect(!memcmp(&got, &rd, sizeof(rd)), "GetDesc changed raster fields");
      if (c.clear_state) {
        D3D10_RASTERIZER_DESC dirty{D3D10_FILL_WIREFRAME, D3D10_CULL_FRONT};
        dirty.ScissorEnable = TRUE;
        ComPtr<ID3D10RasterizerState> previous;
        CHECK(device->CreateRasterizerState(&dirty, &previous));
        device->RSSetState(previous.Get());
        D3D10_VIEWPORT old_viewport{0, 0, raster::width, raster::height, 0, 1};
        D3D10_RECT old_scissor{0, 0, 0, 0};
        device->RSSetViewports(1, &old_viewport);
        device->RSSetScissorRects(1, &old_scissor);
        device->ClearState();
      } else {
        device->RSSetState(c.default_state ? nullptr : state.Get());
        device->RSSetState(c.default_state ? nullptr : state.Get());
      }
      ComPtr<ID3D10RasterizerState> bound;
      device->RSGetState(&bound);
      expect(bound.Get() == (c.default_state ? nullptr : state.Get()), "RSGetState did not return bound object");
      D3D10_VIEWPORT vp{INT(c.viewport.x),  INT(c.viewport.y), UINT(c.viewport.w),
                        UINT(c.viewport.h), c.viewport.low,    c.viewport.high};
      D3D10_RECT rect{c.rect.left, c.rect.top, c.rect.right, c.rect.bottom};
      if (!c.clear_state || c.viewport.w != 0)
        device->RSSetViewports(1, &vp);
      if (!c.clear_state)
        device->RSSetScissorRects(1, &rect);
      std::vector<raster::Vertex> vertices(capacity);
      std::copy(c.vertices.begin(), c.vertices.end(), vertices.begin());
      device->UpdateSubresource(vb.Get(), 0, nullptr, vertices.data(), 0, 0);
      const UINT stride = sizeof(raster::Vertex), offset = 0;
      device->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
      device->IASetInputLayout(layouts[c.mode].Get());
      device->IASetPrimitiveTopology(
          c.shape == raster::Point ? D3D10_PRIMITIVE_TOPOLOGY_POINTLIST
          : c.shape == raster::Line
              ? (c.strip ? D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D10_PRIMITIVE_TOPOLOGY_LINELIST)
              : (c.strip ? D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST));
      device->VSSetShader(vs[c.mode].Get());
      device->GSSetShader(c.geometry ? gs[c.mode][c.shape - 1].Get() : nullptr);
      device->PSSetShader(ps.Get());
      device->OMSetBlendState(c.additive ? additive.Get() : nullptr, nullptr, ~0u);
      device->OMSetRenderTargets(0, nullptr, nullptr);
      device->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      const float clear[raster::lanes] = {};
      device->ClearRenderTargetView(rtv.Get(), clear);
      device->DrawInstanced(c.vertices.size(), c.instances, 0, 0);
      device->CopyResource(staging.Get(), target.Get());
      D3D10_MAPPED_TEXTURE2D mapped;
      CHECK(staging->Map(0, D3D10_MAP_READ, 0, &mapped));
      raster::compare(c, static_cast<const char *>(mapped.pData), mapped.RowPitch, true, true);
      staging->Unmap(0);
    }
  }
  return verdict();
}
