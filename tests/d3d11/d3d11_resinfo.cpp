// contract: resinfo_rcpFloat "does not apply to values that are set to 0 and thus not returned, and also does not
// apply to array size returns" (D3D11.3 22.4.14). "If srcMipLevel is out of the range of the available number of
// miplevels in the resource, the behavior for the size return (dest.xyz) is identical to that of an unbound t#/u#
// resource." the mip count is still returned. missing dimensions stay zero; present zero dimensions become INF
// under rcpFloat, including for null views. cube-array z is explicitly undefined and is not compared.
// all three result encodings query every texture dimension, full and restricted views, and each mip boundary.
#include "d3d11_test.hpp"
#include "../dxbc_tokens.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>

static const char hlsl[] = R"hlsl(
TYPE<float4> texels : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
uint4 ps() : SV_Target {
  uint width, height = 0, depth = 0, levels;
#if KIND == 0
  texels.GetDimensions(0, width, levels);
#elif KIND == 1
  texels.GetDimensions(0, width, height, levels);
#elif KIND == 2 || KIND == 5
  texels.GetDimensions(0, width, height, levels);
#elif KIND == 7
  texels.GetDimensions(width, height, levels);
#elif KIND == 8
  texels.GetDimensions(width, height, depth, levels);
#else
  texels.GetDimensions(0, width, height, depth, levels);
#endif
  return uint4(width, height, depth, levels);
}
)hlsl";

static std::string
query(const std::string &base, UINT mip, microsoft::D3D10_SB_RESINFO_INSTRUCTION_RETURN_TYPE type)
{
  using namespace dxbc_tokens;
  return rewrite(base, [&](Words &p) {
    auto out = declarations(p, 1);
    if (out.empty())
      return false;
    auto resource = source(0);
    resource[0] &= ~D3D10_SB_OPERAND_TYPE_MASK;
    resource[0] |= ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_RESOURCE);
    instruction(out, D3D10_SB_OPCODE_RESINFO, {destination(0), immediate(mip), resource},
                ENCODE_D3D10_SB_RESINFO_INSTRUCTION_RETURN_TYPE(type));
    instruction(out, D3D10_SB_OPCODE_MOV,
                {destination(0, D3D10_SB_OPERAND_4_COMPONENT_MASK_ALL, D3D10_SB_OPERAND_TYPE_OUTPUT), source(0)});
    instruction(out, D3D10_SB_OPCODE_RET);
    p = std::move(out);
    return true;
  });
}

int
main()
{
  using namespace microsoft;
  constexpr UINT width = 16, height = width / 2, depth = height / 2, arrays = 3;
  constexpr UINT levels = std::bit_width(width), samples = 4;
  constexpr UINT cube_faces = D3D10_TEXTURECUBE_FACE_NEGATIVE_Z + 1;
  const char *types[] = {"Texture1D",   "Texture1DArray",   "Texture2D",   "Texture2DArray",  "Texture3D",
                         "TextureCube", "TextureCubeArray", "Texture2DMS", "Texture2DMSArray"};
  struct Shader {
    UINT kind, mip;
    D3D10_SB_RESINFO_INSTRUCTION_RETURN_TYPE type;
    std::string code;
  };
  std::vector<Shader> shaders;
  auto vs_code = compile(hlsl, "vs", "vs", {"TYPE=Texture2D", "KIND=2"});
  if (!expect(!!vs_code, "resinfo vertex shader compiled"))
    return verdict();
  for (UINT kind = 0; kind < std::size(types); kind++) {
    auto seed = compile(hlsl, "ps", "ps", {std::string("TYPE=") + types[kind], "KIND=" + std::to_string(kind)});
    if (!expect(!!seed, "%s fixture compiled", types[kind]))
      return verdict();
    const std::string base(static_cast<const char *>(seed->GetBufferPointer()), seed->GetBufferSize());
    for (UINT mip : {0u, 1u, levels - 1, levels, UINT32_MAX})
      for (auto type : {D3D10_SB_RESINFO_INSTRUCTION_RETURN_FLOAT, D3D10_SB_RESINFO_INSTRUCTION_RETURN_UINT,
                        D3D10_SB_RESINFO_INSTRUCTION_RETURN_RCPFLOAT}) {
        auto code = query(base, mip, type);
        if (!expect(!code.empty(), "%s resinfo instructions generated", types[kind]))
          return verdict();
        shaders.push_back({kind, mip, type, std::move(code)});
      }
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  D3D11_TEXTURE2D_DESC target_desc{
      1, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  target_desc.Usage = D3D11_USAGE_STAGING;
  target_desc.BindFlags = 0;
  target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &staging));
  D3D11_RASTERIZER_DESC raster{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> rasterizer;
  CHECK(device->CreateRasterizerState(&raster, &rasterizer));
  D3D11_DEPTH_STENCIL_DESC depth_desc{};
  depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  ComPtr<ID3D11DepthStencilState> depth_state;
  CHECK(device->CreateDepthStencilState(&depth_desc, &depth_state));
  for (UINT kind = 0; kind < std::size(types); kind++) {
    const bool ms = kind >= 7, cube = kind == 5 || kind == 6, array = kind == 1 || kind == 3 || kind == 6 || kind == 8;
    const UINT mip_count = ms ? 1 : levels;
    ComPtr<ID3D11Resource> resource;
    if (kind <= 1) {
      D3D11_TEXTURE1D_DESC desc{
          width, mip_count, array ? arrays : 1, DXGI_FORMAT_R32_FLOAT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
      ComPtr<ID3D11Texture1D> texture;
      CHECK(device->CreateTexture1D(&desc, nullptr, &texture));
      CHECK(texture.As(&resource));
    } else if (kind == 4) {
      D3D11_TEXTURE3D_DESC desc{
          width, height, depth, mip_count, DXGI_FORMAT_R32_FLOAT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
      ComPtr<ID3D11Texture3D> texture;
      CHECK(device->CreateTexture3D(&desc, nullptr, &texture));
      CHECK(texture.As(&resource));
    } else {
      if (ms) {
        UINT qualities;
        CHECK(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R32_FLOAT, samples, &qualities));
        if (!expect(qualities != 0, "required 4x R32_FLOAT support"))
          continue;
      }
      D3D11_TEXTURE2D_DESC desc{width,
                                cube ? width : height,
                                mip_count,
                                (array ? arrays : 1) * (cube ? cube_faces : 1),
                                DXGI_FORMAT_R32_FLOAT,
                                {ms ? samples : 1, 0},
                                D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE,
                                0,
                                cube ? UINT(D3D11_RESOURCE_MISC_TEXTURECUBE) : 0};
      ComPtr<ID3D11Texture2D> texture;
      CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
      CHECK(texture.As(&resource));
    }
    ComPtr<ID3D11ShaderResourceView> full;
    CHECK(device->CreateShaderResourceView(resource.Get(), nullptr, &full));
    for (UINT view = 0; view < 3; view++) {
      ComPtr<ID3D11ShaderResourceView> srv;
      const UINT first_mip = view == 1 && !ms ? 1 : 0, array_count = array ? arrays - (view == 1) : 1;
      if (view == 0)
        srv = full;
      else if (view == 1) {
        D3D11_SHADER_RESOURCE_VIEW_DESC desc;
        full->GetDesc(&desc);
        switch (desc.ViewDimension) {
        case D3D11_SRV_DIMENSION_TEXTURE1D:
          desc.Texture1D = {first_mip, mip_count - first_mip};
          break;
        case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
          desc.Texture1DArray = {first_mip, mip_count - first_mip, 1, array_count};
          break;
        case D3D11_SRV_DIMENSION_TEXTURE2D:
          desc.Texture2D = {first_mip, mip_count - first_mip};
          break;
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
          desc.Texture2DArray = {first_mip, mip_count - first_mip, 1, array_count};
          break;
        case D3D11_SRV_DIMENSION_TEXTURE3D:
          desc.Texture3D = {first_mip, mip_count - first_mip};
          break;
        case D3D11_SRV_DIMENSION_TEXTURECUBE:
          desc.TextureCube = {first_mip, mip_count - first_mip};
          break;
        case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
          desc.TextureCubeArray = {first_mip, mip_count - first_mip, cube_faces, array_count};
          break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
          desc.Texture2DMSArray = {1, array_count};
          break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMS:
          break;
        default:
          expect(false, "unexpected view dimension %u", desc.ViewDimension);
          return verdict();
        }
        CHECK(device->CreateShaderResourceView(resource.Get(), &desc, &srv));
      }
      for (auto &s : shaders) {
        if (s.kind != kind)
          continue;
        step("create %s resinfo type %u mip %u view %u", types[kind], s.type, s.mip, view);
        ComPtr<ID3D11PixelShader> ps;
        CHECK(device->CreatePixelShader(s.code.data(), s.code.size(), nullptr, &ps));
        const bool bound = view != 2, valid_mip = bound && s.mip < mip_count - first_mip;
        const auto dimension = [&](UINT size) { return valid_mip ? std::max(1u, size >> (s.mip + first_mip)) : 0u; };
        std::array<UINT, 4> sizes{dimension(width), 0, 0, bound ? mip_count - first_mip : 0};
        const UINT dimensions = kind <= 1 ? 1 : kind == 4 ? 3 : 2;
        if (dimensions >= 2)
          sizes[1] = dimension(cube ? width : height);
        if (dimensions == 3)
          sizes[2] = dimension(depth);
        if (array)
          sizes[dimensions] = valid_mip ? array_count : 0;
        for (auto *context : {immediate.Get(), deferred.Get()}) {
          step("%s type %u mip %u view %u, %s", types[kind], s.type, s.mip, view,
               context == immediate.Get() ? "immediate" : "deferred");
          const float clear[4] = {7, 7, 7, 7};
          const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
          context->ClearRenderTargetView(rtv.Get(), clear);
          context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
          context->OMSetDepthStencilState(depth_state.Get(), 0);
          context->RSSetState(rasterizer.Get());
          context->RSSetViewports(1, &viewport);
          context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          context->VSSetShader(vs.Get(), nullptr, 0);
          context->PSSetShader(ps.Get(), nullptr, 0);
          context->PSSetShaderResources(0, 1, srv.GetAddressOf());
          context->Draw(3, 0);
          context->ClearState();
          if (context == deferred.Get()) {
            ComPtr<ID3D11CommandList> commands;
            CHECK(deferred->FinishCommandList(FALSE, &commands));
            immediate->ExecuteCommandList(commands.Get(), FALSE);
          }
          immediate->CopyResource(staging.Get(), target.Get());
          D3D11_MAPPED_SUBRESOURCE mapped;
          CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
          const auto got = static_cast<const uint32_t *>(mapped.pData);
          for (unsigned c = 0; c < sizes.size(); c++) {
            if (kind == 6 && c == 2)
              continue;
            float f = s.type == D3D10_SB_RESINFO_INSTRUCTION_RETURN_RCPFLOAT && c < dimensions
                          ? sizes[c] ? 1.0f / sizes[c] : std::numeric_limits<float>::infinity()
                          : float(sizes[c]);
            uint32_t want = s.type == D3D10_SB_RESINFO_INSTRUCTION_RETURN_UINT ? sizes[c] : std::bit_cast<uint32_t>(f);
            expect(got[c] == want, "component %u: %#x, want %#x", c, got[c], want);
          }
          immediate->Unmap(staging.Get(), 0);
        }
      }
    }
  }
  return verdict();
}
