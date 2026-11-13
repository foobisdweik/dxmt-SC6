// contract: RGB32 texture transfers retain all three components, including padded rows and slices.
// "later components are still located at larger addresses than earlier components." (D3D11.3 19.1.3.1).
// "The default value for missing components in an Element format is "0" for any component except A, which gets
// "1"." (D3D11.3 19.1.3.3). Microsoft Learn, Format support for Direct3D Feature Level 10.0 hardware, requires
// R32G32B32_FLOAT/UINT/SINT textures, Buffer, Shader ld and CPU Lockable; TYPELESS requires texture storage. an
// out-of-bounds ld "returns 0 in all components present in the format of the resource, and the defaults (0,0,0,1)"
// (D3D11.3 22.4.7, ld2dms). typed unordered access to R32G32B32 is not available (the 11.0 table).
#include "d3d11_test.hpp"
#include <algorithm>
#include <array>
#include <bit>

int main() {
  const char vs[] = "float4 main(uint id:SV_VertexID):SV_Position {"
                    "return float4(id==2?3:-1,id==1?3:-1,0,1);}";
  const char ps[] = R"hlsl(
cbuffer Layout : register(b0) { uint width; };
#ifdef BUFFER_LOAD
Buffer<TYPE> tex : register(t0);
float4 main(float4 p:SV_Position):SV_Target { return float4(tex.Load(uint(p.x) + uint(p.y) * width)); }
#else
Texture2D<TYPE> tex : register(t0);
float4 main(float4 p:SV_Position):SV_Target { return float4(tex.Load(int3(p.xy,0))); }
#endif
)hlsl";
  ComPtr<ID3DBlob> vertex, pixels[2][3], errors;
  auto shader = [&](const char *text, const char *profile, const D3D_SHADER_MACRO *macros, ComPtr<ID3DBlob> &code) {
    errors.Reset();
    auto hr = D3DCompile(text, strlen(text), nullptr, macros, nullptr, "main", profile, 0, 0, &code, &errors);
    if (FAILED(hr) && errors)
      printf("%.*s\n", int(errors->GetBufferSize()), (const char *)errors->GetBufferPointer());
    return hr;
  };
  CHECK(shader(vs, "vs_4_0", nullptr, vertex));
  const char *types[] = {"float4", "uint4", "int4"};
  for (UINT buffer_load = 0; buffer_load < std::size(pixels); buffer_load++)
    for (UINT i = 0; i < std::size(types); i++) {
      D3D_SHADER_MACRO macros[] = {{"TYPE", types[i]}, {buffer_load ? "BUFFER_LOAD" : "TEXTURE_LOAD", "1"}, {}};
      CHECK(shader(ps, "ps_4_0", macros, pixels[buffer_load][i]));
    }
  for (auto level : {D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_11_0}) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context, deferred;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                            nullptr, &context));
    CHECK(device->CreateDeferredContext(0, &deferred));
    ComPtr<ID3D11VertexShader> vertex_shader;
    CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vertex_shader));
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32_SINT,
                                   DXGI_FORMAT_R32G32B32_TYPELESS};
    for (auto format : formats) {
      UINT support = 0;
      CHECK(device->CheckFormatSupport(format, &support));
      const UINT storage = D3D11_FORMAT_SUPPORT_TEXTURE1D | D3D11_FORMAT_SUPPORT_TEXTURE2D |
                           D3D11_FORMAT_SUPPORT_TEXTURE3D | D3D11_FORMAT_SUPPORT_TEXTURECUBE |
                           D3D11_FORMAT_SUPPORT_MIP | D3D11_FORMAT_SUPPORT_CPU_LOCKABLE |
                           D3D11_FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT;
      const UINT typed = D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER |
                         D3D11_FORMAT_SUPPORT_SO_BUFFER | D3D11_FORMAT_SUPPORT_SHADER_LOAD;
      expect((support & storage) == storage, "required RGB32 storage support missing");
      expect(format == DXGI_FORMAT_R32G32B32_TYPELESS ? !(support & typed) : (support & typed) == typed,
             "RGB32 typed support disagrees with the format table");
      expect(!(support & (D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL |
                          D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON | D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER)),
             "RGB32 advertises a disallowed operation");
      for (UINT dimension : {1u, 2u, 3u})
        for (bool cube : {false, true}) {
          if (cube && dimension != 2)
            continue;
          const UINT width = 5, height = dimension == 1 ? 1 : cube ? width : 3;
          const UINT depth = dimension == 3 ? 3 : 1, slices = dimension == 3 ? 1 : cube ? 6 : 2;
          const UINT mips = std::bit_width(width | height | depth), texel = sizeof(std::array<UINT, 3>);
          std::vector<std::vector<uint8_t>> bytes(mips * slices);
          std::vector<D3D11_SUBRESOURCE_DATA> data(mips * slices);
          for (UINT slice = 0; slice < slices; slice++)
            for (UINT mip = 0; mip < mips; mip++) {
              const UINT sub = D3D11CalcSubresource(mip, slice, mips);
              const UINT row = std::max(1u, width >> mip) * texel + sizeof(UINT);
              const UINT image = row * std::max(1u, height >> mip) + sizeof(UINT);
              bytes[sub].resize(image * std::max(1u, depth >> mip));
              for (UINT b = 0; b < bytes[sub].size(); b++)
                bytes[sub][b] = uint8_t(b * 29 + sub + 17);
              data[sub] = {bytes[sub].data(), dimension == 1 ? 0 : row, image};
            }
          auto make = [&](D3D11_USAGE usage, const D3D11_SUBRESOURCE_DATA *initial, ComPtr<ID3D11Resource> &out) {
            const UINT bind = usage == D3D11_USAGE_STAGING ? 0 : D3D11_BIND_SHADER_RESOURCE;
            const UINT cpu = usage == D3D11_USAGE_STAGING   ? D3D11_CPU_ACCESS_READ
                             : usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE
                                                            : 0;
            const UINT levels = usage == D3D11_USAGE_DYNAMIC ? 1 : mips;
            const UINT arrays = usage == D3D11_USAGE_DYNAMIC ? 1 : slices;
            const UINT misc = cube && usage != D3D11_USAGE_STAGING && usage != D3D11_USAGE_DYNAMIC
                                  ? D3D11_RESOURCE_MISC_TEXTURECUBE
                                  : 0;
            if (dimension == 1) {
              D3D11_TEXTURE1D_DESC desc{width, levels, arrays, format, usage, bind, cpu};
              ComPtr<ID3D11Texture1D> texture;
              auto hr = device->CreateTexture1D(&desc, initial, &texture);
              if (SUCCEEDED(hr))
                hr = texture.As(&out);
              return hr;
            }
            if (dimension == 2) {
              D3D11_TEXTURE2D_DESC desc{width, height, levels, arrays, format, {1, 0}, usage, bind, cpu, misc};
              ComPtr<ID3D11Texture2D> texture;
              auto hr = device->CreateTexture2D(&desc, initial, &texture);
              if (SUCCEEDED(hr))
                hr = texture.As(&out);
              return hr;
            }
            D3D11_TEXTURE3D_DESC desc{width, height, depth, levels, format, usage, bind, cpu};
            ComPtr<ID3D11Texture3D> texture;
            auto hr = device->CreateTexture3D(&desc, initial, &texture);
            if (SUCCEEDED(hr))
              hr = texture.As(&out);
            return hr;
          };
          ComPtr<ID3D11Resource> texture, staging, other;
          CHECK(make(D3D11_USAGE_DEFAULT, data.data(), texture));
          CHECK(make(D3D11_USAGE_DEFAULT, nullptr, other));
          CHECK(make(D3D11_USAGE_STAGING, nullptr, staging));
          for (UINT operation = 0; operation < 3; operation++) {
            step("RGB32 %u dimension %u cube %u mips %u slices %u operation %u", format, dimension, cube, mips, slices,
                 operation);
            if (operation == 1)
              for (UINT sub = 0; sub < data.size(); sub++)
                context->UpdateSubresource(texture.Get(), sub, nullptr, data[sub].pSysMem, data[sub].SysMemPitch,
                                           data[sub].SysMemSlicePitch);
            if (operation == 2) {
              context->CopyResource(other.Get(), texture.Get());
              context->CopyResource(texture.Get(), other.Get());
            }
            context->CopyResource(staging.Get(), texture.Get());
            for (UINT sub = 0; sub < data.size(); sub++) {
              const UINT mip = sub % mips, wide = std::max(1u, width >> mip);
              D3D11_MAPPED_SUBRESOURCE mapped{};
              CHECK(context->Map(staging.Get(), sub, D3D11_MAP_READ, 0, &mapped));
              bool same = true;
              for (UINT z = 0; z < std::max(1u, depth >> mip); z++)
                for (UINT y = 0; y < std::max(1u, height >> mip); y++)
                  same &= !memcmp((char *)mapped.pData + z * mapped.DepthPitch + y * mapped.RowPitch,
                                  bytes[sub].data() + z * data[sub].SysMemSlicePitch + y * data[sub].SysMemPitch,
                                  wide * texel);
              context->Unmap(staging.Get(), sub);
              if (!expect(same, "RGB32 subresource %u lost bytes", sub))
                return verdict();
            }
          }
          if (!cube)
            for (bool listed : {false, true}) {
              ComPtr<ID3D11Resource> dynamic;
              CHECK(make(D3D11_USAGE_DYNAMIC, data.data(), dynamic));
              auto recording = listed ? deferred.Get() : context.Get();
              auto expected = bytes[0];
              for (UINT discard = 0; discard < 3; discard++) {
                step("RGB32 %u dimension %u list %u initial or discard %u", format, dimension, listed, discard);
                if (discard) {
                  for (auto &byte : expected)
                    byte = uint8_t(byte + discard);
                  D3D11_MAPPED_SUBRESOURCE mapped{};
                  CHECK(recording->Map(dynamic.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
                  for (UINT z = 0; z < depth; z++)
                    for (UINT y = 0; y < height; y++)
                      memcpy((char *)mapped.pData + z * mapped.DepthPitch + y * mapped.RowPitch,
                             expected.data() + z * data[0].SysMemSlicePitch + y * data[0].SysMemPitch, width * texel);
                  recording->Unmap(dynamic.Get(), 0);
                }
                recording->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, dynamic.Get(), 0, nullptr);
                if (listed) {
                  ComPtr<ID3D11CommandList> commands;
                  CHECK(deferred->FinishCommandList(FALSE, &commands));
                  context->ExecuteCommandList(commands.Get(), FALSE);
                }
                D3D11_MAPPED_SUBRESOURCE mapped{};
                CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                bool same = true;
                for (UINT z = 0; z < depth; z++)
                  for (UINT y = 0; y < height; y++)
                    same &= !memcmp((char *)mapped.pData + z * mapped.DepthPitch + y * mapped.RowPitch,
                                    expected.data() + z * data[0].SysMemSlicePitch + y * data[0].SysMemPitch,
                                    width * texel);
                context->Unmap(staging.Get(), 0);
                if (!expect(same, "dynamic RGB32 bytes changed"))
                  return verdict();
              }
            }
        }
    }
    for (UINT kind = 0; kind < std::size(formats); kind++)
      for (UINT width : {1u, 5u, 31u, 32u, 33u})
        for (UINT padding : {0u, UINT(sizeof(UINT))})
          for (bool listed : {false, true}) {
            const UINT height = 3, texel = sizeof(std::array<UINT, 3>), pitch = width * texel + padding;
            std::vector<uint8_t> bytes(pitch * height), expected(width * texel * height);
            for (UINT y = 0; y < height; y++)
              for (UINT x = 0; x < width; x++)
                for (UINT c = 0; c < 3; c++) {
                  UINT value = 1 + x + y * width + c;
                  if (kind == 0)
                    value = std::bit_cast<UINT>(float(value));
                  if (kind == 2)
                    value = UINT(-int(value));
                  memcpy(bytes.data() + y * pitch + x * texel + c * sizeof(UINT), &value, sizeof(value));
                  memcpy(expected.data() + (y * width + x) * texel + c * sizeof(UINT), &value, sizeof(value));
                }
            auto run = listed ? deferred.Get() : context.Get();
            auto execute = [&] {
              if (!listed)
                return S_OK;
              ComPtr<ID3D11CommandList> commands;
              auto hr = deferred->FinishCommandList(FALSE, &commands);
              if (SUCCEEDED(hr))
                context->ExecuteCommandList(commands.Get(), FALSE);
              return hr;
            };
            D3D11_TEXTURE2D_DESC desc{
                width, height, 1, 1, formats[kind], {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
            D3D11_SUBRESOURCE_DATA data{bytes.data(), pitch};
            ComPtr<ID3D11Texture2D> texture, staging, other;
            CHECK(device->CreateTexture2D(&desc, &data, &texture));
            CHECK(device->CreateTexture2D(&desc, nullptr, &other));
            auto staging_desc = desc;
            staging_desc.Usage = D3D11_USAGE_STAGING;
            staging_desc.BindFlags = 0;
            staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            CHECK(device->CreateTexture2D(&staging_desc, nullptr, &staging));
            auto check = [&] {
              run->CopyResource(staging.Get(), texture.Get());
              const auto submitted = execute();
              if (!expect(SUCCEEDED(submitted), "command list failed: %#x", unsigned(submitted)))
                return false;
              D3D11_MAPPED_SUBRESOURCE mapped{};
              const auto mapped_hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
              if (!expect(SUCCEEDED(mapped_hr), "readback map failed: %#x", unsigned(mapped_hr)))
                return false;
              bool same = true;
              for (UINT y = 0; y < height; y++)
                same &= !memcmp((const char *)mapped.pData + y * mapped.RowPitch, expected.data() + y * width * texel,
                                width * texel);
              context->Unmap(staging.Get(), 0);
              return expect(same, "RGB bytes changed");
            };
            step("RGB32 FL %x format %u %ux%u padding %u list %u initial", level, formats[kind], width, height, padding,
                 listed);
            if (!check())
              return verdict();
            for (UINT operation = 0; operation < 5; operation++) {
              step("RGB32 FL %x format %u width %u padding %u list %u operation %u", level, formats[kind], width,
                   padding, listed, operation);
              if (operation == 0) {
                const D3D11_BOX box{width - 1, 1, 0, width, height, 1};
                run->UpdateSubresource(texture.Get(), 0, &box, bytes.data() + (width - 1) * texel, pitch, 0);
                for (UINT y = 1; y < height; y++)
                  memcpy(expected.data() + (y * width + width - 1) * texel,
                         bytes.data() + (y - 1) * pitch + (width - 1) * texel, texel);
              } else if (operation == 1) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                CHECK(context->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped));
                for (UINT y = 0; y < height; y++)
                  memcpy((char *)mapped.pData + y * mapped.RowPitch, bytes.data() + y * pitch, width * texel);
                context->Unmap(staging.Get(), 0);
                run->CopyResource(texture.Get(), staging.Get());
                for (UINT y = 0; y < height; y++)
                  memcpy(expected.data() + y * width * texel, bytes.data() + y * pitch, width * texel);
              } else if (operation == 2) {
                run->CopyResource(other.Get(), texture.Get());
                run->CopyResource(texture.Get(), other.Get());
              } else if (operation == 3) {
                const D3D11_BOX box{0, 0, 0, width, height - 1, 1};
                run->CopySubresourceRegion(other.Get(), 0, 0, 1, 0, texture.Get(), 0, &box);
                run->CopyResource(texture.Get(), other.Get());
                memmove(expected.data() + width * texel, expected.data(), (height - 1) * width * texel);
              } else {
                auto dynamic_desc = desc;
                dynamic_desc.Usage = D3D11_USAGE_DYNAMIC;
                dynamic_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                ComPtr<ID3D11Texture2D> dynamic;
                CHECK(device->CreateTexture2D(&dynamic_desc, &data, &dynamic));
                run->CopyResource(texture.Get(), dynamic.Get());
                for (UINT y = 0; y < height; y++)
                  memcpy(expected.data() + y * width * texel, bytes.data() + y * pitch, width * texel);
                if (!check())
                  return verdict();
                for (UINT discard = 0; discard < 2; discard++) {
                  for (UINT i = 0; i < expected.size() / sizeof(UINT); i++) {
                    UINT value = 1 + i + (discard + 1) * width * height;
                    if (kind == 0)
                      value = std::bit_cast<UINT>(float(value));
                    if (kind == 2)
                      value = UINT(-int(value));
                    memcpy(expected.data() + i * sizeof(value), &value, sizeof(value));
                  }
                  D3D11_MAPPED_SUBRESOURCE mapped{};
                  CHECK(run->Map(dynamic.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
                  for (UINT y = 0; y < height; y++)
                    memcpy((char *)mapped.pData + y * mapped.RowPitch, expected.data() + y * width * texel,
                           width * texel);
                  run->Unmap(dynamic.Get(), 0);
                  run->CopyResource(texture.Get(), dynamic.Get());
                  if (!check())
                    return verdict();
                }
              }
              if (!check())
                return verdict();
            }
            for (bool buffer_load : {false, true}) {
              D3D11_SHADER_RESOURCE_VIEW_DESC view{formats[kind == 3 ? 1 : kind], D3D11_SRV_DIMENSION_TEXTURE2D};
              view.Texture2D.MipLevels = 1;
              ComPtr<ID3D11ShaderResourceView> srv;
              ComPtr<ID3D11Buffer> typed_buffer;
              if (buffer_load) {
                // one element before the view's first and one after its last, which it must not read
                std::vector<uint8_t> elements(texel, 0xa5);
                elements.insert(elements.end(), expected.begin(), expected.end());
                elements.insert(elements.end(), texel, 0x5a);
                D3D11_BUFFER_DESC buffer_desc{UINT(elements.size()), D3D11_USAGE_DEFAULT,
                                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS};
                D3D11_SUBRESOURCE_DATA buffer_data{elements.data()};
                CHECK(device->CreateBuffer(&buffer_desc, &buffer_data, &typed_buffer));
                view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                view.Buffer = {1, width * height};
                CHECK(device->CreateShaderResourceView(typed_buffer.Get(), &view, &srv));
                D3D11_UNORDERED_ACCESS_VIEW_DESC typed_uav{view.Format, D3D11_UAV_DIMENSION_BUFFER};
                typed_uav.Buffer = {1, width * height};
                ComPtr<ID3D11UnorderedAccessView> uav;
                const auto refused = device->CreateUnorderedAccessView(typed_buffer.Get(), &typed_uav, &uav);
                expect(refused == E_INVALIDARG && !uav, "RGB32 typed UAV returned %#x", unsigned(refused));
              } else {
                CHECK(device->CreateShaderResourceView(texture.Get(), &view, &srv));
              }
              step("shader ld format %u width %u buffer %u", view.Format, width, buffer_load);
              // a buffer's view is read one row past its elements
              const UINT rows = height + buffer_load;
              ComPtr<ID3D11PixelShader> pixel_shader;
              auto &code = pixels[buffer_load][kind == 3 ? 1 : kind];
              CHECK(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &pixel_shader));
              auto output_desc = desc;
              output_desc.Height = rows;
              output_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
              output_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
              ComPtr<ID3D11Texture2D> output, readback;
              CHECK(device->CreateTexture2D(&output_desc, nullptr, &output));
              ComPtr<ID3D11RenderTargetView> rtv;
              CHECK(device->CreateRenderTargetView(output.Get(), nullptr, &rtv));
              output_desc.Usage = D3D11_USAGE_STAGING;
              output_desc.BindFlags = 0;
              output_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
              CHECK(device->CreateTexture2D(&output_desc, nullptr, &readback));
              D3D11_VIEWPORT viewport{0, 0, float(width), float(rows), 0, 1};
              auto render = rtv.Get();
              auto resource = srv.Get();
              D3D11_RASTERIZER_DESC raster{};
              raster.FillMode = D3D11_FILL_SOLID;
              raster.CullMode = D3D11_CULL_NONE;
              raster.DepthClipEnable = TRUE;
              ComPtr<ID3D11RasterizerState> raster_state;
              CHECK(device->CreateRasterizerState(&raster, &raster_state));
              context->RSSetState(raster_state.Get());
              context->RSSetViewports(1, &viewport);
              context->OMSetRenderTargets(1, &render, nullptr);
              context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
              context->VSSetShader(vertex_shader.Get(), nullptr, 0);
              context->PSSetShader(pixel_shader.Get(), nullptr, 0);
              std::array<UINT, 4> layout{width};
              D3D11_BUFFER_DESC layout_desc{sizeof(layout), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER};
              D3D11_SUBRESOURCE_DATA layout_data{layout.data()};
              ComPtr<ID3D11Buffer> layout_buffer;
              CHECK(device->CreateBuffer(&layout_desc, &layout_data, &layout_buffer));
              auto constants = layout_buffer.Get();
              context->PSSetConstantBuffers(0, 1, &constants);
              context->PSSetShaderResources(0, 1, &resource);
              context->Draw(3, 0);
              context->CopyResource(readback.Get(), output.Get());
              D3D11_MAPPED_SUBRESOURCE mapped{};
              CHECK(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
              bool same = true;
              for (UINT y = 0; y < rows; y++)
                for (UINT x = 0; x < width; x++) {
                  float value[4];
                  memcpy(value, (const char *)mapped.pData + y * mapped.RowPitch + x * sizeof(value), sizeof(value));
                  for (UINT c = 0; c < 4; c++) {
                    UINT word = 0;
                    if (c < 3 && y < height)
                      memcpy(&word, expected.data() + (y * width + x) * texel + c * sizeof(word), sizeof(word));
                    const float want = c == 3      ? 1.0f
                                       : kind == 0 ? std::bit_cast<float>(word)
                                       : kind == 2 ? float(INT(word))
                                                   : float(word);
                    same &= value[c] == want;
                  }
                }
              context->Unmap(readback.Get(), 0);
              if (!expect(same, "ld did not return RGB and default alpha"))
                return verdict();
              context->ClearState();
            }
          }
  }
  return verdict();
}
