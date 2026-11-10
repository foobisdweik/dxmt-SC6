// contract: native Direct3D 10/10.1 blendable formats obey the selected equations and native output encoding.
// "The min and max blend ops ignore D3D11_BLEND modes" (D3D11.3 17.1). "Values entering Blending Hardware,
// including the BlendFactor value, are always clamped to the range of the RenderTarget" (17.5), with its
// small-float exception. "For all formats, there is a clamp to the RenderTarget range after blend, before
// writing values out to memory." (17.5). all legal factors, operations, independent alpha pairs and masks
// use exact binary/small-integer inputs and CPU equations (17.1/.2/.5/.6). both state-creation methods and
// both feature levels render bytes; the unsigned small-float path avoids its optional input clamp.
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include "../depth_formats.hpp"
#include "../format_blend.hpp"

using namespace format_blend;

static const char shaders[] = R"hlsl(
Buffer<float4> source : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id==2?3:-1,id==1?3:-1,0,1); }
struct Output { float4 value : SV_Target0; float4 second : SV_Target1; };
Output ps(float4 position : SV_Position) {
  uint at = 2*uint(position.x); Output o; o.value = source[at]; o.second = source[at+1]; return o;
}
)hlsl";

int
main() {
  auto vertex = compile(shaders, "vs", "vs_4_0"), pixel = compile(shaders, "ps", "ps_4_0");
  if (!expect(vertex && pixel, "blend HLSL did not compile"))
    return verdict();
  const auto cases = equations();
  constexpr UINT state_forms = 2;
  if (!expect(
          cases.size() * state_forms <= D3D10_REQ_BLEND_OBJECT_COUNT_PER_CONTEXT &&
              cases.size() <= D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION,
          "blend cases exceed API object/image limits"
      ))
    return verdict();
  HMODULE dll = LoadLibraryA("d3d10_1.dll");
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      dll ? (void *)GetProcAddress(dll, "D3D10CreateDevice1") : nullptr
  );
  if (!expect(create != nullptr, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  UINT tested = 0;
  for (auto level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, level, D3D10_1_SDK_VERSION, &device));
    ComPtr<ID3D10VertexShader> vs;
    ComPtr<ID3D10PixelShader> ps;
    CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), &vs));
    CHECK(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), &ps));
    std::vector<ComPtr<ID3D10BlendState>> states[state_forms];
    for (auto &set : states)
      set.resize(cases.size());
    for (UINT row = 0; row < cases.size(); row++) {
      const auto &e = cases[row];
      D3D10_BLEND_DESC desc{};
      desc.BlendEnable[0] = TRUE;
      desc.SrcBlend = D3D10_BLEND(e.source);
      desc.DestBlend = D3D10_BLEND(e.destination);
      desc.BlendOp = D3D10_BLEND_OP(e.operation);
      desc.SrcBlendAlpha = D3D10_BLEND(e.alpha_source);
      desc.DestBlendAlpha = D3D10_BLEND(e.alpha_destination);
      desc.BlendOpAlpha = D3D10_BLEND_OP(e.alpha_operation);
      desc.RenderTargetWriteMask[0] = UINT8(e.write_mask);
      CHECK(device->CreateBlendState(&desc, &states[0][row]));
      D3D10_BLEND_DESC1 desc1{};
      desc1.RenderTarget[0] = {
          TRUE,
          desc.SrcBlend,
          desc.DestBlend,
          desc.BlendOp,
          desc.SrcBlendAlpha,
          desc.DestBlendAlpha,
          desc.BlendOpAlpha,
          desc.RenderTargetWriteMask[0]
      };
      ComPtr<ID3D10BlendState1> state;
      CHECK(device->CreateBlendState1(&desc1, &state));
      CHECK(state.As(&states[1][row]));
    }
    D3D10_RASTERIZER_DESC rd{D3D10_FILL_SOLID, D3D10_CULL_NONE};
    rd.DepthClipEnable = TRUE;
    ComPtr<ID3D10RasterizerState> raster;
    CHECK(device->CreateRasterizerState(&rd, &raster));
    for (const auto &f : formats()) {
      UINT support = 0;
      HRESULT hr = device->CheckFormatSupport(f.id, &support);
      if (FAILED(hr))
        continue;
      if (!(support & D3D10_FORMAT_SUPPORT_BLENDABLE))
        continue;
      if (!expect(
              f.kind != Unsigned && f.kind != Signed && f.kind != Shared &&
                  (support & D3D10_FORMAT_SUPPORT_RENDER_TARGET),
              "%s invalid blend capability", f.name.c_str()
          ))
        continue;
      const UINT height = UINT(cases.size());
      for (UINT phase = 0; phase < phase_count; phase++) {
        std::vector<Source> source;
        std::vector<uint8_t> destination;
        inputs(f, phase, source, destination);
        static_assert(sizeof(Source) == 2 * sizeof(Color));
        D3D10_BUFFER_DESC bd{UINT(source.size() * sizeof(Source)), D3D10_USAGE_IMMUTABLE, D3D10_BIND_SHADER_RESOURCE};
        D3D10_SUBRESOURCE_DATA data{source.data()};
        ComPtr<ID3D10Buffer> input;
        ComPtr<ID3D10ShaderResourceView> srv;
        CHECK(device->CreateBuffer(&bd, &data, &input));
        D3D10_SHADER_RESOURCE_VIEW_DESC sv{DXGI_FORMAT_R32G32B32A32_FLOAT, D3D10_SRV_DIMENSION_BUFFER};
        sv.Buffer.NumElements = UINT(source.size() * sizeof(Source) / sizeof(Color));
        CHECK(device->CreateShaderResourceView(input.Get(), &sv, &srv));
        std::vector<uint8_t> initial(destination.size() * height);
        for (UINT row = 0; row < height; row++)
          memcpy(initial.data() + row * destination.size(), destination.data(), destination.size());
        D3D10_TEXTURE2D_DESC td{width, height, 1, 1, f.id, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET};
        data = {initial.data(), width * f.bytes};
        ComPtr<ID3D10Texture2D> target, staging;
        ComPtr<ID3D10RenderTargetView> rtv;
        CHECK(device->CreateTexture2D(&td, &data, &target));
        CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        td.Usage = D3D10_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        CHECK(device->CreateTexture2D(&td, nullptr, &staging));
        for (UINT form = 0; form < state_forms; form++) {
          step(
              "D3D10 level %#x %s phase %u state form %u: %u equations/masks", level, f.name.c_str(), phase, form,
              height
          );
          device->ClearState();
          device->UpdateSubresource(target.Get(), 0, nullptr, initial.data(), width * f.bytes, 0);
          device->VSSetShader(vs.Get());
          device->PSSetShader(ps.Get());
          device->PSSetShaderResources(0, 1, srv.GetAddressOf());
          device->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
          device->RSSetState(raster.Get());
          device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          const auto constant = constants(phase);
          for (UINT row = 0; row < height; row++) {
            D3D10_VIEWPORT viewport{0, INT(row), width, 1, 0, 1};
            device->RSSetViewports(1, &viewport);
            device->OMSetBlendState(states[form][row].Get(), constant.data(), D3D10_DEFAULT_SAMPLE_MASK);
            device->Draw(3, 0);
          }
          device->ClearState();
          device->CopyResource(staging.Get(), target.Get());
          D3D10_MAPPED_TEXTURE2D mapped{};
          CHECK(staging->Map(0, D3D10_MAP_READ, 0, &mapped));
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
          staging->Unmap(0);
          tested++;
          if (!same)
            return verdict();
        }
      }
    }
  }
  FreeLibrary(dll);
  expect(tested != 0, "no D3D10 blendable format ran");
  return verdict();
}
