// contract: a direct draw through a geometry shader streams out every primitive, in the API's order, however many
// parts it takes. InstanceCount is "How many Instances to render." (D3D11.3 functional spec, 8.4), and "The hardware
// always writes as many complete primitives" (14.5). Apple limits payload and mesh geometry together to 4 GB per
// draw on Apple7/8 (Metal Feature Set Tables, footnote 10). "The total size of metal::mesh cannot exceed 16 kilobytes."
// (Apple, WWDC22 "Transform your geometry with Metal mesh shaders", 11:47). two draws split along points and
// instances. narrow and wide outputs stream every emitted value in order, plus an unwritten sentinel. the wide shader
// declares over 2 KB per mesh: 16 vertices with 37 scalars each, within D3D's 1024-component limit (13.3). 65536
// inputs times 32 geometry instances exceed 4 GB at that output size.
#include "d3d11_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint points; };
struct V { uint key : KEY; };
V vs(uint id : SV_VertexID, uint instance : SV_InstanceID) { V v; v.key = instance * points + id; return v; }
struct P {
  float4 pos : SV_Position;
  uint value : VALUE;
#if WIDE
  uint4 padding[8] : PADDING;
#endif
};
[maxvertexcount(OUTPUT_VERTICES)] [instance(32)]
void gs(point V v[1], uint i : SV_GSInstanceID, inout PointStream<P> s) {
  if (i != v[0].key % 32)
    return;
  P o;
  o.pos = float4(0, 0, 0, 1);
  for (uint j = 0; j < OUTPUT_VERTICES; j++) {
    o.value = (v[0].key * OUTPUT_VERTICES + j) * 32 + i;
#if WIDE
    for (uint k = 0; k < 8; k++)
      o.padding[k] = o.value + k;
#endif
    s.Append(o);
  }
}
)hlsl";

int
main() {
  ComPtr<ID3DBlob> vs[2], gs[2];
  for (bool wide : {false, true}) {
    const std::vector<std::string> defines = {"WIDE=" + std::to_string(wide),
                                               "OUTPUT_VERTICES=" + std::to_string(wide ? 16 : 1)};
    vs[wide] = compile(hlsl, "vs", "vs", defines);
    gs[wide] = compile(hlsl, "gs", "gs", defines);
    if (!expect(vs[wide] && gs[wide], "HLSL did not compile"))
      return verdict();
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  std::vector<D3D11_SO_DECLARATION_ENTRY> entries{{0, "VALUE", 0, 0, 1, 0}};
  for (UINT i = 0; i < 8; i++)
    entries.push_back({0, "PADDING", i, 0, 4, 0});
  ComPtr<ID3D11VertexShader> vertex[2];
  ComPtr<ID3D11GeometryShader> geometry[2];
  for (bool wide : {false, true}) {
    CHECK(device->CreateVertexShader(vs[wide]->GetBufferPointer(), vs[wide]->GetBufferSize(), nullptr, &vertex[wide]));
    const UINT stride = sizeof(UINT) * (wide ? 1 + 8 * 4 : 1);
    CHECK(device->CreateGeometryShaderWithStreamOutput(
        gs[wide]->GetBufferPointer(), gs[wide]->GetBufferSize(), entries.data(), wide ? entries.size() : 1, &stride,
        1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &geometry[wide]
    ));
  }
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);

  const UINT sentinel = 0xa5a5a5a5u;
  struct Draw {
    const char *what;
    UINT points, instances;
  };
  for (bool wide : {false, true})
    for (auto [what, points, instances] :
         {Draw{"points", wide ? 65536u : 96000u, 1}, Draw{"instances", 64, wide ? 1024u : 2000u}}) {
      step("stream every %s output of %s in order", wide ? "wide" : "narrow", what);
      context->VSSetShader(vertex[wide].Get(), nullptr, 0);
      context->GSSetShader(geometry[wide].Get(), nullptr, 0);
      const UINT outputs = wide ? 16 : 1, output_words = wide ? 1 + 8 * 4 : 1;
      D3D11_BUFFER_DESC constants_desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
      const UINT words[4] = {points};
      D3D11_SUBRESOURCE_DATA constants_data{words};
      ComPtr<ID3D11Buffer> constants;
      CHECK(device->CreateBuffer(&constants_desc, &constants_data, &constants));
      context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
      const UINT total = points * instances * outputs * output_words;
      auto out = buffer(device.Get(), sizeof(UINT) * (total + 1), D3D11_BIND_STREAM_OUTPUT, sentinel);
      UINT offset = 0;
      ID3D11Buffer *targets[] = {out.Get()};
      context->SOSetTargets(1, targets, &offset);
      context->DrawInstanced(points, instances, 0, 0);
      context->SOSetTargets(0, nullptr, nullptr);
      auto got = read(device.Get(), context.Get(), out.Get());
      if (!expect(got.size() == total + 1, "readback has %zu words, want %u", got.size(), total + 1))
        continue;
      for (UINT k = 0; k <= total; k++) {
        UINT vertex = k / output_words, component = k % output_words;
        UINT want = k < total ? vertex * 32 + vertex / outputs % 32 + (component ? (component - 1) / 4 : 0) : sentinel;
        if (!expect(got[k] == want, "%s, value %u: %#x, want %#x", what, k, got[k], want))
          break;
      }
    }
  return verdict();
}
