// contract: D3D10 bindings roundtrip every legal slot without owning public
// references; getters AddRef. "The method will not hold a reference to the
// interfaces passed in." (ID3D10Device::VSSetConstantBuffers, Microsoft Learn);
// VSGetConstantBuffers: "Any returned interfaces will have their reference
// count incremented by one."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-vssetconstantbuffers
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-vsgetconstantbuffers
// ClearState: "The primitive topology will be set to UNDEFINED." All resource
// slots and state objects become NULL.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-clearstate
// RSGetViewports: "then unused members of the array will contain 0." The same
// rule applies to RSGetScissorRects.
// IAGetVertexBuffers: "A maximum of 16 or 32 input slots"; the D3D10.1 count comes from its header.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-iagetvertexbuffers
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-rsgetviewports
// D3D11.3 15.6/17.1 define viewport and blend state; counts and defaults come
// from the D3D10 headers.
// 17.13: "the offending view(s) on the input side (SRVs) are immediately unbound." D3D10 DSVs are outputs even
// when depth writes are disabled. mip/array intervals decide overlap; both binding orders and all stages matter.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include <algorithm>
#include <iterator>
#include <limits>

static const char hlsl[] = R"hlsl(
float4 vs(float4 position : POSITION) : SV_Position { return position; }
struct V { float4 position : SV_Position; };
[maxvertexcount(1)] void gs(point V vertices[1], inout PointStream<V> stream) {
  stream.Append(vertices[0]);
}
float4 ps() : SV_Target { return 1; }
)hlsl";

static ULONG
references(IUnknown *object) {
  object->AddRef();
  return object->Release();
}

int
main() {
  auto vs = compile(hlsl, "vs", "vs_4_0"), gs = compile(hlsl, "gs", "gs_4_0"), ps = compile(hlsl, "ps", "ps_4_0");
  if (!expect(vs && gs && ps, "HLSL did not compile"))
    return verdict();
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")
  );
  if (!expect(create != nullptr, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  struct Stage {
    const char *name;
    decltype(&ID3D10Device::VSSetConstantBuffers) set_cb;
    decltype(&ID3D10Device::VSGetConstantBuffers) get_cb;
    decltype(&ID3D10Device::VSSetShaderResources) set_srv;
    decltype(&ID3D10Device::VSGetShaderResources) get_srv;
    decltype(&ID3D10Device::VSSetSamplers) set_sampler;
    decltype(&ID3D10Device::VSGetSamplers) get_sampler;
  };
  const Stage stages[] = {
      {"VS", &ID3D10Device::VSSetConstantBuffers, &ID3D10Device::VSGetConstantBuffers,
       &ID3D10Device::VSSetShaderResources, &ID3D10Device::VSGetShaderResources, &ID3D10Device::VSSetSamplers,
       &ID3D10Device::VSGetSamplers},
      {"GS", &ID3D10Device::GSSetConstantBuffers, &ID3D10Device::GSGetConstantBuffers,
       &ID3D10Device::GSSetShaderResources, &ID3D10Device::GSGetShaderResources, &ID3D10Device::GSSetSamplers,
       &ID3D10Device::GSGetSamplers},
      {"PS", &ID3D10Device::PSSetConstantBuffers, &ID3D10Device::PSGetConstantBuffers,
       &ID3D10Device::PSSetShaderResources, &ID3D10Device::PSGetShaderResources, &ID3D10Device::PSSetSamplers,
       &ID3D10Device::PSGetSamplers}
  };
  for (auto level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, level, D3D10_1_SDK_VERSION, &device));
    D3D10_BUFFER_DESC cb_desc{
        D3D10_COMMONSHADER_CONSTANT_BUFFER_COMPONENTS * sizeof(float), D3D10_USAGE_DEFAULT, D3D10_BIND_CONSTANT_BUFFER
    };
    ComPtr<ID3D10Buffer> cb, vertex;
    CHECK(device->CreateBuffer(&cb_desc, nullptr, &cb));
    D3D10_BUFFER_DESC vertex_desc{
        sizeof(float) * D3D10_STANDARD_VERTEX_ELEMENT_COUNT, D3D10_USAGE_DEFAULT,
        D3D10_BIND_VERTEX_BUFFER | D3D10_BIND_INDEX_BUFFER
    };
    CHECK(device->CreateBuffer(&vertex_desc, nullptr, &vertex));
    D3D10_TEXTURE2D_DESC texture_desc{
        1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_SHADER_RESOURCE
    };
    ComPtr<ID3D10Texture2D> texture;
    ComPtr<ID3D10ShaderResourceView> srv;
    CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture));
    CHECK(device->CreateShaderResourceView(texture.Get(), nullptr, &srv));
    D3D10_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D10_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D10_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D10_FLOAT32_MAX;
    ComPtr<ID3D10SamplerState> sampler;
    CHECK(device->CreateSamplerState(&sampler_desc, &sampler));

    auto slots = [&](const char *stage, auto object, UINT limit, auto set, auto get) {
      using Pointer = decltype(object);
      std::vector<Pointer> bound(limit), got(limit);
      const ULONG before = references(object);
      for (UINT start = 0; start < limit; start++) {
        for (UINT count : {0u, 1u, limit - start}) {
          step("level %#x %s slots %u..%u of %u", level, stage, start, start + count, limit);
          device->ClearState();
          std::fill(bound.begin(), bound.end(), object);
          set(start, count, bound.data());
          expect(references(object) == before, "setter owns a public reference");
          get(0, limit, got.data());
          expect(references(object) == before + count, "getter did not AddRef every returned interface");
          for (UINT slot = 0; slot < limit; slot++) {
            expect(got[slot] == (slot >= start && slot < start + count ? object : nullptr), "binding slot %u", slot);
            if (got[slot])
              got[slot]->Release();
          }
          expect(references(object) == before, "getter leaked a reference");
          set(start, count, nullptr);
          get(0, limit, got.data());
          for (UINT slot = 0; slot < limit; slot++) {
            expect(!got[slot], "null array did not unbind slot %u", slot);
            if (got[slot])
              got[slot]->Release();
          }
        }
      }
      std::fill(bound.begin(), bound.end(), object);
      for (UINT slot = 0; slot < limit; slot += 2)
        bound[slot] = nullptr;
      set(0, limit, bound.data());
      get(0, limit, got.data());
      for (UINT slot = 0; slot < limit; slot++) {
        expect(got[slot] == bound[slot], "null element at slot %u", slot);
        if (got[slot])
          expect(got[slot]->Release() >= before, "getter did not AddRef slot %u", slot);
      }
      device->ClearState();
    };
    for (auto stage : stages) {
      step("level %#x %s arrays", level, stage.name);
      slots(
          stage.name, cb.Get(), D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT,
          [&](UINT start, UINT count, auto values) { (device.Get()->*stage.set_cb)(start, count, values); },
          [&](UINT start, UINT count, auto values) { (device.Get()->*stage.get_cb)(start, count, values); }
      );
      slots(
          stage.name, srv.Get(), D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,
          [&](UINT start, UINT count, auto values) { (device.Get()->*stage.set_srv)(start, count, values); },
          [&](UINT start, UINT count, auto values) { (device.Get()->*stage.get_srv)(start, count, values); }
      );
      slots(
          stage.name, sampler.Get(), D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT,
          [&](UINT start, UINT count, auto values) { (device.Get()->*stage.set_sampler)(start, count, values); },
          [&](UINT start, UINT count, auto values) { (device.Get()->*stage.get_sampler)(start, count, values); }
      );
    }

    ComPtr<ID3D10VertexShader> vertex_shader;
    ComPtr<ID3D10GeometryShader> geometry_shader;
    ComPtr<ID3D10PixelShader> pixel_shader;
    ComPtr<ID3D10InputLayout> layout;
    CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), &vertex_shader));
    CHECK(device->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), &geometry_shader));
    CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), &pixel_shader));
    D3D10_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA,
                                     0};
    CHECK(device->CreateInputLayout(&element, 1, vs->GetBufferPointer(), vs->GetBufferSize(), &layout));
    ComPtr<ID3D10BlendState> blend;
    D3D10_BLEND_DESC blend_desc{};
    blend_desc.SrcBlend = blend_desc.SrcBlendAlpha = D3D10_BLEND_ONE;
    blend_desc.DestBlend = blend_desc.DestBlendAlpha = D3D10_BLEND_ZERO;
    blend_desc.BlendOp = blend_desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
    CHECK(device->CreateBlendState(&blend_desc, &blend));
    ComPtr<ID3D10DepthStencilState> depth;
    D3D10_DEPTH_STENCIL_DESC depth_desc{};
    depth_desc.DepthFunc = D3D10_COMPARISON_ALWAYS;
    depth_desc.FrontFace = depth_desc.BackFace = {
        D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_COMPARISON_ALWAYS
    };
    CHECK(device->CreateDepthStencilState(&depth_desc, &depth));
    ComPtr<ID3D10RasterizerState> raster;
    D3D10_RASTERIZER_DESC raster_desc{D3D10_FILL_SOLID, D3D10_CULL_NONE};
    CHECK(device->CreateRasterizerState(&raster_desc, &raster));
    ComPtr<ID3D10Predicate> predicate;
    D3D10_QUERY_DESC predicate_desc{D3D10_QUERY_OCCLUSION_PREDICATE};
    CHECK(device->CreatePredicate(&predicate_desc, &predicate));
    predicate->Begin();
    predicate->End();
    texture_desc.ArraySize = D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT;
    texture_desc.BindFlags = D3D10_BIND_RENDER_TARGET;
    ComPtr<ID3D10Texture2D> target, depth_target;
    CHECK(device->CreateTexture2D(&texture_desc, nullptr, &target));
    ComPtr<ID3D10RenderTargetView> targets[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D10RenderTargetView *target_ptrs[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT];
    for (UINT i = 0; i < std::size(targets); i++) {
      D3D10_RENDER_TARGET_VIEW_DESC desc{texture_desc.Format, D3D10_RTV_DIMENSION_TEXTURE2DARRAY};
      desc.Texture2DArray = {0, i, 1};
      CHECK(device->CreateRenderTargetView(target.Get(), &desc, &targets[i]));
      target_ptrs[i] = targets[i].Get();
    }
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_D32_FLOAT;
    texture_desc.BindFlags = D3D10_BIND_DEPTH_STENCIL;
    ComPtr<ID3D10DepthStencilView> depth_view;
    CHECK(device->CreateTexture2D(&texture_desc, nullptr, &depth_target));
    CHECK(device->CreateDepthStencilView(depth_target.Get(), nullptr, &depth_view));
    std::vector<ComPtr<ID3D10Buffer>> streams(D3D10_SO_BUFFER_SLOT_COUNT);
    std::vector<ID3D10Buffer *> stream_ptrs(streams.size());
    std::vector<UINT> offsets(streams.size());
    for (UINT slot = 0; slot < streams.size(); slot++) {
      D3D10_BUFFER_DESC desc{UINT((streams.size() + 1) * sizeof(UINT)), D3D10_USAGE_DEFAULT, D3D10_BIND_STREAM_OUTPUT};
      CHECK(device->CreateBuffer(&desc, nullptr, &streams[slot]));
      stream_ptrs[slot] = streams[slot].Get();
      offsets[slot] = slot * sizeof(UINT);
    }
    const UINT vertex_slots = level == D3D10_FEATURE_LEVEL_10_1 ? D3D10_1_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
                                                                : D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
    std::vector<ID3D10Buffer *> vertices(vertex_slots, vertex.Get());
    std::vector<UINT> strides(vertex_slots), vertex_offsets(vertex_slots);
    for (UINT slot = 0; slot < vertex_slots; slot++) {
      strides[slot] = (slot + 1) * sizeof(UINT);
      vertex_offsets[slot] = slot * sizeof(UINT);
    }
    slots(
        "IA", vertex.Get(), vertex_slots,
        [&](UINT start, UINT count, auto values) {
          device->IASetVertexBuffers(start, count, values, strides.data(), vertex_offsets.data());
        },
        [&](UINT start, UINT count, auto values) { device->IAGetVertexBuffers(start, count, values, nullptr, nullptr); }
    );
    const D3D10_PRIMITIVE_TOPOLOGY topologies[] = {
        D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED,        D3D10_PRIMITIVE_TOPOLOGY_POINTLIST,
        D3D10_PRIMITIVE_TOPOLOGY_LINELIST,         D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP,
        D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST,     D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
        D3D10_PRIMITIVE_TOPOLOGY_LINELIST_ADJ,     D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ,
        D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ
    };
    for (auto topology : topologies) {
      device->IASetPrimitiveTopology(topology);
      D3D10_PRIMITIVE_TOPOLOGY got;
      device->IAGetPrimitiveTopology(&got);
      expect(got == topology, "topology %u became %u", topology, got);
    }

    const UINT max_viewports = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    const UINT max_capacity = max_viewports + 1;
    std::vector<D3D10_VIEWPORT> viewports(max_viewports);
    std::vector<D3D10_RECT> rects(max_viewports);
    for (UINT i = 0; i < max_viewports; i++) {
      viewports[i] = {INT(i), INT(i), i + 1, i + 2, 0.25f, 0.75f};
      rects[i] = {LONG(i), LONG(i + 1), LONG(i + 2), LONG(i + 3)};
    }
    viewports.front() = {D3D10_VIEWPORT_BOUNDS_MIN, D3D10_VIEWPORT_BOUNDS_MIN, 1, 1, 0, 1};
    viewports.back() = {D3D10_VIEWPORT_BOUNDS_MAX - 1, D3D10_VIEWPORT_BOUNDS_MAX - 1, 1, 1, 0.25f, 0.75f};
    for (UINT count = 0; count <= max_viewports; count++) {
      device->RSSetViewports(count, viewports.data());
      device->RSSetScissorRects(count, rects.data());
      for (UINT capacity = 0; capacity <= max_capacity; capacity++) {
        step("level %#x rasterizer count %u capacity %u", level, count, capacity);
        std::vector<D3D10_VIEWPORT> got(max_capacity + 1, viewports.back());
        std::vector<D3D10_RECT> got_rects(max_capacity + 1, rects.back());
        UINT n = capacity, r = capacity, query = 0;
        device->RSGetViewports(&query, nullptr);
        expect(query == count, "viewport count query %u", query);
        query = 0;
        device->RSGetScissorRects(&query, nullptr);
        expect(query == count, "scissor count query %u", query);
        device->RSGetViewports(&n, got.data());
        device->RSGetScissorRects(&r, got_rects.data());
        expect(n == std::min(count, capacity) && r == n, "returned rasterizer counts %u/%u", n, r);
        for (UINT i = 0; i <= max_capacity; i++) {
          D3D10_VIEWPORT wanted = i < capacity ? i < count ? viewports[i] : D3D10_VIEWPORT{} : viewports.back();
          D3D10_RECT wanted_rect = i < capacity ? i < count ? rects[i] : D3D10_RECT{} : rects.back();
          expect(!memcmp(&got[i], &wanted, sizeof(wanted)), "viewport %u fields or unused output", i);
          expect(!memcmp(&got_rects[i], &wanted_rect, sizeof(wanted_rect)), "scissor %u fields or unused output", i);
        }
      }
    }

    for (bool reset : {false, true}) {
      step("level %#x complete binding state reset %u", level, reset);
      device->VSSetShader(vertex_shader.Get());
      device->GSSetShader(geometry_shader.Get());
      device->PSSetShader(pixel_shader.Get());
      device->IASetInputLayout(layout.Get());
      device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_POINTLIST);
      device->IASetVertexBuffers(0, vertex_slots, vertices.data(), strides.data(), vertex_offsets.data());
      device->IASetIndexBuffer(vertex.Get(), DXGI_FORMAT_R32_UINT, sizeof(UINT));
      device->RSSetState(raster.Get());
      device->SOSetTargets(streams.size(), stream_ptrs.data(), offsets.data());
      device->SetPredication(predicate.Get(), TRUE);
      device->OMSetRenderTargets(std::size(target_ptrs), target_ptrs, depth_view.Get());
      const float factors[] = {0.125f, 0.25f, 0.5f, 1};
      const UINT mask = std::numeric_limits<UINT>::max() / 3, stencil_ref = std::numeric_limits<UINT8>::max();
      device->OMSetBlendState(blend.Get(), factors, mask);
      device->OMSetDepthStencilState(depth.Get(), stencil_ref);
      for (auto stage : stages) {
        std::vector<ID3D10Buffer *> constants(D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cb.Get());
        std::vector<ID3D10ShaderResourceView *> resources(D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srv.Get());
        std::vector<ID3D10SamplerState *> samplers(D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, sampler.Get());
        (device.Get()->*stage.set_cb)(0, constants.size(), constants.data());
        (device.Get()->*stage.set_srv)(0, resources.size(), resources.data());
        (device.Get()->*stage.set_sampler)(0, samplers.size(), samplers.data());
      }
      if (reset)
        device->ClearState();
      ComPtr<ID3D10VertexShader> v;
      ComPtr<ID3D10GeometryShader> g;
      ComPtr<ID3D10PixelShader> p;
      ComPtr<ID3D10InputLayout> l;
      ComPtr<ID3D10RasterizerState> rs;
      ComPtr<ID3D10BlendState> bs;
      ComPtr<ID3D10DepthStencilState> ds;
      ComPtr<ID3D10Predicate> pred;
      ComPtr<ID3D10Buffer> index;
      DXGI_FORMAT format;
      UINT offset, sample_mask, ref;
      BOOL value;
      float factor[4];
      device->VSGetShader(&v);
      device->GSGetShader(&g);
      device->PSGetShader(&p);
      device->IAGetInputLayout(&l);
      device->RSGetState(&rs);
      device->OMGetBlendState(&bs, factor, &sample_mask);
      device->OMGetDepthStencilState(&ds, &ref);
      device->GetPredication(&pred, &value);
      device->IAGetIndexBuffer(&index, &format, &offset);
      D3D10_PRIMITIVE_TOPOLOGY topology;
      device->IAGetPrimitiveTopology(&topology);
      expect(
          topology == (reset ? D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED : D3D10_PRIMITIVE_TOPOLOGY_POINTLIST),
          "topology after ClearState"
      );
      expect(
          v.Get() == (reset ? nullptr : vertex_shader.Get()) && g.Get() == (reset ? nullptr : geometry_shader.Get()) &&
              p.Get() == (reset ? nullptr : pixel_shader.Get()) && l.Get() == (reset ? nullptr : layout.Get()) &&
              rs.Get() == (reset ? nullptr : raster.Get()) && bs.Get() == (reset ? nullptr : blend.Get()) &&
              ds.Get() == (reset ? nullptr : depth.Get()) && pred.Get() == (reset ? nullptr : predicate.Get()),
          "shader, layout, predicate or state object identity"
      );
      expect(
          index.Get() == (reset ? nullptr : vertex.Get()) && offset == (reset ? 0u : sizeof(UINT)) &&
              format == (reset ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_UINT),
          "index binding fields"
      );
      expect(
          sample_mask == (reset ? D3D10_DEFAULT_SAMPLE_MASK : mask) && ref == (reset ? 0u : stencil_ref),
          "sample mask or stencil reference"
      );
      for (UINT i = 0; i < std::size(factor); i++)
        expect(factor[i] == (reset ? 1.0f : factors[i]), "blend factor %u", i);
      std::vector<ID3D10Buffer *> got(vertex_slots);
      std::vector<UINT> got_strides(vertex_slots), got_offsets(vertex_slots);
      device->IAGetVertexBuffers(0, vertex_slots, got.data(), got_strides.data(), got_offsets.data());
      for (UINT i = 0; i < vertex_slots; i++) {
        expect(
            got[i] == (reset ? nullptr : vertex.Get()) && got_strides[i] == (reset ? 0u : strides[i]) &&
                got_offsets[i] == (reset ? 0u : vertex_offsets[i]),
            "vertex slot %u", i
        );
        if (got[i])
          got[i]->Release();
      }
      device->IAGetVertexBuffers(0, vertex_slots, nullptr, got_strides.data(), got_offsets.data());
      ID3D10RenderTargetView *got_targets[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
      ComPtr<ID3D10DepthStencilView> got_depth;
      device->OMGetRenderTargets(std::size(got_targets), got_targets, &got_depth);
      expect(got_depth.Get() == (reset ? nullptr : depth_view.Get()), "depth target after ClearState");
      for (UINT i = 0; i < std::size(got_targets); i++) {
        expect(got_targets[i] == (reset ? nullptr : targets[i].Get()), "render target %u", i);
        if (got_targets[i])
          got_targets[i]->Release();
      }
      device->OMGetRenderTargets(0, nullptr, &got_depth);
      got.resize(streams.size());
      device->SOGetTargets(streams.size(), got.data(), got_offsets.data());
      for (UINT i = 0; i < streams.size(); i++) {
        expect(
            got[i] == (reset ? nullptr : streams[i].Get()) && got_offsets[i] == (reset ? 0u : offsets[i]),
            "stream output slot %u", i
        );
        if (got[i])
          got[i]->Release();
      }
      for (auto stage : stages) {
        ID3D10Buffer *constants[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
        ID3D10ShaderResourceView *resources[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
        ID3D10SamplerState *samplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT]{};
        (device.Get()->*stage.get_cb)(0, std::size(constants), constants);
        (device.Get()->*stage.get_srv)(0, std::size(resources), resources);
        (device.Get()->*stage.get_sampler)(0, std::size(samplers), samplers);
        auto check = [&](auto &objects, auto wanted) {
          for (auto object : objects) {
            expect(object == (reset ? nullptr : wanted), "%s stage binding after reset %u", stage.name, reset);
            if (object)
              object->Release();
          }
        };
        check(constants, cb.Get());
        check(resources, srv.Get());
        check(samplers, sampler.Get());
      }
    }
    device->ClearState();
    for (bool depth_output : {false, true}) {
      constexpr UINT width = 4, mip_count = 3, array_count = 3;
      D3D10_TEXTURE2D_DESC td{
          width,
          width,
          mip_count,
          array_count,
          depth_output ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32_FLOAT,
          {1, 0},
          D3D10_USAGE_DEFAULT,
          UINT(D3D10_BIND_SHADER_RESOURCE | (depth_output ? D3D10_BIND_DEPTH_STENCIL : D3D10_BIND_RENDER_TARGET))
      };
      ComPtr<ID3D10Texture2D> texture;
      CHECK(device->CreateTexture2D(&td, nullptr, &texture));
      D3D10_DEPTH_STENCIL_DESC no_write{
          TRUE,
          D3D10_DEPTH_WRITE_MASK_ZERO,
          D3D10_COMPARISON_ALWAYS,
          FALSE,
          D3D10_DEFAULT_STENCIL_READ_MASK,
          D3D10_DEFAULT_STENCIL_WRITE_MASK,
          {D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_COMPARISON_ALWAYS},
          {D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_COMPARISON_ALWAYS}
      };
      ComPtr<ID3D10DepthStencilState> depth_read;
      CHECK(device->CreateDepthStencilState(&no_write, &depth_read));
      for (UINT first_mip = 0; first_mip < mip_count; first_mip++)
        for (UINT mips = 1; mips <= mip_count - first_mip; mips++)
          for (UINT first_slice = 0; first_slice < array_count; first_slice++)
            for (UINT slices = 1; slices <= array_count - first_slice; slices++) {
              D3D10_SHADER_RESOURCE_VIEW_DESC sd{DXGI_FORMAT_R32_FLOAT, D3D10_SRV_DIMENSION_TEXTURE2DARRAY};
              sd.Texture2DArray = {first_mip, mips, first_slice, slices};
              ComPtr<ID3D10ShaderResourceView> input;
              CHECK(device->CreateShaderResourceView(texture.Get(), &sd, &input));
              for (UINT output_mip = 0; output_mip < mip_count; output_mip++)
                for (UINT output_slice = 0; output_slice < array_count; output_slice++) {
                  ComPtr<ID3D10RenderTargetView> color_view;
                  ComPtr<ID3D10DepthStencilView> depth_view;
                  if (depth_output) {
                    D3D10_DEPTH_STENCIL_VIEW_DESC dd{DXGI_FORMAT_D32_FLOAT, D3D10_DSV_DIMENSION_TEXTURE2DARRAY};
                    dd.Texture2DArray = {output_mip, output_slice, 1};
                    CHECK(device->CreateDepthStencilView(texture.Get(), &dd, &depth_view));
                  } else {
                    D3D10_RENDER_TARGET_VIEW_DESC rd{DXGI_FORMAT_R32_FLOAT, D3D10_RTV_DIMENSION_TEXTURE2DARRAY};
                    rd.Texture2DArray = {output_mip, output_slice, 1};
                    CHECK(device->CreateRenderTargetView(texture.Get(), &rd, &color_view));
                  }
                  const bool overlap = output_mip >= first_mip && output_mip - first_mip < mips &&
                                       output_slice >= first_slice && output_slice - first_slice < slices;
                  for (auto stage : stages)
                    for (bool output_first : {false, true}) {
                      step(
                          "level=%#x %s hazard depth=%u input mip=%u+%u slice=%u+%u output=%u/%u output_first=%u",
                          level, stage.name, depth_output, first_mip, mips, first_slice, slices, output_mip,
                          output_slice, output_first
                      );
                      device->ClearState();
                      device->OMSetDepthStencilState(depth_read.Get(), 0);
                      auto bind_output = [&] {
                        device->OMSetRenderTargets(
                            depth_output ? 0 : 1, depth_output ? nullptr : color_view.GetAddressOf(), depth_view.Get()
                        );
                      };
                      if (output_first)
                        bind_output();
                      std::vector<ID3D10ShaderResourceView *> inputs(
                          D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, input.Get()
                      );
                      (device.Get()->*stage.set_srv)(0, inputs.size(), inputs.data());
                      if (!output_first)
                        bind_output();
                      (device.Get()->*stage.get_srv)(0, inputs.size(), inputs.data());
                      for (UINT slot = 0; slot < inputs.size(); slot++) {
                        expect(inputs[slot] == (overlap ? nullptr : input.Get()), "hazard binding at slot=%u", slot);
                        if (inputs[slot])
                          inputs[slot]->Release();
                      }
                      ComPtr<ID3D10RenderTargetView> got_color;
                      ComPtr<ID3D10DepthStencilView> got_depth;
                      device->OMGetRenderTargets(1, &got_color, &got_depth);
                      expect(got_color == color_view && got_depth == depth_view, "input binding changed the output");
                    }
                  device->ClearState();
                }
            }
      device->ClearState();
    }
    for (UINT count :
         {UINT(D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT), UINT(D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT + 1),
          UINT(D3D10_1_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1), UINT(D3D10_1_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)}) {
      if (count > vertex_slots)
        continue;
      step("level=%#x complete vertex getter count=%u after ClearState", level, count);
      const UINT marker = sizeof(UINT);
      std::vector<ID3D10Buffer *> got(count + 1, vertex.Get());
      std::vector<UINT> got_strides(count + 1, marker), got_offsets(count + 1, marker);
      device->IAGetVertexBuffers(0, count, got.data(), got_strides.data(), got_offsets.data());
      for (UINT slot = 0; slot <= count; slot++)
        expect(
            got[slot] == (slot < count ? nullptr : vertex.Get()) && got_strides[slot] == (slot < count ? 0u : marker) &&
                got_offsets[slot] == (slot < count ? 0u : marker),
            "large vertex getter slot=%u count=%u", slot, count
        );
    }
  }
  return verdict();
}
