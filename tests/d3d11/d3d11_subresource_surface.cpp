// contract: DXGI exposes eligible subresources with their own dimensions and storage, and a created surface
// retains its parent without exposing the parent's interfaces. IDXGIResource1::CreateSubresourceSurface:
// "Creates a subresource surface object." IDXGISurface2::GetResource: "Gets the parent resource and subresource
// index that support a subresource surface."
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiresource1-createsubresourcesurface
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgisurface2-getresource
// Wine dlls/dxgi/tests/dxgi.c test_subresource_surface records Windows accepting buffers, 1D/2D mip arrays and
// depth-one 3D mip chains, rejecting every subresource of a deeper 3D texture, and leaving an invalid index's
// output unchanged. Its GetResource checks require E_POINTER for a null parent output and an unchanged index
// on E_NOINTERFACE. dlls/{d3d10core,d3d11}/tests test_texture{1,2,3}d_interfaces record direct surface queries
// succeeding only for single-subresource 1D/2D textures; these surfaces share their resource's COM identity.
// D3D11.3 5: "mipslice N+1 size = floor( mipslice N size / 2)"; each dimension stops at one. 5.3: "For buffers,
// Texture1D, Texture1DArray, and Texture3D resources, the only Sample Count allowed is 1 and the only allowed
// Quality Level is zero." 5.6.1: "Mapping means granting CPU access to the Subresource's storage or contents."
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// seeded shapes include full mip chains, odd dimensions and every slice. mapping is compared to the parent
// API's pitches and to independently written words, never padding. argv[1] is the seed, argv[2] one variant.
// private data belongs to each COM identity: "Set an interface in the object's private data." its retained
// reference ends "when either the IDXGIObject is destroyed" (IDXGIObject::SetPrivateDataInterface).
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiobject-setprivatedatainterface
// DXGI_MAP_DISCARD: "Discard the previous contents of a resource when it is mapped." (IDXGISurface::Map).
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgisurface-map
// there are no shaders; tests/run.py bounds the whole process by its Meson timeout.
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <limits>
#include <random>

static ULONG
references(IUnknown *object) {
  object->AddRef();
  return object->Release();
}

static int
exercise(UINT seed, UINT only) {
  const UINT word_bytes = sizeof(UINT);
  const UINT map_modes[] = {DXGI_MAP_READ, DXGI_MAP_WRITE, DXGI_MAP_READ | DXGI_MAP_WRITE};
  UINT writes = 0;
  for (auto mode : map_modes)
    writes += bool(mode & DXGI_MAP_WRITE);
  auto module = LoadLibraryW(L"d3d10_1.dll");
  if (!expect(module != nullptr, "d3d10_1.dll did not load: %lu", GetLastError()))
    return verdict();
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(GetProcAddress(module, "D3D10CreateDevice1"));
  if (!expect(create10 != nullptr, "D3D10CreateDevice1 is absent")) {
    FreeLibrary(module);
    return verdict();
  }
  UINT variant = 0;
  for (bool via10 : {false, true}) {
    step("seed %u D3D%u device", seed, via10 ? 10 : 11);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D10Device1> device10;
    ComPtr<ID3D11DeviceContext> context;
    if (via10) {
      CHECK(create10(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION,
                     &device10));
      CHECK(device10.As(&device));
      device->GetImmediateContext(&context);
    } else {
      const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
      CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                              nullptr, &context));
    }
    if (!only) {
      for (UINT samples = 1; samples <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; samples *= 2) {
        const DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
        UINT qualities = 0;
        CHECK(device->CheckMultisampleQualityLevels(format, samples, &qualities));
        if (!qualities)
          continue;
        step("seed %u D3D%u surface sample description %u", seed, via10 ? 10 : 11, samples);
        ComPtr<ID3D11Resource> resource;
        const DXGI_SAMPLE_DESC sample_desc{samples, 0};
        if (via10) {
          D3D10_TEXTURE2D_DESC desc{1, 1, 1, 1, format, sample_desc, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET};
          ComPtr<ID3D10Texture2D> object;
          CHECK(device10->CreateTexture2D(&desc, nullptr, &object));
          CHECK(object.As(&resource));
        } else {
          D3D11_TEXTURE2D_DESC desc{1, 1, 1, 1, format, sample_desc, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
          ComPtr<ID3D11Texture2D> object;
          CHECK(device->CreateTexture2D(&desc, nullptr, &object));
          CHECK(object.As(&resource));
        }
        ComPtr<IDXGIResource1> dxgi;
        ComPtr<IDXGISurface2> direct, created;
        CHECK(resource.As(&dxgi));
        CHECK(resource.As(&direct));
        CHECK(dxgi->CreateSubresourceSurface(0, &created));
        for (auto surface : {direct.Get(), created.Get()}) {
          DXGI_SURFACE_DESC desc{};
          expect(surface->GetDesc(&desc) == S_OK && desc.SampleDesc.Count == sample_desc.Count &&
                     desc.SampleDesc.Quality == sample_desc.Quality,
                 "surface lost the queried sample description");
          DXGI_MAPPED_RECT rect{};
          auto hr = surface->Map(&rect, DXGI_MAP_READ);
          expect(FAILED(hr), "default surface allowed CPU mapping");
          if (SUCCEEDED(hr))
            surface->Unmap();
        }
      }
    }
    std::minstd_rand random(seed);
    for (UINT dimension = D3D11_RESOURCE_DIMENSION_BUFFER; dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D;
         ++dimension) {
      for (UINT shape = 0; shape < 3; ++shape) {
        const UINT width = shape ? 9 + random() % 32 : 1;
        const UINT height = dimension >= D3D11_RESOURCE_DIMENSION_TEXTURE2D ? (shape ? 3 + random() % 16 : 1) : 1;
        for (UINT slices : {1u, 3u}) {
          if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER && slices != 1)
            continue;
          const UINT depth = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? slices : 1;
          const UINT elements =
              dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D || dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D
                  ? slices
                  : 1;
          const UINT full_mips = std::bit_width(std::max({width, height, depth}));
          for (UINT requested_mips : {1u, full_mips, 0u}) {
            if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER && requested_mips != 1)
              continue;
            ++variant;
            if (only && only != variant)
              continue;
            const UINT mips =
                dimension == D3D11_RESOURCE_DIMENSION_BUFFER ? 1 : (requested_mips ? requested_mips : full_mips);
            const UINT count = mips * elements;
            const bool valid = dimension != D3D11_RESOURCE_DIMENSION_TEXTURE3D || depth == 1;
            const bool direct =
                (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D || dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) &&
                count == 1;
            step("seed %u variant %u D3D%u dimension %u %ux%ux%u mips %u/%u slices %u", seed, variant, via10 ? 10 : 11,
                 dimension, width, height, depth, requested_mips, mips, elements);
            ComPtr<ID3D11Resource> resource;
            ComPtr<IUnknown> route;
            const DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
            const UINT cpu = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            switch (dimension) {
            case D3D11_RESOURCE_DIMENSION_BUFFER: {
              if (via10) {
                D3D10_BUFFER_DESC desc{width * word_bytes, D3D10_USAGE_STAGING, 0, cpu};
                ComPtr<ID3D10Buffer> object;
                CHECK(device10->CreateBuffer(&desc, nullptr, &object));
                CHECK(object.As(&resource));
                CHECK(object.As(&route));
              } else {
                D3D11_BUFFER_DESC desc{width * word_bytes, D3D11_USAGE_STAGING, 0, cpu};
                ComPtr<ID3D11Buffer> object;
                CHECK(device->CreateBuffer(&desc, nullptr, &object));
                CHECK(object.As(&resource));
                CHECK(object.As(&route));
              }
              break;
            }
            case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
              if (via10) {
                D3D10_TEXTURE1D_DESC desc{width, requested_mips, elements, format, D3D10_USAGE_STAGING, 0, cpu};
                ComPtr<ID3D10Texture1D> object;
                CHECK(device10->CreateTexture1D(&desc, nullptr, &object));
                CHECK(object.As(&resource));
                CHECK(object.As(&route));
              } else {
                D3D11_TEXTURE1D_DESC desc{width, requested_mips, elements, format, D3D11_USAGE_STAGING, 0, cpu};
                ComPtr<ID3D11Texture1D> object;
                CHECK(device->CreateTexture1D(&desc, nullptr, &object));
                CHECK(object.As(&resource));
                CHECK(object.As(&route));
              }
              break;
            }
            case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
              if (via10) {
                D3D10_TEXTURE2D_DESC desc{width, height, requested_mips, elements, format, {1, 0}, D3D10_USAGE_STAGING,
                                          0,     cpu};
                ComPtr<ID3D10Texture2D> object;
                CHECK(device10->CreateTexture2D(&desc, nullptr, &object));
                CHECK(object.As(&resource));
                CHECK(object.As(&route));
              } else {
                D3D11_TEXTURE2D_DESC desc{width, height, requested_mips, elements, format, {1, 0}, D3D11_USAGE_STAGING,
                                          0,     cpu};
                ComPtr<ID3D11Texture2D> object;
                CHECK(device->CreateTexture2D(&desc, nullptr, &object));
                CHECK(object.As(&resource));
                CHECK(object.As(&route));
              }
              break;
            }
            case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
              if (via10) {
                D3D10_TEXTURE3D_DESC desc{width, height, depth, requested_mips, format, D3D10_USAGE_STAGING, 0, cpu};
                ComPtr<ID3D10Texture3D> object;
                CHECK(device10->CreateTexture3D(&desc, nullptr, &object));
                CHECK(object.As(&resource));
                CHECK(object.As(&route));
              } else {
                D3D11_TEXTURE3D_DESC desc{width, height, depth, requested_mips, format, D3D11_USAGE_STAGING, 0, cpu};
                ComPtr<ID3D11Texture3D> object;
                CHECK(device->CreateTexture3D(&desc, nullptr, &object));
                CHECK(object.As(&resource));
                CHECK(object.As(&route));
              }
              break;
            }
            }
            ComPtr<IDXGIResource1> dxgi;
            CHECK(resource.As(&dxgi));
            if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
              for (UINT dynamic_width : {width, MULLION_PAGE_SIZE / word_bytes - 1, MULLION_PAGE_SIZE / word_bytes,
                                         MULLION_PAGE_SIZE / word_bytes + 1}) {
                ComPtr<ID3D11Buffer> dynamic;
                if (via10) {
                  D3D10_BUFFER_DESC desc{dynamic_width * word_bytes, D3D10_USAGE_DYNAMIC, D3D10_BIND_VERTEX_BUFFER,
                                         D3D10_CPU_ACCESS_WRITE};
                  ComPtr<ID3D10Buffer> object;
                  CHECK(device10->CreateBuffer(&desc, nullptr, &object));
                  CHECK(object.As(&dynamic));
                } else {
                  D3D11_BUFFER_DESC desc{dynamic_width * word_bytes, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER,
                                         D3D11_CPU_ACCESS_WRITE};
                  CHECK(device->CreateBuffer(&desc, nullptr, &dynamic));
                }
                ComPtr<IDXGIResource1> dynamic_dxgi;
                ComPtr<IDXGISurface2> dynamic_surface;
                CHECK(dynamic.As(&dynamic_dxgi));
                CHECK(dynamic_dxgi->CreateSubresourceSurface(0, &dynamic_surface));
                for (UINT generation = 0; generation < 2; ++generation) {
                  step("seed %u variant %u dynamic surface WRITE|DISCARD width %u generation %u", seed, variant,
                       dynamic_width, generation);
                  DXGI_MAPPED_RECT rect{};
                  CHECK(dynamic_surface->Map(&rect, DXGI_MAP_WRITE | DXGI_MAP_DISCARD));
                  for (UINT x = 0; x < dynamic_width; ++x) {
                    const UINT word = seed + generation * dynamic_width + x;
                    memcpy(rect.pBits + x * word_bytes, &word, sizeof(word));
                  }
                  CHECK(dynamic_surface->Unmap());
                  const auto words = read(device.Get(), context.Get(), dynamic.Get());
                  expect(words.size() == dynamic_width, "dynamic surface readback size differs");
                  for (UINT x = 0; x < words.size(); ++x)
                    expect(words[x] == seed + generation * dynamic_width + x, "dynamic surface word %u differs", x);
                }
              }
            }
            auto check_surface = [&](IDXGISurface2 *surface, UINT index, bool whole) {
              const UINT w = std::max(1u, width >> (index % mips));
              const UINT h = std::max(1u, height >> (index % mips));
              const UINT words = w * h;
              auto value = [&](UINT x, UINT y, UINT generation) {
                return seed + (index + 1) * width * height + y * w + x + generation * words;
              };
              DXGI_SURFACE_DESC desc{};
              expect(surface->GetDesc(&desc) == S_OK &&
                         desc.Width == (dimension == D3D11_RESOURCE_DIMENSION_BUFFER ? w * sizeof(UINT) : w) &&
                         desc.Height == h &&
                         desc.Format == (dimension == D3D11_RESOURCE_DIMENSION_BUFFER ? DXGI_FORMAT_UNKNOWN : format) &&
                         desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0,
                     "surface description differs");
              ComPtr<ID3D11Device> owner, parent_device;
              expect(surface->GetDevice(IID_PPV_ARGS(&owner)) == S_OK && owner == device, "wrong surface device");
              expect(surface->GetParent(IID_PPV_ARGS(&parent_device)) == S_OK && parent_device == device,
                     "wrong surface parent");
              ComPtr<IUnknown> identity;
              CHECK(surface->QueryInterface(IID_PPV_ARGS(&identity)));
              expect(whole ? identity == route : identity != route, "surface identity differs");
              const auto &key = __uuidof(IDXGISurface2);
              const UINT parent_value = seed, surface_value = seed + 1, peer_value = seed + 2;
              CHECK(dxgi->SetPrivateData(key, sizeof(parent_value), &parent_value));
              CHECK(surface->SetPrivateData(key, sizeof(surface_value), &surface_value));
              ComPtr<IDXGISurface2> peer;
              CHECK(dxgi->CreateSubresourceSurface(index, &peer));
              CHECK(peer->SetPrivateData(key, sizeof(peer_value), &peer_value));
              UINT size = sizeof(UINT), got = 0;
              expect(surface->GetPrivateData(key, &size, &got) == S_OK && size == sizeof(got) && got == surface_value,
                     "another surface changed private data");
              size = sizeof(got);
              expect(dxgi->GetPrivateData(key, &size, &got) == S_OK && got == (whole ? surface_value : parent_value),
                     "surface private data changed its parent");
              size = sizeof(got);
              expect(peer->GetPrivateData(key, &size, &got) == S_OK && got == peer_value,
                     "created surface lost its private data");
              const ULONG before = references(device.Get());
              CHECK(surface->SetPrivateDataInterface(key, device.Get()));
              expect(references(device.Get()) == before + 1, "surface private interface was not retained");
              IUnknown *stored = nullptr;
              size = sizeof(IUnknown *);
              CHECK((whole ? static_cast<IDXGIObject *>(dxgi.Get()) : static_cast<IDXGIObject *>(surface))
                        ->GetPrivateData(key, &size, &stored));
              expect(size == sizeof(stored) && stored == device.Get(), "surface private interface lost its identity");
              if (stored == device.Get())
                stored->Release();
              stored = nullptr;
              CHECK(surface->SetPrivateData(key, 0, nullptr));
              expect(references(device.Get()) == before, "surface private interface survived replacement");
              CHECK(peer->SetPrivateDataInterface(key, device.Get()));
              expect(references(device.Get()) == before + 1, "private interface was not retained");
              size = sizeof(IUnknown *);
              CHECK(peer->GetPrivateData(key, &size, &stored));
              expect(size == sizeof(stored) && stored == device.Get() && references(device.Get()) == before + 2,
                     "private interface retrieval lost its reference");
              if (stored == device.Get())
                stored->Release();
              peer.Reset();
              expect(references(device.Get()) == before, "destroyed surface retained its private interface");
              for (const auto &iid : {__uuidof(IDXGISurface), __uuidof(IDXGISurface1), __uuidof(IDXGISurface2),
                                      __uuidof(IDXGIDeviceSubObject), __uuidof(IDXGIObject)}) {
                ComPtr<IUnknown> result;
                expect(surface->QueryInterface(iid, &result) == S_OK && result, "surface lost IID %lu", iid.Data1);
              }
              for (const auto &iid : {__uuidof(ID3D11Resource), __uuidof(ID3D10Resource), __uuidof(IDXGIResource),
                                      __uuidof(IDXGIResource1)}) {
                ComPtr<IUnknown> result;
                auto hr = surface->QueryInterface(iid, &result);
                expect(hr == (whole ? S_OK : E_NOINTERFACE) && (whole ? bool(result) : !result),
                       "surface outer IID %lu returned %#lx", iid.Data1, hr);
              }
              UINT at = count + 1;
              expect(surface->GetResource(__uuidof(IDXGIResource1), nullptr, &at) == E_POINTER && at == count + 1,
                     "null parent output changed the index or HRESULT");
              IUnknown *rejected = route.Get();
              expect(surface->GetResource(__uuidof(IDXGIFactory), reinterpret_cast<void **>(&rejected), &at) ==
                             E_NOINTERFACE &&
                         rejected == nullptr && at == count + 1,
                     "failed parent query changed the index or output");
              ComPtr<IDXGIResource1> parent;
              expect(surface->GetResource(IID_PPV_ARGS(&parent), &at) == S_OK && parent == dxgi && at == index,
                     "wrong surface resource or index");
              HDC dc = nullptr;
              expect(surface->GetDC(FALSE, &dc) == DXGI_ERROR_INVALID_CALL, "non-GDI surface gave a DC");
              expect(surface->ReleaseDC(nullptr) == DXGI_ERROR_INVALID_CALL, "non-GDI surface released a DC");
              D3D11_MAPPED_SUBRESOURCE mapped{};
              CHECK(context->Map(resource.Get(), index, D3D11_MAP_WRITE, 0, &mapped));
              if (!expect(mapped.pData && mapped.RowPitch >= w * word_bytes, "parent map lost its storage or pitch")) {
                context->Unmap(resource.Get(), index);
                return 1;
              }
              for (UINT y = 0; y < h; ++y)
                for (UINT x = 0; x < w; ++x) {
                  UINT word = value(x, y, 0);
                  memcpy(static_cast<BYTE *>(mapped.pData) + y * mapped.RowPitch + x * sizeof(word), &word,
                         sizeof(word));
                }
              const UINT pitch = mapped.RowPitch;
              context->Unmap(resource.Get(), index);
              UINT generation = 0;
              for (UINT mode : map_modes) {
                DXGI_MAPPED_RECT rect{};
                CHECK(surface->Map(&rect, mode));
                expect(rect.pBits && rect.Pitch == INT(pitch), "surface map lost storage or pitch");
                if (rect.pBits && rect.Pitch >= INT(w * sizeof(UINT))) {
                  for (UINT y = 0; y < h; ++y)
                    for (UINT x = 0; x < w; ++x) {
                      UINT word;
                      auto address = rect.pBits + y * rect.Pitch + x * sizeof(word);
                      if (mode & DXGI_MAP_READ) {
                        memcpy(&word, address, sizeof(word));
                        expect(word == value(x, y, generation), "subresource %u word %u,%u differs", index, x, y);
                      }
                      if (mode & DXGI_MAP_WRITE) {
                        word = value(x, y, generation + 1);
                        memcpy(address, &word, sizeof(word));
                      }
                    }
                }
                CHECK(surface->Unmap());
                generation += bool(mode & DXGI_MAP_WRITE);
                CHECK(context->Map(resource.Get(), index, D3D11_MAP_READ, 0, &mapped));
                if (!expect(mapped.pData && mapped.RowPitch >= w * word_bytes,
                            "parent map lost its storage or pitch")) {
                  context->Unmap(resource.Get(), index);
                  return 1;
                }
                for (UINT y = 0; y < h; ++y)
                  for (UINT x = 0; x < w; ++x) {
                    UINT word;
                    memcpy(&word, static_cast<BYTE *>(mapped.pData) + y * mapped.RowPitch + x * sizeof(word),
                           sizeof(word));
                    expect(word == value(x, y, generation), "surface did not write subresource %u word %u,%u", index, x,
                           y);
                  }
                context->Unmap(resource.Get(), index);
              }
              return 0;
            };
            ComPtr<IDXGISurface2> whole;
            auto hr = resource.As(&whole);
            expect(hr == (direct ? S_OK : E_NOINTERFACE), "direct surface query returned %#lx", hr);
            if (whole && check_surface(whole.Get(), 0, true))
              return 1;
            whole.Reset();
            ComPtr<IDXGISurface2> last;
            std::vector<bool> written(count);
            for (UINT index = 0; index < count; ++index) {
              step("seed %u variant %u subresource %u/%u", seed, variant, index, count);
              auto before = references(resource.Get());
              ComPtr<IDXGISurface2> surface;
              hr = dxgi->CreateSubresourceSurface(index, &surface);
              expect(hr == (valid ? S_OK : E_INVALIDARG) && (valid ? bool(surface) : !surface),
                     "CreateSubresourceSurface returned %#lx", hr);
              if (!surface)
                continue;
              expect(references(resource.Get()) == before + 1, "surface did not retain its resource once");
              expect(surface->AddRef() == 2 && references(resource.Get()) == before + 1,
                     "surface references changed the parent count");
              expect(surface->Release() == 1, "surface reference count differs");
              if (check_surface(surface.Get(), index, false))
                return 1;
              written[index] = true;
              surface.Reset();
              expect(references(resource.Get()) == before, "surface leaked its resource reference");
              CHECK(dxgi->CreateSubresourceSurface(index, &last));
            }
            for (UINT index = 0; index < count; ++index) {
              if (!written[index])
                continue;
              step("seed %u variant %u verify retained subresource %u", seed, variant, index);
              const UINT w = std::max(1u, width >> (index % mips)), h = std::max(1u, height >> (index % mips));
              D3D11_MAPPED_SUBRESOURCE mapped{};
              CHECK(context->Map(resource.Get(), index, D3D11_MAP_READ, 0, &mapped));
              if (!expect(mapped.pData && mapped.RowPitch >= w * word_bytes,
                          "retained map lost its storage or pitch")) {
                context->Unmap(resource.Get(), index);
                return 1;
              }
              for (UINT y = 0; y < h; ++y)
                for (UINT x = 0; x < w; ++x) {
                  UINT word;
                  memcpy(&word, static_cast<BYTE *>(mapped.pData) + y * mapped.RowPitch + x * word_bytes, sizeof(word));
                  expect(word == seed + (index + 1) * width * height + y * w + x + writes * w * h,
                         "another surface changed subresource %u word %u,%u", index, x, y);
                }
              context->Unmap(resource.Get(), index);
            }
            for (UINT index : {count, count + 1, std::numeric_limits<UINT>::max()}) {
              auto sentinel = reinterpret_cast<IDXGISurface2 *>(resource.Get());
              auto beyond = sentinel;
              hr = dxgi->CreateSubresourceSurface(index, &beyond);
              expect(hr == E_INVALIDARG && beyond == sentinel, "invalid index %u changed output or returned %#lx",
                     index, hr);
              if (beyond && beyond != sentinel)
                beyond->Release();
            }
            if (last) {
              const UINT index = count - 1;
              dxgi.Reset();
              route.Reset();
              resource.Reset();
              step("seed %u variant %u surface outlives resource pointers", seed, variant);
              DXGI_MAPPED_RECT rect{};
              CHECK(last->Map(&rect, DXGI_MAP_READ));
              UINT word = 0;
              if (rect.pBits)
                memcpy(&word, rect.pBits, sizeof(word));
              const UINT words = std::max(1u, width >> (index % mips)) * std::max(1u, height >> (index % mips));
              expect(rect.pBits && word == seed + (index + 1) * width * height + writes * words,
                     "retained surface lost its storage");
              CHECK(last->Unmap());
            }
          }
        }
      }
    }
  }
  FreeLibrary(module);
  expect(!only || only <= variant, "variant %u does not exist", only);
  return verdict();
}

int
main(int argc, char **argv) {
  UINT arguments[] = {argc > 1 ? UINT(strtoul(argv[1], nullptr, 0)) : 1,
                      argc > 2 ? UINT(strtoul(argv[2], nullptr, 0)) : 0};
  return exercise(arguments[0], arguments[1]);
}
