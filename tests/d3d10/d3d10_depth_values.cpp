// contract: depth/stencil SRV loads reinterpret packed storage and supply every missing component correctly.
// "defaults for missing components are 0 for any component except the A component, whose default is 1"
// (D3D11.3 19.1.3.3). depth occupies R, stencil occupies G (19.1.3/.4 format spelling). UNORM values are
// "divided by (2n-1)" (3.2.3.5); UINT widens without sign extension (3.2.3.12). "the bits are untouched"
// (22.4.6). CPU-packed input covers all 16-bit depth and 8-bit stencil encodings,
// wider bit/exponent boundaries and exceptional float encodings, through every mip/slice of 1D/2D arrays.
// native Direct3D 10.0/10.1 reads actual depth-bound resources; output uint targets preserve all bits.
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include "../depth_formats.hpp"
#include "../depth_values.hpp"

using format_values::Pixel;

static const char shaders[] = R"hlsl(
cbuffer Shape : register(b0) { uint mip; uint layer; };
#if DIMENSION == 1
Texture1DArray<TYPE> tex : register(t0);
#else
Texture2DArray<TYPE> tex : register(t0);
#endif
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id==2?3:-1,id==1?3:-1,0,1); }
uint4 ps(float4 position : SV_Position) : SV_Target {
#if DIMENSION == 1
  return asuint(tex.Load(int3(uint(position.x),layer,mip)));
#else
  return asuint(tex.Load(int4(uint2(position.xy),layer,mip)));
#endif
}
)hlsl";

int
main() {
  auto vertex = compile(shaders, "vs", "vs_4_0", {"DIMENSION=1", "TYPE=float4"});
  ComPtr<ID3DBlob> codes[2][2];
  for (UINT dimension = 0; dimension < std::size(codes); dimension++)
    for (UINT plane = 0; plane < std::size(codes[dimension]); plane++) {
      codes[dimension][plane] = compile(
          shaders, "ps", "ps_4_0",
          {"DIMENSION=" + std::to_string(dimension + 1), std::string("TYPE=") + (plane ? "uint4" : "float4")}
      );
      if (!expect(bool(codes[dimension][plane]), "depth-plane HLSL did not compile"))
        return verdict();
    }
  if (!expect(bool(vertex), "vertex HLSL did not compile"))
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
    ComPtr<ID3D10PixelShader> ps[2][2];
    CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), &vs));
    for (UINT dimension = 0; dimension < std::size(ps); dimension++)
      for (UINT plane = 0; plane < std::size(ps[dimension]); plane++) {
        auto code = codes[dimension][plane].Get();
        CHECK(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), &ps[dimension][plane]));
      }
    D3D10_RASTERIZER_DESC rd{D3D10_FILL_SOLID, D3D10_CULL_NONE};
    rd.DepthClipEnable = TRUE;
    ComPtr<ID3D10RasterizerState> raster;
    CHECK(device->CreateRasterizerState(&rd, &raster));
    for (const auto &f : depth_values::formats()) {
      if (!expect(
              f.depth_bits && f.parent != DXGI_FORMAT_UNKNOWN && f.planes[0] != DXGI_FORMAT_UNKNOWN &&
                  (!f.stencil_bits || f.planes[1] != DXGI_FORMAT_UNKNOWN),
              "depth family missing from SDK"
          ))
        return verdict();
      const auto depth = f.depth();
      auto corpus = depth;
      if (!f.floating && f.depth_bits > sizeof(UINT16) * CHAR_BIT)
        corpus.components.front().bits = sizeof(UINT) * CHAR_BIT;
      auto raw = format_values::inputs(corpus);
      const UINT count = UINT(raw.size() / f.bytes());
      if (f.stencil_bits && raw.size() / f.bytes() < (1u << f.stencil_bits)) {
        const auto initial = raw;
        raw.resize((1u << f.stencil_bits) * f.bytes());
        for (UINT i = 0; i < raw.size() / f.bytes(); i++)
          memcpy(raw.data() + i * f.bytes(), initial.data() + (i % count) * f.bytes(), f.bytes());
      }
      for (UINT i = 0; f.stencil_bits && i < raw.size() / f.bytes(); i++)
        raw[i * f.bytes() + f.depth_bits / CHAR_BIT] = UINT8(i);
      const UINT source_count = UINT(raw.size() / f.bytes());
      for (UINT dimension : {1u, 2u}) {
        const UINT width = dimension == 1 ? std::min<UINT>(std::bit_ceil(source_count), D3D10_REQ_TEXTURE1D_U_DIMENSION)
                                          : 1u << CHAR_BIT;
        const UINT height = dimension == 1 ? 1 : std::bit_ceil((source_count + width - 1) / width);
        const UINT slices = std::max(2u, (source_count + width * height - 1) / (width * height));
        const UINT mips = std::bit_width(width | height), subresources = mips * slices;
        std::vector<std::vector<uint8_t>> data(subresources);
        std::vector<D3D10_SUBRESOURCE_DATA> initial(subresources);
        UINT next = 0;
        for (UINT sub = 0; sub < subresources; sub++) {
          UINT wide = std::max(1u, width >> (sub % mips)), high = std::max(1u, height >> (sub % mips));
          data[sub].resize(wide * high * f.bytes());
          for (UINT i = 0; i < wide * high; i++, next++)
            memcpy(data[sub].data() + i * f.bytes(), raw.data() + (next % source_count) * f.bytes(), f.bytes());
          initial[sub] = {data[sub].data(), wide * f.bytes()};
        }
        ComPtr<ID3D10Resource> texture;
        constexpr UINT bind = D3D10_BIND_DEPTH_STENCIL | D3D10_BIND_SHADER_RESOURCE;
        if (dimension == 1) {
          D3D10_TEXTURE1D_DESC desc{width, mips, slices, f.parent, D3D10_USAGE_DEFAULT, bind};
          ComPtr<ID3D10Texture1D> typed;
          CHECK(device->CreateTexture1D(&desc, initial.data(), &typed));
          CHECK(typed.As(&texture));
        } else {
          D3D10_TEXTURE2D_DESC desc{width, height, mips, slices, f.parent, {1, 0}, D3D10_USAGE_DEFAULT, bind};
          ComPtr<ID3D10Texture2D> typed;
          CHECK(device->CreateTexture2D(&desc, initial.data(), &typed));
          CHECK(typed.As(&texture));
        }
        for (UINT plane = 0; plane < (f.stencil_bits ? 2u : 1u); plane++) {
          UINT support = 0;
          CHECK(device->CheckFormatSupport(f.planes[plane], &support));
          if (!expect(support & D3D10_FORMAT_SUPPORT_SHADER_LOAD, "required depth-plane shader load missing"))
            continue;
          D3D10_SHADER_RESOURCE_VIEW_DESC vd{
              f.planes[plane], dimension == 1 ? D3D10_SRV_DIMENSION_TEXTURE1DARRAY : D3D10_SRV_DIMENSION_TEXTURE2DARRAY
          };
          if (dimension == 1)
            vd.Texture1DArray = {0, mips, 0, slices};
          else
            vd.Texture2DArray = {0, mips, 0, slices};
          ComPtr<ID3D10ShaderResourceView> srv;
          CHECK(device->CreateShaderResourceView(texture.Get(), &vd, &srv));
          for (UINT sub = 0; sub < subresources; sub++) {
            const UINT mip = sub % mips, layer = sub / mips;
            const UINT wide = std::max(1u, width >> mip), high = std::max(1u, height >> mip);
            UINT shape[4] = {mip, layer};
            D3D10_BUFFER_DESC bd{sizeof(shape), D3D10_USAGE_IMMUTABLE, D3D10_BIND_CONSTANT_BUFFER};
            D3D10_SUBRESOURCE_DATA constants_data{shape};
            ComPtr<ID3D10Buffer> constants;
            CHECK(device->CreateBuffer(&bd, &constants_data, &constants));
            D3D10_TEXTURE2D_DESC td{
                wide, high, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET
            };
            ComPtr<ID3D10Texture2D> target, staging;
            ComPtr<ID3D10RenderTargetView> rtv;
            CHECK(device->CreateTexture2D(&td, nullptr, &target));
            CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
            td.BindFlags = 0;
            td.Usage = D3D10_USAGE_STAGING;
            td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
            CHECK(device->CreateTexture2D(&td, nullptr, &staging));
            step(
                "D3D10 level %#x %s %uD plane %u mip %u slice %u", level, f.name.c_str(), dimension, plane, mip, layer
            );
            device->ClearState();
            device->VSSetShader(vs.Get());
            device->PSSetShader(ps[dimension - 1][plane].Get());
            device->PSSetShaderResources(0, 1, srv.GetAddressOf());
            device->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
            device->RSSetState(raster.Get());
            D3D10_VIEWPORT viewport{0, 0, wide, high, 0, 1};
            device->RSSetViewports(1, &viewport);
            device->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            const float poison[] = {float(UINT_MAX), float(UINT_MAX), float(UINT_MAX), float(UINT_MAX)};
            device->ClearRenderTargetView(rtv.Get(), poison);
            device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            device->Draw(3, 0);
            device->ClearState();
            device->CopyResource(staging.Get(), target.Get());
            D3D10_MAPPED_TEXTURE2D mapped{};
            CHECK(staging->Map(0, D3D10_MAP_READ, 0, &mapped));
            bool same = true;
            for (UINT y = 0; same && y < high; y++)
              for (UINT x = 0; x < wide; x++) {
                const auto bytes = data[sub].data() + (y * wide + x) * f.bytes();
                Pixel expected =
                    plane ? Pixel{0, bytes[f.depth_bits / CHAR_BIT], 0, 1} : format_values::decode(depth, bytes);
                Pixel have;
                memcpy(
                    have.data(), static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch + x * sizeof(Pixel),
                    sizeof(have)
                );
                if (!expect(
                        have == expected, "texel %u,%u: %08x %08x %08x %08x expected %08x %08x %08x %08x", x, y,
                        have[0], have[1], have[2], have[3], expected[0], expected[1], expected[2], expected[3]
                    )) {
                  same = false;
                  break;
                }
              }
            staging->Unmap(0);
            tested++;
          }
        }
      }
    }
  }
  FreeLibrary(dll);
  expect(tested != 0, "no D3D10 depth-plane values ran");
  return verdict();
}
