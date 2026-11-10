// contract: D3D11.3 19.2.4: "Standard sample patterns are defined for the following sample counts:
// 1(trivial), 2, 4, 8, 16." chapter 16 adds sample-frequency execution and output coverage in D3D10.1;
// input coverage begins at D3D11. ../raster_samples.hpp gives the ordered positions and independent coverage.
// every stored sample is copied by a PS4.1 load to a single-sample target, retaining its identity on readback.
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include "../raster_samples.hpp"
#include <cstddef>
#include <map>

int main() {
  using namespace raster_samples;
  if (!raster::reference_contract())
    return 1;
  struct Shaders {
    ComPtr<ID3DBlob> sample, index, modifier, varying, centroid, perspective, perspective_centroid, depth, query,
        output, color, read;
  };
  step("compile all Direct3D 10.1 sample shaders before device creation");
  auto vs_code = compile(raster::hlsl, "vs", "vs_4_1", {"DISTANCES=0"});
  auto clip_code = compile(raster::hlsl, "vs", "vs_4_1", {"DISTANCES=1"});
  auto read_vs_code =
      compile(raster_samples::hlsl, "vs_read", "vs_4_1", {"LEGACY=1", "WIDTH=" + std::to_string(width), "COUNT=1"});
  if (!vs_code || !clip_code || !read_vs_code)
    return 1;
  std::map<unsigned, Shaders> shaders;
  for (unsigned count : {1u, 2u, 4u, 8u, 16u}) {
    auto &s = shaders[count];
    std::vector<std::string> defs = {"LEGACY=1", "WIDTH=" + std::to_string(width), "COUNT=" + std::to_string(count)};
    s.sample = compile(raster_samples::hlsl, "ps_sample", "ps_4_1", defs);
    s.index = compile(raster_samples::hlsl, "ps_index", "ps_4_1", defs);
    s.modifier = compile(raster_samples::hlsl, "ps_modifier", "ps_4_1", defs);
    s.varying = compile(raster_samples::hlsl, "ps_varying", "ps_4_1", defs);
    s.centroid = compile(raster_samples::hlsl, "ps_centroid", "ps_4_1", defs);
    s.depth = compile(raster_samples::hlsl, "ps_depth", "ps_4_1", defs);
    defs.push_back("PERSPECTIVE=1");
    s.perspective = compile(raster_samples::hlsl, "ps_varying", "ps_4_1", defs);
    s.perspective_centroid = compile(raster_samples::hlsl, "ps_centroid", "ps_4_1", defs);
    defs.pop_back();
    s.query = compile(raster_samples::hlsl, "ps_query", "ps_4_1", defs);
    s.output = compile(raster_samples::hlsl, "ps_export", "ps_4_1", defs);
    s.color = compile(raster_samples::hlsl, "ps_color", "ps_4_1", defs);
    s.read = compile(raster_samples::hlsl, "ps_read", "ps_4_1", defs);
    if (!s.sample || !s.index || !s.modifier || !s.varying || !s.centroid || !s.perspective ||
        !s.perspective_centroid || !s.depth || !s.query || !s.output || !s.color || !s.read)
      return 1;
  }
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  ComPtr<ID3D10Device1> device;
  CHECK(
      create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &device));
  ComPtr<ID3D10VertexShader> vs, clip, read_vs;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), &vs));
  CHECK(device->CreateVertexShader(clip_code->GetBufferPointer(), clip_code->GetBufferSize(), &clip));
  CHECK(device->CreateVertexShader(read_vs_code->GetBufferPointer(), read_vs_code->GetBufferSize(), &read_vs));
  D3D10_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, position), D3D10_INPUT_PER_VERTEX_DATA, 0},
      {"DISTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, distance), D3D10_INPUT_PER_VERTEX_DATA, 0},
      {"DISTANCE", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, distance) + sizeof(Color),
       D3D10_INPUT_PER_VERTEX_DATA, 0}};
  ComPtr<ID3D10InputLayout> layout;
  CHECK(device->CreateInputLayout(elements, std::size(elements), clip_code->GetBufferPointer(),
                                  clip_code->GetBufferSize(), &layout));
  D3D10_BUFFER_DESC bd{sizeof(Vertex) * Triangle, D3D10_USAGE_DEFAULT, D3D10_BIND_VERTEX_BUFFER};
  ComPtr<ID3D10Buffer> vb;
  CHECK(device->CreateBuffer(&bd, nullptr, &vb));
  D3D10_RASTERIZER_DESC rd{D3D10_FILL_SOLID, D3D10_CULL_NONE};
  rd.DepthClipEnable = TRUE;
  ComPtr<ID3D10RasterizerState> states[4];
  for (unsigned flags = 0; flags < std::size(states); flags++) {
    rd.MultisampleEnable = flags & 1;
    rd.AntialiasedLineEnable = flags >> 1;
    CHECK(device->CreateRasterizerState(&rd, &states[flags]));
  }
  auto geometry = raster_samples::cases();
  for (auto &[count, s] : shaders)
    for (bool centered : {false, true})
      for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32G32B32A32_FLOAT}) {
        if (centered && count == 1)
          continue;
        UINT quality = 0;
        CHECK(device->CheckMultisampleQualityLevels(format, count, &quality));
        if (count == 1 || (count == 4 && format == DXGI_FORMAT_R8G8B8A8_UNORM))
          expect(quality > 0, "required format %u count %u was not reported", format, count);
        if (!quality) {
          printf("unsupported: format %u count %u\n", format, count);
          continue;
        }
        std::vector<Job> all;
        if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
          for (size_t n = 0; n < geometry.size(); n++)
            if ((count > 1 || geometry[n].shape != Line) && (!centered || geometry[n].shape != Line))
              all.push_back({n, ~0u, LegacyCoverage});
        } else {
          all = jobs(count, centered);
          for (auto &j : all) {
            if (j.pass == Shading)
              j.pass = LegacyShading;
            else if (j.pass == IndexOnly)
              j.pass = LegacyIndex;
            else if (j.pass == ModifierOnly)
              j.pass = LegacyModifier;
            else if (j.pass == Export)
              j.pass = LegacyExport;
          }
        }
        ComPtr<ID3D10PixelShader> sample_ps, index_ps, modifier_ps, varying_ps, centroid_ps, perspective_ps,
            perspective_centroid_ps, depth_ps, query_ps, output_ps, color_ps, read_ps;
        CHECK(device->CreatePixelShader(s.sample->GetBufferPointer(), s.sample->GetBufferSize(), &sample_ps));
        CHECK(device->CreatePixelShader(s.index->GetBufferPointer(), s.index->GetBufferSize(), &index_ps));
        CHECK(device->CreatePixelShader(s.modifier->GetBufferPointer(), s.modifier->GetBufferSize(), &modifier_ps));
        CHECK(device->CreatePixelShader(s.varying->GetBufferPointer(), s.varying->GetBufferSize(), &varying_ps));
        CHECK(device->CreatePixelShader(s.centroid->GetBufferPointer(), s.centroid->GetBufferSize(), &centroid_ps));
        CHECK(device->CreatePixelShader(s.perspective->GetBufferPointer(), s.perspective->GetBufferSize(),
                                        &perspective_ps));
        CHECK(device->CreatePixelShader(s.perspective_centroid->GetBufferPointer(),
                                        s.perspective_centroid->GetBufferSize(), &perspective_centroid_ps));
        CHECK(device->CreatePixelShader(s.depth->GetBufferPointer(), s.depth->GetBufferSize(), &depth_ps));
        CHECK(device->CreatePixelShader(s.query->GetBufferPointer(), s.query->GetBufferSize(), &query_ps));
        CHECK(device->CreatePixelShader(s.output->GetBufferPointer(), s.output->GetBufferSize(), &output_ps));
        CHECK(device->CreatePixelShader(s.color->GetBufferPointer(), s.color->GetBufferSize(), &color_ps));
        CHECK(device->CreatePixelShader(s.read->GetBufferPointer(), s.read->GetBufferSize(), &read_ps));
        D3D10_TEXTURE2D_DESC td{
            width,
            height,
            1,
            1,
            format,
            {count,
             count == 1 ? 0u : UINT(centered ? D3D11_CENTER_MULTISAMPLE_PATTERN : D3D11_STANDARD_MULTISAMPLE_PATTERN)},
            D3D10_USAGE_DEFAULT,
            D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE};
        ComPtr<ID3D10Texture2D> target, positions, copied, staging;
        ComPtr<ID3D10RenderTargetView> rtv, copied_rtv;
        ComPtr<ID3D10ShaderResourceView> target_srv, positions_srv;
        // a sample count the device reports is one it creates: a refusal is a wrong result, not the test's end
        step("Direct3D10.1 center %u format %u count %u targets", centered, format, count);
        if (!expect(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &target)), "render target was refused"))
          continue;
        CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        td.BindFlags = D3D10_BIND_SHADER_RESOURCE;
        if (!expect(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &positions)), "texture was refused"))
          continue;
        D3D10_SHADER_RESOURCE_VIEW_DESC sd{format, D3D10_SRV_DIMENSION_TEXTURE2DMS};
        CHECK(device->CreateShaderResourceView(target.Get(), &sd, &target_srv));
        CHECK(device->CreateShaderResourceView(positions.Get(), &sd, &positions_srv));
        td = {width * count,           height, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D10_USAGE_DEFAULT,
              D3D10_BIND_RENDER_TARGET};
        CHECK(device->CreateTexture2D(&td, nullptr, &copied));
        CHECK(device->CreateRenderTargetView(copied.Get(), nullptr, &copied_rtv));
        td.Usage = D3D10_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        CHECK(device->CreateTexture2D(&td, nullptr, &staging));
        for (const auto &j : all) {
          auto &c = geometry[j.geometry];
          step("Direct3D10.1 center %u format %u count %u geometry %zu pass %u mask %#x", centered, format, count,
               j.geometry, j.pass, j.mask);
          std::array<Vertex, Triangle> vertices{};
          std::copy(c.vertices.begin(), c.vertices.end(), vertices.begin());
          device->UpdateSubresource(vb.Get(), 0, nullptr, vertices.data(), 0, 0);
          const UINT stride = sizeof(Vertex), offset = 0;
          device->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
          device->IASetInputLayout(layout.Get());
          device->IASetPrimitiveTopology(c.shape == Point  ? D3D10_PRIMITIVE_TOPOLOGY_POINTLIST
                                         : c.shape == Line ? D3D10_PRIMITIVE_TOPOLOGY_LINELIST
                                                           : D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          device->VSSetShader(c.mode == Clip ? clip.Get() : vs.Get());
          device->PSSetShader(j.pass == PerspectiveVarying    ? perspective_ps.Get()
                              : j.pass == PerspectiveCentroid ? perspective_centroid_ps.Get()
                              : j.pass == PositionDepth       ? depth_ps.Get()
                              : j.pass == Varying             ? varying_ps.Get()
                              : j.pass == Centroid            ? centroid_ps.Get()
                              : j.pass == LegacyCoverage      ? color_ps.Get()
                              : j.pass == LegacyShading       ? sample_ps.Get()
                              : j.pass == LegacyIndex         ? index_ps.Get()
                              : j.pass == LegacyModifier      ? modifier_ps.Get()
                              : j.pass == LegacyExport        ? output_ps.Get()
                                                              : query_ps.Get());
          auto source = j.pass == Unbound ? nullptr : positions_srv.Get();
          device->PSSetShaderResources(0, 1, &source);
          device->RSSetState(states[unsigned(c.multisample) | (unsigned(c.aa) << 1)].Get());
          D3D10_VIEWPORT vp{0, 0, width, height, 0, 1};
          device->RSSetViewports(1, &vp);
          device->OMSetBlendState(nullptr, nullptr, j.mask);
          device->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
          const float clear[lanes] = {};
          device->ClearRenderTargetView(rtv.Get(), clear);
          device->DrawInstanced(c.vertices.size(), 1, 0, 0);
          device->OMSetRenderTargets(1, copied_rtv.GetAddressOf(), nullptr);
          device->PSSetShaderResources(0, 1, target_srv.GetAddressOf());
          vp.Width = width * count;
          device->RSSetViewports(1, &vp);
          device->RSSetState(states[0].Get());
          device->OMSetBlendState(nullptr, nullptr, ~0u);
          device->IASetInputLayout(nullptr);
          device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          device->VSSetShader(read_vs.Get());
          device->PSSetShader(read_ps.Get());
          device->Draw(Triangle, 0);
          ID3D10ShaderResourceView *empty = nullptr;
          device->PSSetShaderResources(0, 1, &empty);
          device->CopyResource(staging.Get(), copied.Get());
          D3D10_MAPPED_TEXTURE2D mapped;
          CHECK(staging->Map(0, D3D10_MAP_READ, 0, &mapped));
          std::vector<Color> pixels(width * height * count);
          for (unsigned y = 0; y < height; y++)
            for (unsigned x = 0; x < width; x++)
              for (unsigned sid = 0; sid < count; sid++)
                pixels[(y * width + x) * count + sid] = reinterpret_cast<const Color *>(
                    static_cast<const char *>(mapped.pData) + y * mapped.RowPitch)[sid * width + x];
          staging->Unmap(0);
          raster_samples::compare(pixels.data(), c, count, j.mask, j.pass, centered);
        }
      }
  return verdict();
}
