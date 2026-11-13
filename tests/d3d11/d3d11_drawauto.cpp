// contract: DrawAuto draws the vertices that stream output filled a buffer with, from the offset of vertex buffer slot
// 0 to the buffer's filled size, as one instance of the current topology, without the count reaching the CPU (D3D11.3
// functional spec, 8.9). a vertex shader streams points out in two draws, the second appending. DrawAuto feeds them,
// from the second vertex on, through a geometry shader that streams each out twice into a second buffer; DrawAuto
// feeds that one through a vertex shader streaming out alone into a third; and DrawAuto draws the third as points, one
// pixel per vertex, its vertex shader streaming out what it draws too: stream output does not replace rasterization
// (14.7). the draws span several object threadgroups, each stage changes the vertices, and the expectations
// are the same stages run on the CPU.
// 8.9: "Buffers may be bound to other IA input slots above zero for DrawAuto" and the draw uses the current IA
// topology. all nine nonpatch topologies below recirculate the point-filled buffer, with overlapping/ordinary/wide
// strides, offsets below/at/above its filled size, and an extra slot whose out-of-bounds fetches return zero.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#include "d3d11_test.hpp"
#include "../assembly.hpp"
#include <algorithm>
#include <array>

static const char hlsl[] = R"hlsl(
struct D { uint4 value : VALUE; };
D vs_source(uint id : SV_VertexID) { D d; d.value = uint4(id, id * 7, 1, 2); return d; }
D vs_pass(D d) { return d; }
D vs_extra(D d, uint extra : EXTRA) { d.value.y += extra; return d; }
[maxvertexcount(2)]
void gs_twice(point D v[1], inout PointStream<D> s) {
  D d = v[0];
  s.Append(d);
  d.value.z += 1;
  s.Append(d);
}
D vs_step(D d) { d.value.w += 10; return d; }
struct R { float4 pos : SV_Position; nointerpolation uint value : SHOWN; };
// vertex n covers pixel n of the one row
R vs_draw(D d, uint id : SV_VertexID) {
  R r;
  r.pos = float4((id + 0.5) / WIDTH * 2 - 1, 0, 0, 1);
  r.value = d.value.x + d.value.z + d.value.w;
  return r;
}
uint ps(R r) : SV_Target { return r.value; }
)hlsl";

int
main() {
  using Vertex = std::array<uint32_t, 4>;
  const UINT first = 70, second = 5, skipped = 1, stride = sizeof(Vertex);
  // the CPU's stages
  std::vector<Vertex> a, b, c;
  for (UINT count : {first, second})
    for (UINT id = 0; id < count; id++)
      a.push_back({id, id * 7, 1, 2});
  for (size_t i = skipped; i < a.size(); i++) {
    b.push_back(a[i]);
    b.push_back({a[i][0], a[i][1], a[i][2] + 1, a[i][3]});
  }
  for (auto v : b)
    c.push_back({v[0], v[1], v[2], v[3] + 10});
  // a target wider than the vertices drawn, so drawing too many shows
  const UINT width = c.size() + 8;

  std::vector<std::string> defines = {"WIDTH=" + std::to_string(width)};
  auto vs_source = compile(hlsl, "vs_source", "vs", defines), vs_pass = compile(hlsl, "vs_pass", "vs", defines),
       vs_extra = compile(hlsl, "vs_extra", "vs", defines),
       gs_twice = compile(hlsl, "gs_twice", "gs", defines), vs_step = compile(hlsl, "vs_step", "vs", defines),
       vs_draw = compile(hlsl, "vs_draw", "vs", defines), ps = compile(hlsl, "ps", "ps", defines);
  if (!vs_source || !vs_pass || !vs_extra || !gs_twice || !vs_step || !vs_draw || !ps) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));

  const D3D11_SO_DECLARATION_ENTRY entries[] = {{0, "VALUE", 0, 0, 4, 0}};
  const D3D11_INPUT_ELEMENT_DESC elements[] = {{"VALUE", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  ComPtr<ID3D11VertexShader> source, pass, step, draw;
  ComPtr<ID3D11GeometryShader> source_out, twice_out, step_out;
  ComPtr<ID3D11PixelShader> pixel;
  ComPtr<ID3D11InputLayout> layout;
  auto stream_out = [&](ID3DBlob *code, ID3D11GeometryShader **shader) {
    return device->CreateGeometryShaderWithStreamOutput(
        code->GetBufferPointer(), code->GetBufferSize(), entries, 1, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, shader
    );
  };
  CHECK(device->CreateVertexShader(vs_source->GetBufferPointer(), vs_source->GetBufferSize(), nullptr, &source));
  CHECK(device->CreateVertexShader(vs_pass->GetBufferPointer(), vs_pass->GetBufferSize(), nullptr, &pass));
  CHECK(device->CreateVertexShader(vs_step->GetBufferPointer(), vs_step->GetBufferSize(), nullptr, &step));
  CHECK(device->CreateVertexShader(vs_draw->GetBufferPointer(), vs_draw->GetBufferSize(), nullptr, &draw));
  CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
  CHECK(device->CreateInputLayout(elements, 1, vs_pass->GetBufferPointer(), vs_pass->GetBufferSize(), &layout));
  CHECK(stream_out(vs_source.Get(), &source_out));
  CHECK(stream_out(gs_twice.Get(), &twice_out));
  CHECK(stream_out(vs_step.Get(), &step_out));
  // the drawing vertex shader's stream, which rasterizes
  const D3D11_SO_DECLARATION_ENTRY shown_entries[] = {{0, "SHOWN", 0, 0, 1, 0}};
  const UINT shown_stride = sizeof(uint32_t);
  ComPtr<ID3D11GeometryShader> draw_out;
  CHECK(device->CreateGeometryShaderWithStreamOutput(
      vs_draw->GetBufferPointer(), vs_draw->GetBufferSize(), shown_entries, 1, &shown_stride, 1, 0, nullptr, &draw_out
  ));

  const UINT sentinel = 0xa5a5a5a5u, append = ~0u, none = 0;
  auto filled = [&](size_t vertices) {
    return buffer(device.Get(), stride * (vertices + 1), D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_VERTEX_BUFFER, sentinel);
  };
  auto buffer_a = filled(a.size()), buffer_b = filled(b.size()), buffer_c = filled(c.size());
  auto buffer_d = buffer(device.Get(), shown_stride * (c.size() + 1), D3D11_BIND_STREAM_OUTPUT, sentinel);
  auto target = [&](ID3D11Buffer *to, UINT offset) { context->SOSetTargets(1, &to, &offset); };
  auto vertices = [&](ID3D11Buffer *from, UINT offset) {
    context->SOSetTargets(0, nullptr, nullptr);
    context->IASetVertexBuffers(0, 1, &from, &stride, &offset);
  };
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  context->VSSetShader(source.Get(), nullptr, 0);
  context->GSSetShader(source_out.Get(), nullptr, 0);
  target(buffer_a.Get(), 0);
  context->Draw(first, 0);
  target(buffer_a.Get(), append);
  context->Draw(second, 0);

  ComPtr<ID3D11Query> statistics;
  D3D11_QUERY_DESC query_desc{D3D11_QUERY_SO_STATISTICS};
  CHECK(device->CreateQuery(&query_desc, &statistics));
  context->IASetInputLayout(layout.Get());
  vertices(buffer_a.Get(), skipped * stride);
  context->VSSetShader(pass.Get(), nullptr, 0);
  context->GSSetShader(twice_out.Get(), nullptr, 0);
  target(buffer_b.Get(), 0);
  context->Begin(statistics.Get());
  context->DrawAuto();
  context->End(statistics.Get());

  vertices(buffer_b.Get(), 0);
  context->VSSetShader(step.Get(), nullptr, 0);
  context->GSSetShader(step_out.Get(), nullptr, 0);
  target(buffer_c.Get(), 0);
  context->DrawAuto();

  D3D11_TEXTURE2D_DESC texture_desc{width, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> texture, texture_read;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture));
  CHECK(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv));
  const float clear[4] = {};
  context->ClearRenderTargetView(rtv.Get(), clear);
  D3D11_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
  context->RSSetViewports(1, &viewport);
  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  vertices(buffer_c.Get(), 0);
  target(buffer_d.Get(), 0);
  context->VSSetShader(draw.Get(), nullptr, 0);
  context->GSSetShader(draw_out.Get(), nullptr, 0);
  context->PSSetShader(pixel.Get(), nullptr, 0);
  context->DrawAuto();

  unsigned failures = 0;
  auto expect = [&](const char *what, uint64_t value, uint64_t want) {
    if (value != want && failures++ < 12)
      printf("%s: %#llx, want %#llx\n", what, (unsigned long long)value, (unsigned long long)want);
  };
  auto check = [&](const char *what, ID3D11Buffer *from, const std::vector<Vertex> &want) {
    auto got = read(device.Get(), context.Get(), from);
    for (size_t v = 0; v < want.size(); v++)
      for (int k = 0; k < 4; k++)
        expect(what, got[4 * v + k], want[v][k]);
    expect(what, got[4 * want.size()], sentinel);
  };
  check("the source", buffer_a.Get(), a);
  check("each vertex twice", buffer_b.Get(), b);
  check("the vertex shader alone", buffer_c.Get(), c);
  auto shown = read(device.Get(), context.Get(), buffer_d.Get());
  for (size_t v = 0; v < c.size(); v++)
    expect("the drawn value streamed", shown[v], c[v][0] + c[v][2] + c[v][3]);
  expect("the drawn value streamed", shown[c.size()], sentinel);
  D3D11_QUERY_DATA_SO_STATISTICS counted{};
  HRESULT hr;
  do {
    hr = context->GetData(statistics.Get(), &counted, sizeof(counted), 0);
  } while (hr == S_FALSE);
  if (!::expect(hr == S_OK, "SO statistics returned %#lx", hr))
    return verdict();
  expect("primitives written", counted.NumPrimitivesWritten, b.size());
  expect("primitives needed", counted.PrimitivesStorageNeeded, b.size());

  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture_read));
  context->CopyResource(texture_read.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped;
  CHECK(context->Map(texture_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  for (UINT x = 0; x < width; x++)
    expect("drawn pixel", static_cast<const uint32_t *>(mapped.pData)[x], x < c.size() ? c[x][0] + c[x][2] + c[x][3] : none);
  context->Unmap(texture_read.Get(), 0);
  if (failures) {
    printf("failed: %u wrong values\n", failures);
    return 1;
  }
  ComPtr<ID3D11DeviceContext> deferred;
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> extra_vertex;
  ComPtr<ID3D11GeometryShader> extra_stream;
  ComPtr<ID3D11InputLayout> extra_layout;
  CHECK(device->CreateVertexShader(vs_extra->GetBufferPointer(), vs_extra->GetBufferSize(), nullptr, &extra_vertex));
  CHECK(stream_out(vs_extra.Get(), &extra_stream));
  const UINT extra_slot = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1, word = sizeof(UINT);
  const D3D11_INPUT_ELEMENT_DESC extra_elements[] = {
      elements[0], {"EXTRA", 0, DXGI_FORMAT_R32_UINT, extra_slot, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  CHECK(device->CreateInputLayout(extra_elements, std::size(extra_elements), vs_extra->GetBufferPointer(),
                                  vs_extra->GetBufferSize(), &extra_layout));
  std::vector<UINT> extra(a.size() / 2), source_words((a.size() + 1) * 4, sentinel);
  for (UINT i = 0; i < extra.size(); i++)
    extra[i] = i * 17 + 3;
  for (UINT i = 0; i < a.size(); i++)
    std::copy(a[i].begin(), a[i].end(), source_words.begin() + i * 4);
  D3D11_BUFFER_DESC extra_desc{UINT(extra.size()) * word, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
  const D3D11_SUBRESOURCE_DATA extra_initial{extra.data()};
  ComPtr<ID3D11Buffer> extra_buffer;
  CHECK(device->CreateBuffer(&extra_desc, &extra_initial, &extra_buffer));
  const UINT filled_bytes = a.size() * stride, maximum_vertices = filled_bytes / word;
  auto recirculated = filled(3 * maximum_vertices);
  if (!::expect(bool(recirculated), "no recirculation output buffer"))
    return verdict();
  const UINT output_words = (3 * maximum_vertices + 1) * 4;
  for (auto draw_context : {context.Get(), deferred.Get()})
    for (auto topology : assembly::cases)
      for (UINT input_stride : {word, stride, 2 * stride})
        for (UINT offset : {0u, stride, filled_bytes, filled_bytes + stride}) {
          ::step("DrawAuto %s topology %u stride %u offset %u, extra slot %u", draw_context == context.Get() ? "immediate" : "deferred",
               topology.topology, input_stride, offset, extra_slot);
          const UINT drawn_vertices = filled_bytes > offset ? (filled_bytes - offset) / input_stride : 0;
          const bool adjacent = topology.topology >= D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
          const UINT first_primitive = topology.strip ? topology.corners * (adjacent ? 2 : 1) : topology.stride;
          const UINT advance = topology.strip ? topology.every : topology.stride;
          const UINT primitives = drawn_vertices >= first_primitive ? (drawn_vertices - first_primitive) / advance + 1 : 0;
          std::vector<UINT> want(output_words, sentinel), fill(want);
          UINT at = 0;
          for (UINT primitive = 0; primitive < primitives; primitive++)
            for (UINT corner = 0; corner < topology.corners; corner++) {
              const UINT order = topology.strip && topology.corners == 3 && (primitive & 1) && corner ? 3 - corner : corner;
              const UINT vertex = topology.strip ? topology.first + (primitive + order) * topology.every
                                                 : primitive * topology.stride + topology.first + corner * topology.every;
              const UINT address = offset + vertex * input_stride;
              Vertex value{};
              if (address + sizeof(value) <= source_words.size() * word)
                memcpy(value.data(), reinterpret_cast<const char *>(source_words.data()) + address, sizeof(value));
              value[1] += vertex < extra.size() ? extra[vertex] : 0;
              std::copy(value.begin(), value.end(), want.begin() + at);
              at += value.size();
            }
          draw_context->UpdateSubresource(recirculated.Get(), 0, nullptr, fill.data(), 0, 0);
          draw_context->IASetInputLayout(extra_layout.Get());
          draw_context->IASetPrimitiveTopology(topology.topology);
          draw_context->IASetVertexBuffers(0, 1, buffer_a.GetAddressOf(), &input_stride, &offset);
          draw_context->IASetVertexBuffers(extra_slot, 1, extra_buffer.GetAddressOf(), &word, &none);
          draw_context->VSSetShader(extra_vertex.Get(), nullptr, 0);
          draw_context->GSSetShader(extra_stream.Get(), nullptr, 0);
          draw_context->PSSetShader(nullptr, nullptr, 0);
          draw_context->OMSetRenderTargets(0, nullptr, nullptr);
          draw_context->SOSetTargets(1, recirculated.GetAddressOf(), &none);
          draw_context->DrawAuto();
          draw_context->SOSetTargets(0, nullptr, nullptr);
          if (draw_context == deferred.Get()) {
            ComPtr<ID3D11CommandList> list;
            CHECK(draw_context->FinishCommandList(FALSE, &list));
            context->ExecuteCommandList(list.Get(), TRUE);
          }
          auto got = read(device.Get(), context.Get(), recirculated.Get());
          if (::expect(got.size() == want.size(), "read %zu words, want %zu", got.size(), want.size()))
            for (UINT i = 0; i < want.size(); i++)
              if (got[i] != want[i]) {
                ::expect(false, "word %u is %#x, want %#x", i, got[i], want[i]);
                break;
              }
        }
  return verdict();
}
