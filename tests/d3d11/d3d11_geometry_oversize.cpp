// contract: an oversized draw does not discard the draws around it, and every omitted indirect submission warns
// after completion. "the GPU finished running" (Apple, MTLCommandBufferStatus.completed).
// "The value should be sampled at the instant that the GPU is finished with all the preceding workload."
// (D3D11.3 functional spec, 20.4.3). readback waits for the queue, while the device stays alive: neither another
// indirect draw nor device teardown may be needed to report the warning. MULLION_MAX_MESH_THREADGROUPS lowers the
// budget on every family. the big draw is clipped; the small draws before and after it each own half the target.
#include "d3d11_test.hpp"
#include <fstream>
#include <iterator>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint base; };
struct V { uint id : ID; };
V vs(uint id : SV_VertexID) { V v; v.id = base + id; return v; }
struct P { float4 pos : SV_Position; nointerpolation uint value : VALUE; };
// point p comes out once, from geometry instance p % 32, on pixel p of the first 64 and clipped after them
[maxvertexcount(1)] [instance(32)]
void gs(point V v[1], uint i : SV_GSInstanceID, inout PointStream<P> s) {
  if (i != v[0].id % 32)
    return;
  P o;
  o.pos = float4((v[0].id % 64 + 0.5) / 32 - 1, 0, v[0].id < 64 ? 0.5 : 2, 1);
  o.value = v[0].id + 1;
  s.Append(o);
}
uint ps(P p) : SV_Target { return p.value; }
)hlsl";

int
main() {
  auto vs = compile(hlsl, "vs", "vs"), gs = compile(hlsl, "gs", "gs"), ps = compile(hlsl, "ps", "ps");
  if (!expect(vs && gs && ps, "HLSL did not compile"))
    return verdict();
  const UINT width = 64, big = 96000;
  char temporary[MAX_PATH], directory[MAX_PATH];
  auto length = GetTempPathA(sizeof(temporary), temporary);
  CHECK(length && length < sizeof(temporary) ? S_OK : E_FAIL);
  CHECK(temporary_directory(temporary, "gsp", directory) ? S_OK : E_FAIL);
  CHECK(SetEnvironmentVariableA("MULLION_LOG_PATH", directory) ? S_OK : E_FAIL);
  CHECK(SetEnvironmentVariableA("MULLION_MAX_MESH_THREADGROUPS", std::to_string(big).c_str()) ? S_OK : E_FAIL);
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11GeometryShader> geometry;
  ComPtr<ID3D11PixelShader> pixel;
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
  CHECK(device->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), nullptr, &geometry));
  CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));

  D3D11_TEXTURE2D_DESC texture_desc{
      width, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> texture, texture_read;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture));
  CHECK(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv));
  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture_read));
  D3D11_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
  const float clear[4] = {};
  context->RSSetViewports(1, &viewport);
  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  context->VSSetShader(vertex.Get(), nullptr, 0);
  context->GSSetShader(geometry.Get(), nullptr, 0);
  context->PSSetShader(pixel.Get(), nullptr, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  // SV_VertexID starts at 0 in every draw (D3D11.3 8.16), so each draw's first point comes from the constant buffer
  D3D11_BUFFER_DESC constants_desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  ComPtr<ID3D11Buffer> constants;
  CHECK(device->CreateBuffer(&constants_desc, nullptr, &constants));
  context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
  const UINT arguments[4] = {big, 1, 0, 0};
  D3D11_BUFFER_DESC args_desc{sizeof(arguments), D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS};
  D3D11_SUBRESOURCE_DATA args_data{arguments};
  ComPtr<ID3D11Buffer> args;
  CHECK(device->CreateBuffer(&args_desc, &args_data, &args));
  auto draw = [&](UINT first, UINT points, bool indirect = false) {
    const UINT words[4] = {first};
    context->UpdateSubresource(constants.Get(), 0, nullptr, words, 0, 0);
    if (indirect)
      context->DrawInstancedIndirect(args.Get(), 0);
    else
      context->Draw(points, 0);
  };
  for (bool indirect : {false, true}) {
    step("read the points before and after the oversized %s draw", indirect ? "indirect" : "direct");
    context->ClearRenderTargetView(rtv.Get(), clear);
    draw(0, 32);
    draw(64, big, indirect);
    draw(32, 32);
    context->CopyResource(texture_read.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(context->Map(texture_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    auto pixels = reinterpret_cast<const uint32_t *>(mapped.pData);
    for (UINT x = 0; x < width; x++)
      if (!expect(pixels[x] == x + 1, "pixel %u: %u, want %u", x, pixels[x], x + 1))
        break;
    context->Unmap(texture_read.Get(), 0);
  }
  step("collect the warning of the completed indirect draw");
  const std::string path = std::string(directory) + "\\d3d11_geometry_oversize_d3d11.log";
  std::ifstream log(path);
  if (!expect(log.is_open(), "diagnostic log did not open: %s", path.c_str()))
    return verdict();
  const std::string lines{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
  const std::string warning = "Omitted indirect mesh draw (GS) because of too many object threadgroups";
  auto at = lines.find(warning);
  expect(at != std::string::npos, "completed indirect draw left no warning");
  expect(at == std::string::npos || lines.find(warning, at + warning.size()) == std::string::npos,
         "completed indirect draw warned more than once");
  log.close();
  printf("test log: %s\n", path.c_str());
  return verdict();
}
