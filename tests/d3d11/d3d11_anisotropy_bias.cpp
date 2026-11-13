// contract: a pixel shader's Sample, SampleBias and SampleCmp through an anisotropic sampler state add the LOD bias
// to the LOD of the clamped footprint. D3D11.3 7.18.11: "if ( output.ratioOfAnisotropy > input.maxAniso )" then
// "lengthMinor = lengthMajor/output.ratioOfAnisotropy", else "lengthMinor = determinant/lengthMajor";
// "output.LOD = log2(lengthMinor)", then "biasedLOD = output.LOD + MipLODBias; biasedLOD = biasedLOD + srcLODBias;"
// and "clampedLOD = max(MinLOD,(min(MaxLOD, biasedLOD)))". mip m of each texture holds m, and anisotropic filters
// are linear between mips (7.18.10), so a sample reads its LOD; a comparison passes where the reference is below the
// texel (LESS), so references half a mip on either side of the expected LOD tell it apart. footprints are
// axis-aligned powers of two, so every expected LOD is a whole mip. MaxAnisotropy 1 and a bias of 0 are controls.
#include "d3d11_test.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>

// the footprints in texels of the first mip per pixel along x and y, one 2x2 quad each
static const float footprints[][2] = {{16, 1}, {1, 16}, {4, 4}, {64, 4}, {2, 1}};
constexpr unsigned quads = std::size(footprints);

static const char hlsl[] = R"hlsl(
Texture2D<float> t : register(t0);
Texture2D<float> d : register(t1);
SamplerState s : register(s0);
SamplerComparisonState c : register(s1);
cbuffer C : register(b0) { float4 quad[QUADS]; float bias; };
struct O { float t : SV_Target0; float b : SV_Target1; float lo : SV_Target2; float hi : SV_Target3; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
O ps(float4 p : SV_Position) {
  // xy: the quad's footprint, zw: the comparison references
  float4 q = quad[uint(p.x) / 2];
  uint width, height, mips;
  t.GetDimensions(0, width, height, mips);
  float2 uv = p.xy * q.xy / float2(width, height);
  O o;
  o.t = t.Sample(s, uv);
  o.b = t.SampleBias(s, uv, bias);
  o.lo = d.SampleCmp(c, uv, q.z);
  o.hi = d.SampleCmp(c, uv, q.w);
  return o;
}
)hlsl";

// D3D11.3 7.18.11 for an axis-aligned footprint, then the bias and the clamp to the mips there are
static float
lod(const float footprint[2], float anisotropy, float bias, unsigned mips) {
  float major = std::max(footprint[0], footprint[1]), determinant = footprint[0] * footprint[1];
  float ratio = major * major / determinant;
  float minor = ratio > anisotropy ? major / anisotropy : determinant / major;
  return std::clamp(std::log2(minor) + bias, 0.0f, float(mips - 1));
}

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  step("compile the sampling shaders");
  const std::string define = "QUADS=" + std::to_string(quads);
  auto vs_code = compile(hlsl, "vs", "vs", {define}), ps_code = compile(hlsl, "ps", "ps", {define});
  if (!expect(vs_code && ps_code, "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));

  // the largest footprint at the largest bias stays inside the mips
  constexpr UINT size = 256, mips = std::bit_width(size), targets = 4;
  constexpr float depth_scale = 16, sample_bias = -1, biases[] = {-1, 0, 1};
  constexpr UINT anisotropies[] = {1, 4, D3D11_MAX_MAXANISOTROPY};

  step("make the textures");
  std::vector<std::vector<float>> texels(mips);
  D3D11_SUBRESOURCE_DATA data[mips];
  for (UINT m = 0; m < mips; m++) {
    texels[m].assign((size >> m) * (size >> m), float(m));
    data[m] = {texels[m].data(), UINT((size >> m) * sizeof(float))};
  }
  D3D11_TEXTURE2D_DESC desc{size, size, mips, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_SHADER_RESOURCE};
  ComPtr<ID3D11Texture2D> color, depth, target, staging;
  CHECK(device->CreateTexture2D(&desc, data, &color));
  desc.Format = DXGI_FORMAT_R32_TYPELESS;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
  CHECK(device->CreateTexture2D(&desc, nullptr, &depth));
  D3D11_TEXTURE2D_DESC target_desc{2 * quads, 2, 1, targets, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET};
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  target_desc.Usage = D3D11_USAGE_STAGING;
  target_desc.BindFlags = 0;
  target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &staging));

  ComPtr<ID3D11ShaderResourceView> views[2];
  D3D11_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D};
  srv.Texture2D = {0, mips};
  CHECK(device->CreateShaderResourceView(color.Get(), &srv, &views[0]));
  CHECK(device->CreateShaderResourceView(depth.Get(), &srv, &views[1]));
  for (UINT m = 0; m < mips; m++) {
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{DXGI_FORMAT_D32_FLOAT, D3D11_DSV_DIMENSION_TEXTURE2D};
    dsv_desc.Texture2D.MipSlice = m;
    ComPtr<ID3D11DepthStencilView> dsv;
    CHECK(device->CreateDepthStencilView(depth.Get(), &dsv_desc, &dsv));
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, m / depth_scale, 0);
  }
  ComPtr<ID3D11RenderTargetView> rtvs[targets];
  ID3D11RenderTargetView *bound[targets];
  for (UINT k = 0; k < targets; k++) {
    D3D11_RENDER_TARGET_VIEW_DESC rtv{DXGI_FORMAT_R32_FLOAT, D3D11_RTV_DIMENSION_TEXTURE2DARRAY};
    rtv.Texture2DArray = {0, k, 1};
    CHECK(device->CreateRenderTargetView(target.Get(), &rtv, &rtvs[k]));
    bound[k] = rtvs[k].Get();
  }
  float constants[quads * 4 + 4] = {};
  auto cb = buffer(device.Get(), sizeof(constants), D3D11_BIND_CONSTANT_BUFFER);
  if (!expect(bool(cb), "constant buffer was not created"))
    return verdict();

  ID3D11ShaderResourceView *srvs[] = {views[0].Get(), views[1].Get()};
  D3D11_VIEWPORT viewport{0, 0, float(2 * quads), 2, 0, 1};
  // the LOD's precision, and so a mip blend's: 8 fractional bits (D3D11.3 7.18.16; D3D11_MIP_LOD_FRACTIONAL_BIT_COUNT,
  // which these headers lack)
  const float tolerance = std::ldexp(1.0f, -8);
  const char *names[targets] = {"Sample", "SampleBias", "SampleCmp below", "SampleCmp above"};
  for (UINT anisotropy : anisotropies)
    for (float bias : biases) {
      step("draw with MaxAnisotropy %u, MipLODBias %g", anisotropy, bias);
      D3D11_SAMPLER_DESC sampler{D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP,
                                 D3D11_TEXTURE_ADDRESS_WRAP, bias, anisotropy, D3D11_COMPARISON_LESS, {},
                                 0, D3D11_FLOAT32_MAX};
      ComPtr<ID3D11SamplerState> samplers[2];
      CHECK(device->CreateSamplerState(&sampler, &samplers[0]));
      sampler.Filter = D3D11_FILTER_COMPARISON_ANISOTROPIC;
      CHECK(device->CreateSamplerState(&sampler, &samplers[1]));
      ID3D11SamplerState *bound_samplers[] = {samplers[0].Get(), samplers[1].Get()};
      // per quad its footprint and the references half a mip below and above the LOD a comparison expects
      for (UINT q = 0; q < quads; q++) {
        float want = lod(footprints[q], float(anisotropy), bias, mips);
        float values[] = {footprints[q][0], footprints[q][1], (want - 0.5f) / depth_scale, (want + 0.5f) / depth_scale};
        std::copy_n(values, 4, constants + 4 * q);
      }
      constants[quads * 4] = sample_bias;
      context->UpdateSubresource(cb.Get(), 0, nullptr, constants, 0, 0);
      context->VSSetShader(vs.Get(), nullptr, 0);
      context->PSSetShader(ps.Get(), nullptr, 0);
      context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
      context->PSSetShaderResources(0, 2, srvs);
      context->PSSetSamplers(0, 2, bound_samplers);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context->RSSetViewports(1, &viewport);
      context->OMSetRenderTargets(targets, bound, nullptr);
      context->Draw(3, 0);
      context->CopyResource(staging.Get(), target.Get());

      step("read MaxAnisotropy %u, MipLODBias %g", anisotropy, bias);
      for (UINT k = 0; k < targets; k++) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        CHECK(context->Map(staging.Get(), k, D3D11_MAP_READ, 0, &mapped));
        for (UINT q = 0; q < quads; q++) {
          float want = lod(footprints[q], float(anisotropy), bias, mips);
          float wants[targets] = {want, lod(footprints[q], float(anisotropy), bias + sample_bias, mips), 1, 0};
          for (UINT y = 0; y < 2; y++)
            for (UINT x = 2 * q; x < 2 * q + 2; x++) {
              float value = ((float *)((char *)mapped.pData + y * mapped.RowPitch))[x];
              expect(std::fabs(value - wants[k]) <= tolerance, "%s at pixel %u,%u (footprint %gx%g) read %g, expected %g",
                     names[k], x, y, footprints[q][0], footprints[q][1], value, wants[k]);
            }
        }
        context->Unmap(staging.Get(), k);
      }
    }
  return verdict();
}
