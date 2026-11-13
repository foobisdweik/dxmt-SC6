// contract: usage, CPU access and bind flags form one creation contract, including validation-only calls.
// "Read-only memory that can be accessed only by the GPU" (D3D11_USAGE_IMMUTABLE); "cannot be set as outputs
// of the pipeline" (D3D11_CPU_ACCESS_WRITE); CPU read resources "must be created with staging usage" and
// "cannot be set as either inputs or outputs to the pipeline" (D3D11_CPU_ACCESS_READ, Microsoft Learn).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_usage
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_cpu_access_flag
// D3D11.3 5.6.1.3 separately permits CPU access to DEFAULT SRV/UAV buffers when MapOnDefaultBuffers is reported.
// 5.3.9.1: "may NOT be combined with" includes STREAM_OUTPUT for UNORDERED_ACCESS.
// Wine d3d11.c test_resource_access records E_INVALIDARG for missing dynamic/staging CPU access, zero immutable
// or dynamic bind flags, and incompatible usage/bind/access combinations. its committed verdict is timeout.
// every legal control checks all initial bytes through a staging copy; both single flags and every pair run.
#include "d3d11_test.hpp"
#include "../resource_cases.hpp"
#include <d3d11_2.h>
#include <algorithm>
#include <array>
#include <climits>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, &context));
  D3D11_FEATURE_DATA_D3D11_OPTIONS1 options{};
  CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &options, sizeof(options)));
  constexpr UINT input = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_CONSTANT_BUFFER |
                         D3D11_BIND_SHADER_RESOURCE;
  constexpr UINT output = D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
  constexpr UINT cpu = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
  std::vector<UINT> binds;
  for (UINT a : resource_binds)
    for (UINT b : resource_binds)
      binds.push_back(a | b);
  std::sort(binds.begin(), binds.end());
  binds.erase(std::unique(binds.begin(), binds.end()), binds.end());
  std::array<UINT, 4 * D3D11_COMMONSHADER_CONSTANT_BUFFER_COMPONENTS> words;
  for (UINT i = 0; i < words.size(); i++)
    words[i] = i * 3 + 1;
  const D3D11_SUBRESOURCE_DATA data{words.data()};
  for (UINT usage = D3D11_USAGE_DEFAULT; usage <= D3D11_USAGE_STAGING; usage++)
    for (UINT access : {0u, UINT(D3D11_CPU_ACCESS_READ), UINT(D3D11_CPU_ACCESS_WRITE), cpu, UINT_MAX})
      for (UINT bind : binds) {
        bool valid = !(access & ~cpu) && !(bind & ~(input | output)) &&
                     (!(bind & D3D11_BIND_CONSTANT_BUFFER) || bind == D3D11_BIND_CONSTANT_BUFFER) &&
                     !((bind & D3D11_BIND_STREAM_OUTPUT) && (bind & D3D11_BIND_UNORDERED_ACCESS));
        switch (usage) {
        case D3D11_USAGE_DEFAULT:
          valid &= !access || (options.MapOnDefaultBuffers &&
                                  (bind & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)) &&
                                  !(bind & ~(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)));
          break;
        case D3D11_USAGE_IMMUTABLE:
          valid &= !access && bind && !(bind & ~input);
          break;
        case D3D11_USAGE_DYNAMIC:
          valid &= access == D3D11_CPU_ACCESS_WRITE && bind && !(bind & ~input);
          break;
        case D3D11_USAGE_STAGING:
          valid &= access && !bind;
          break;
        }
        for (bool result : {false, true}) {
          step("buffer usage %u CPU %#x bind %#x output %u valid %u", usage, access, bind, result, valid);
          D3D11_BUFFER_DESC desc{sizeof(words), D3D11_USAGE(usage), bind, access};
          auto sentinel = reinterpret_cast<ID3D11Buffer *>(device.Get());
          auto made = sentinel;
          const HRESULT hr = device->CreateBuffer(&desc, &data, result ? &made : nullptr);
          const HRESULT want = valid ? (result ? S_OK : S_FALSE) : E_INVALIDARG;
          expect(hr == want, "CreateBuffer %#lx, want %#lx", hr, want);
          if (result) {
            expect(SUCCEEDED(hr) ? made && made != sentinel : !made, "creation output %p", made);
            if (made && made != sentinel) {
              ComPtr<ID3D11Buffer> resource;
              resource.Attach(made);
              if (valid) {
                auto got = read(device.Get(), context.Get(), resource.Get());
                expect(got.size() == words.size() && !memcmp(got.data(), words.data(), sizeof(words)),
                       "initial bytes differ for a legal creation");
              }
            }
          }
        }
      }
  // and a texture's, of the binds a texture takes, depth with a depth format and without initial data (Wine runs the
  // same table on textures)
  for (UINT usage = D3D11_USAGE_DEFAULT; usage <= D3D11_USAGE_STAGING; usage++)
    for (UINT access : {0u, UINT(D3D11_CPU_ACCESS_READ), UINT(D3D11_CPU_ACCESS_WRITE), cpu})
      for (UINT bind : {0u, UINT(D3D11_BIND_SHADER_RESOURCE), UINT(D3D11_BIND_RENDER_TARGET),
                        UINT(D3D11_BIND_UNORDERED_ACCESS), UINT(D3D11_BIND_DEPTH_STENCIL),
                        UINT(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET)}) {
        bool depth = bind == D3D11_BIND_DEPTH_STENCIL, valid = false;
        switch (usage) {
        case D3D11_USAGE_DEFAULT:
          valid = !access;
          break;
        case D3D11_USAGE_IMMUTABLE:
          valid = !access && bind == D3D11_BIND_SHADER_RESOURCE;
          break;
        case D3D11_USAGE_DYNAMIC:
          valid = access == D3D11_CPU_ACCESS_WRITE && bind == D3D11_BIND_SHADER_RESOURCE;
          break;
        case D3D11_USAGE_STAGING:
          valid = access && !bind;
          break;
        }
        step("texture usage %u CPU %#x bind %#x valid %u", usage, access, bind, valid);
        D3D11_TEXTURE2D_DESC desc{
            4, 4, 1, 1, depth ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE(usage), bind,
            access
        };
        const D3D11_SUBRESOURCE_DATA texels{words.data(), 4 * sizeof(UINT)};
        const HRESULT hr = device->CreateTexture2D(&desc, depth ? nullptr : &texels, nullptr);
        expect(hr == (valid ? S_FALSE : E_INVALIDARG), "CreateTexture2D %#lx", hr);
      }
  // TIER_1 explicitly includes TILE_POOL creation; NOT_SUPPORTED provides no tiled-resource capability.
  // https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_tiled_resources_tier
  // 5.9.2.4: "Allocation size, as a multiple of 64KB (0 is valid since there is a Resize operation available)."
  if (options.TiledResourcesTier == D3D11_TILED_RESOURCES_NOT_SUPPORTED)
    for (UINT bytes : {0u, UINT(D3D11_2_TILED_RESOURCE_TILE_SIZE_IN_BYTES)})
      for (bool output_pointer : {false, true}) {
        step("unsupported tile pool bytes %u output %u", bytes, output_pointer);
        D3D11_BUFFER_DESC desc{bytes, D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_TILE_POOL};
        auto sentinel = reinterpret_cast<ID3D11Buffer *>(device.Get());
        auto made = sentinel;
        const HRESULT hr = device->CreateBuffer(&desc, nullptr, output_pointer ? &made : nullptr);
        expect(FAILED(hr), "unsupported tile pool accepted: %#lx", hr);
        if (output_pointer) {
          expect(!made, "unsupported creation output %p", made);
          if (made && made != sentinel)
            made->Release();
        }
      }
  return verdict();
}
