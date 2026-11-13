// contract: Direct3D 10/10.1 typed texture/buffer reads and render targets obey
// the format conversion rules. "The starting n-bit value is converted to float
// (0.0f, 1.0f, 2.0f, etc.) and then divided by (2n-1)." (D3D11.3 3.2.3.5,
// inherited Direct3D 10 conversion). "Round-to-zero must be used during
// conversion to another float format." (3.2.2). normalized output permits "0.6f
// ULP" (3.2.3.4/.6/.8); sRGB decode permits "0.5f ULP" on the encoded side
// (3.2.3.7). SDK-derived layouts and CPU fields cover complete 8/16-bit
// domains, every packed 16-bit word, small float codes and RGB9E5
// exponent/mantissa pairs. shader-model 4.0 bytecode reaches the actual
// D3D10CreateDevice1 entry point at both feature levels, with integer render
// targets preserving read result bits.
// "the input and the output data must be identical in memory" (19.1.2): same-format
// Load/point/center-linear copies include the SNORM minimum and NaN exceptions (19.1.2.1).
// vertex fetch uses the same component domains, padded strides and offsets, then an out-of-bounds vertex:
// "the return is 0 in all non-missing components of the format" (8.19.2), with 19.1.3.3 defaults.
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include "../depth_formats.hpp"
#include "../format_values.hpp"

using namespace format_values;

static const char shaders[] = R"hlsl(
cbuffer Shape : register(b0) { uint width; };
#if MODE == 0 || MODE == 1 || MODE >= 4
Texture2D<TYPE> tex : register(t0);
#else
Buffer<TYPE> tex : register(t0);
#endif
Buffer<uint4> source : register(t1);
SamplerState nearest : register(s0);
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id==2?3:-1,id==1?3:-1,0,1); }
#if MODE == 3
TYPE ps(float4 p : SV_Position) : SV_Target { return UNPACK(source[uint(p.x)+uint(p.y)*width]); }
#elif MODE == 4
TYPE ps(float4 p : SV_Position) : SV_Target { return tex.Load(int3(uint2(p.xy),0)); }
#elif MODE >= 5
TYPE ps(float4 p : SV_Position) : SV_Target {
  uint wide; uint high; tex.GetDimensions(wide,high);
  return tex.SampleLevel(nearest,p.xy/float2(wide,high),0);
}
#else
uint4 ps(float4 p : SV_Position) : SV_Target {
#if MODE == 0
  return asuint(tex.Load(int3(uint2(p.xy),0)));
#elif MODE == 1
  uint wide; uint high; tex.GetDimensions(wide,high);
  return asuint(tex.SampleLevel(nearest,p.xy/float2(wide,high),0));
#else
  return asuint(tex.Load(uint(p.x)+uint(p.y)*width));
#endif
}
#endif
)hlsl";

int
main() {
  auto vertex = compile(shaders, "vs", "vs_4_0", defines(0, 0));
  ComPtr<ID3DBlob> vertex_codes[3];
  for (UINT kind = 0; kind < std::size(vertex_codes); kind++) {
    vertex_codes[kind] = compile(vertices_hlsl, "vs", "vs_4_0", defines(kind, 0));
    if (!expect(bool(vertex_codes[kind]), "input-vertex HLSL did not compile"))
      return verdict();
  }
  ComPtr<ID3DBlob> codes[3][7];
  for (UINT kind = 0; kind < std::size(codes); kind++)
    for (UINT mode = 0; mode < std::size(codes[kind]); mode++) {
      if (kind && (mode == 1 || mode >= 5))
        continue;
      codes[kind][mode] = compile(shaders, "ps", "ps_4_0", defines(kind, mode));
      if (!expect(bool(codes[kind][mode]), "pixel HLSL did not compile"))
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
    ComPtr<ID3D10PixelShader> ps[3][7];
    CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), &vs));
    for (UINT kind = 0; kind < std::size(ps); kind++)
      for (UINT mode = 0; mode < std::size(ps[kind]); mode++)
        if (codes[kind][mode]) {
          CHECK(device->CreatePixelShader(
              codes[kind][mode]->GetBufferPointer(), codes[kind][mode]->GetBufferSize(), &ps[kind][mode]
          ));
        }
    D3D10_SAMPLER_DESC sd{
        D3D10_FILTER_MIN_MAG_MIP_POINT, D3D10_TEXTURE_ADDRESS_CLAMP, D3D10_TEXTURE_ADDRESS_CLAMP,
        D3D10_TEXTURE_ADDRESS_CLAMP
    };
    sd.MaxLOD = D3D10_FLOAT32_MAX;
    ComPtr<ID3D10SamplerState> sampler[2];
    CHECK(device->CreateSamplerState(&sd, &sampler[0]));
    sd.Filter = D3D10_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    CHECK(device->CreateSamplerState(&sd, &sampler[1]));
    D3D10_RASTERIZER_DESC rd{D3D10_FILL_SOLID, D3D10_CULL_NONE};
    rd.DepthClipEnable = TRUE;
    ComPtr<ID3D10RasterizerState> raster;
    CHECK(device->CreateRasterizerState(&rd, &raster));
    for (const auto &f : formats()) {
      UINT caps = 0;
      HRESULT support = device->CheckFormatSupport(f.id, &caps);
      step("D3D10 level %#x %s capabilities", level, f.name.c_str());
      if (required_vertex(f))
        expect(SUCCEEDED(support) && (caps & D3D10_FORMAT_SUPPORT_IA_VERTEX_BUFFER), "required vertex format missing");
      if (FAILED(support))
        continue;
      if (f.kind == Shared)
        expect(!(caps & D3D10_FORMAT_SUPPORT_RENDER_TARGET), "RGB9E5 advertises forbidden rendering (19.3.2)");
      auto bytes = inputs(f);
      if (caps & D3D10_FORMAT_SUPPORT_IA_VERTEX_BUFFER) {
        const UINT count = UINT(bytes.size() / f.bytes), output_bytes = (count + 2) * sizeof(Pixel);
        auto code = vertex_codes[f.shader_kind()].Get();
        ComPtr<ID3D10VertexShader> fetch;
        ComPtr<ID3D10GeometryShader> stream;
        CHECK(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), &fetch));
        const D3D10_SO_DECLARATION_ENTRY entry{"VALUE", 0, 0, UINT8(std::tuple_size_v<Pixel>), 0};
        CHECK(device->CreateGeometryShaderWithStreamOutput(
            code->GetBufferPointer(), code->GetBufferSize(), &entry, 1, sizeof(Pixel), &stream
        ));
        for (UINT padding : {0u, UINT(sizeof(Pixel))}) {
          const UINT stride = f.bytes + 2 * padding, offset = padding, start = 0;
          step("D3D10 level %#x %s vertex stride %u offsets %u", level, f.name.c_str(), stride, padding);
          std::vector<uint8_t> interleaved(offset + count * stride, uint8_t(~0u)), zero(f.bytes);
          for (UINT i = 0; i < count; i++)
            memcpy(interleaved.data() + offset + i * stride + padding, bytes.data() + i * f.bytes, f.bytes);
          std::vector<Pixel> poison(count + 2, Pixel{~0u, ~0u, ~0u, ~0u});
          D3D10_BUFFER_DESC bd{UINT(interleaved.size()), D3D10_USAGE_IMMUTABLE, D3D10_BIND_VERTEX_BUFFER};
          D3D10_SUBRESOURCE_DATA data{interleaved.data()};
          ComPtr<ID3D10Buffer> input, output, staging;
          CHECK(device->CreateBuffer(&bd, &data, &input));
          bd = {output_bytes, D3D10_USAGE_DEFAULT, D3D10_BIND_STREAM_OUTPUT};
          data.pSysMem = poison.data();
          CHECK(device->CreateBuffer(&bd, &data, &output));
          bd = {output_bytes, D3D10_USAGE_STAGING, 0, D3D10_CPU_ACCESS_READ};
          CHECK(device->CreateBuffer(&bd, nullptr, &staging));
          const D3D10_INPUT_ELEMENT_DESC element{"DATA", 0, f.id, 0, padding, D3D10_INPUT_PER_VERTEX_DATA, 0};
          ComPtr<ID3D10InputLayout> layout;
          CHECK(device->CreateInputLayout(&element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout));
          device->ClearState();
          device->IASetInputLayout(layout.Get());
          device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_POINTLIST);
          device->IASetVertexBuffers(0, 1, input.GetAddressOf(), &stride, &offset);
          device->VSSetShader(fetch.Get());
          device->GSSetShader(stream.Get());
          device->SOSetTargets(1, output.GetAddressOf(), &start);
          device->Draw(count + 1, 0);
          device->ClearState();
          device->CopyResource(staging.Get(), output.Get());
          void *mapped;
          CHECK(staging->Map(D3D10_MAP_READ, 0, &mapped));
          bool same = true;
          for (UINT i = 0; i <= count; i++) {
            Pixel value;
            memcpy(value.data(), static_cast<uint8_t *>(mapped) + i * sizeof(Pixel), sizeof(Pixel));
            if (!loaded(f, i == count ? zero.data() : bytes.data() + i * f.bytes, value)) {
              expect(false, "vertex %u conversion or buffer-end default differs", i);
              same = false;
              break;
            }
          }
          same &= expect(
              !memcmp(
                  static_cast<uint8_t *>(mapped) + (count + 1) * sizeof(Pixel), poison.back().data(), sizeof(Pixel)
              ),
              "stream output wrote beyond the draw"
          );
          staging->Unmap();
          if (!same)
            return verdict();
          tested++;
        }
      }
      if (!(caps & D3D10_FORMAT_SUPPORT_TEXTURE2D))
        continue;
      auto values = sources(f, bytes);
      const UINT width = 1u << CHAR_BIT, count = UINT(values.size()),
                 height = std::bit_ceil((count + width - 1) / width);
      const UINT pixels_count = width * height, kind = f.shader_kind();
      bytes.resize(pixels_count * f.bytes);
      values.resize(pixels_count);
      for (UINT i = count; i < pixels_count; i++)
        values[i] = decode(f, bytes.data() + i * f.bytes);
      auto filtered_bytes = linear_inputs(f, bytes);
      auto inverse_bytes = bytes;
      for (auto &byte : inverse_bytes)
        byte = ~byte;
      const UINT shape[4] = {width};
      D3D10_BUFFER_DESC bd{sizeof(shape), D3D10_USAGE_IMMUTABLE, D3D10_BIND_CONSTANT_BUFFER};
      D3D10_SUBRESOURCE_DATA data{shape};
      ComPtr<ID3D10Buffer> constants, source;
      CHECK(device->CreateBuffer(&bd, &data, &constants));
      bd = {UINT(values.size() * sizeof(Pixel)), D3D10_USAGE_IMMUTABLE, D3D10_BIND_SHADER_RESOURCE};
      data.pSysMem = values.data();
      CHECK(device->CreateBuffer(&bd, &data, &source));
      D3D10_SHADER_RESOURCE_VIEW_DESC sv{DXGI_FORMAT_R32G32B32A32_UINT, D3D10_SRV_DIMENSION_BUFFER};
      sv.Buffer.NumElements = pixels_count;
      ComPtr<ID3D10ShaderResourceView> source_srv;
      CHECK(device->CreateShaderResourceView(source.Get(), &sv, &source_srv));
      for (UINT mode = 0; mode < std::size(ps[kind]); mode++) {
        if (f.kind == Shared && mode >= 3)
          continue;
        if ((mode == 0 || mode == 4) && !(caps & D3D10_FORMAT_SUPPORT_SHADER_LOAD))
          continue;
        if ((mode == 1 || mode >= 5) && (kind || !(caps & D3D10_FORMAT_SUPPORT_SHADER_SAMPLE)))
          continue;
        if (mode == 2 && !(caps & D3D10_FORMAT_SUPPORT_BUFFER))
          continue;
        if (mode >= 3 && !(caps & D3D10_FORMAT_SUPPORT_RENDER_TARGET))
          continue;
        step("D3D10 level %#x %s mode %u, %u values", level, f.name.c_str(), mode, pixels_count);
        device->ClearState();
        ComPtr<ID3D10ShaderResourceView> srv;
        ComPtr<ID3D10Texture2D> texture;
        ComPtr<ID3D10Buffer> typed;
        if (mode == 2) {
          bd = {UINT(bytes.size()), D3D10_USAGE_IMMUTABLE, D3D10_BIND_SHADER_RESOURCE};
          data = {bytes.data()};
          CHECK(device->CreateBuffer(&bd, &data, &typed));
          sv.Format = f.id;
          CHECK(device->CreateShaderResourceView(typed.Get(), &sv, &srv));
        } else if (mode < 3 || mode >= 4) {
          D3D10_TEXTURE2D_DESC td{width, height, 1, 1, f.id, {1, 0}, D3D10_USAGE_IMMUTABLE, D3D10_BIND_SHADER_RESOURCE};
          data = {(mode == 6 ? filtered_bytes : bytes).data(), width * f.bytes};
          CHECK(device->CreateTexture2D(&td, &data, &texture));
          CHECK(device->CreateShaderResourceView(texture.Get(), nullptr, &srv));
        }
        D3D10_TEXTURE2D_DESC td{
            width,
            height,
            1,
            1,
            mode >= 3 ? f.id : DXGI_FORMAT_R32G32B32A32_UINT,
            {1, 0},
            D3D10_USAGE_DEFAULT,
            D3D10_BIND_RENDER_TARGET
        };
        ComPtr<ID3D10Texture2D> target, staging;
        ComPtr<ID3D10RenderTargetView> rtv;
        D3D10_SUBRESOURCE_DATA inverse_initial{inverse_bytes.data(), width * f.bytes};
        CHECK(device->CreateTexture2D(&td, mode >= 4 ? &inverse_initial : nullptr, &target));
        CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        td.Usage = D3D10_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        CHECK(device->CreateTexture2D(&td, nullptr, &staging));
        device->VSSetShader(vs.Get());
        device->PSSetShader(ps[kind][mode].Get());
        ID3D10ShaderResourceView *srvs[] = {srv.Get(), source_srv.Get()};
        device->PSSetShaderResources(0, UINT(std::size(srvs)), srvs);
        device->PSSetSamplers(0, 1, sampler[mode == 6].GetAddressOf());
        device->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        device->RSSetState(raster.Get());
        D3D10_VIEWPORT viewport{0, 0, width, height, 0, 1};
        device->RSSetViewports(1, &viewport);
        device->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        const float poison[] = {-1, -1, -1, -1};
        if (mode < 4)
          device->ClearRenderTargetView(rtv.Get(), poison);
        device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        device->Draw(3, 0);
        device->ClearState();
        device->CopyResource(staging.Get(), target.Get());
        D3D10_MAPPED_TEXTURE2D mapped{};
        CHECK(staging->Map(0, D3D10_MAP_READ, 0, &mapped));
        bool same = true;
        for (UINT i = 0; i < pixels_count; i++) {
          const auto *at = static_cast<const uint8_t *>(mapped.pData) + (i / width) * mapped.RowPitch;
          if (mode >= 4)
            same = inverted(
                f, (mode == 6 ? filtered_bytes : bytes).data() + i * f.bytes, at + (i % width) * f.bytes, mode >= 5
            );
          else if (mode == 3)
            same = stored(f, values[i], at + (i % width) * f.bytes);
          else {
            Pixel have;
            memcpy(have.data(), at + (i % width) * sizeof(Pixel), sizeof(Pixel));
            same = loaded(f, bytes.data() + i * f.bytes, have, mode == 1);
          }
          if (!same) {
            expect(false, "value %u does not match the CPU conversion", i);
            break;
          }
        }
        if (same && mode == 3)
          same = expect(
              ordered(f, values, static_cast<const uint8_t *>(mapped.pData), width, mapped.RowPitch),
              "normalized conversion decreases or maps equal values differently"
          );
        staging->Unmap(0);
        tested++;
        if (!same)
          return verdict();
      }
    }
  }
  FreeLibrary(dll);
  expect(tested != 0, "no D3D10 format operations ran");
  return verdict();
}
