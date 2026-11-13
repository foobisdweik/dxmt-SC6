// contract: a nonempty box copies its bytes to the destination offset, including between texture dimensions;
// an empty box leaves every byte alone. "This function allows sub-region copying of data from one Subresource
// to another." (D3D11.3 5.6.2, D3D11_3_FunctionalSpec.htm). "An empty box results in a no-op."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion
// Wine's d3d11.c test_copy_subresource_region_1d and _3d record Windows copying 1D rows to 2D and 2D planes to 3D;
// Learn's "Must be the same type" conflicts with that witness. only boxes that fit both subresources are used.
// seeded boxes exercise each texture dimension from the API enum, both staging directions, mips, slices and
// immediate/deferred contexts. every destination word, including outside the box, and every source is checked.
// initial 1D staging data is read before any region copy, with ignored pitches: "System-memory pitch is used
// only for 2D and 3D texture data" (Microsoft Learn, D3D11_SUBRESOURCE_DATA).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_subresource_data
// there are no shaders. Map pitches locate words; padding is never compared.
// the same byte oracle runs CopyResource and CopySubresourceRegion through devices created at both D3D10 levels.
// D3D10 copies use matching resource dimensions and its immediate device API.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-copysubresourceregion
// UpdateSubresource: "An empty box results in a no-op." and "the application is free to change or even free
// the data pointed to by pSrcData". source padding and immediate reuse distinguish pitches from image extents.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-updatesubresource
// same-subresource copies use a pre-copy snapshot: "The driver must handle overlapping copies."
// (D3D11.3 5.6.2.1). both directions and every nondegenerate axis must preserve the untouched texels.
// 5.6.9: "DISCARD means that the system may discard the entire contents of the destination memory outside
// the region being updated." NO_OVERWRITE follows a DISCARD on the same recording context, with no earlier
// read of the written range. an earlier read before DISCARD retains its generation; discarded bytes are ignored.
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <d3d10_1.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <random>

static int
copying(int argc, char **argv, D3D10_FEATURE_LEVEL1 legacy_level = D3D10_FEATURE_LEVEL1(0)) {
  const UINT seed = argc > 1 ? UINT(strtoul(argv[1], nullptr, 0)) : std::mt19937::default_seed;
  const UINT word_bytes = sizeof(UINT);
  std::mt19937 random(seed);
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D10Device1> device10;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  step("seed %u: device and deferred context", seed);
  if (legacy_level) {
    auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
        GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")
    );
    if (!expect(create != nullptr, "D3D10CreateDevice1 is unavailable"))
      return verdict();
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, legacy_level, D3D10_1_SDK_VERSION, &device10));
    CHECK(device10.As(&device));
    device->GetImmediateContext(&immediate);
  } else {
    CHECK(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate
    ));
  }
  CHECK(device->CreateDeferredContext(0, &deferred));
  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)));
  ComPtr<ID3D11DeviceContext1> immediate1, deferred1;
  CHECK(immediate.As(&immediate1));
  CHECK(deferred.As(&deferred1));
  auto copy_all = [&](ID3D11Resource *dst, ID3D11Resource *src) -> HRESULT {
    if (device10) {
      ComPtr<ID3D10Resource> from, to;
      auto hr = src->QueryInterface(IID_PPV_ARGS(&from));
      if (FAILED(hr))
        return hr;
      hr = dst->QueryInterface(IID_PPV_ARGS(&to));
      if (FAILED(hr))
        return hr;
      device10->CopyResource(to.Get(), from.Get());
    } else
      immediate->CopyResource(dst, src);
    return S_OK;
  };
  auto copy_region = [&](ID3D11Resource *dst, UINT dst_sub, UINT x, UINT y, UINT z, ID3D11Resource *src, UINT src_sub,
                         const D3D11_BOX *box) -> HRESULT {
    ComPtr<ID3D10Resource> from, to;
    auto hr = src->QueryInterface(IID_PPV_ARGS(&from));
    if (FAILED(hr))
      return hr;
    hr = dst->QueryInterface(IID_PPV_ARGS(&to));
    if (FAILED(hr))
      return hr;
    D3D10_BOX region{};
    if (box)
      region = {box->left, box->top, box->front, box->right, box->bottom, box->back};
    device10->CopySubresourceRegion(to.Get(), dst_sub, x, y, z, from.Get(), src_sub, box ? &region : nullptr);
    return S_OK;
  };
  auto submit = [&](bool listed) -> HRESULT {
    if (!listed)
      return S_OK;
    ComPtr<ID3D11CommandList> commands;
    HRESULT hr = deferred->FinishCommandList(FALSE, &commands);
    if (SUCCEEDED(hr))
      immediate->ExecuteCommandList(commands.Get(), FALSE);
    return hr;
  };

  struct Image {
    ComPtr<ID3D11Resource> resource;
    std::vector<std::array<UINT, 3>> extents;
    std::vector<std::vector<UINT>> words;
    UINT mips, layers;
  };
  UINT serial = 0;
  auto make = [&](UINT dimension, std::array<UINT, 3> extent, UINT layers, bool staging, Image &image) -> HRESULT {
    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER || dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
      extent[1] = 1;
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE3D)
      extent[2] = 1;
    else
      layers = 1;
    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER)
      layers = 1;
    UINT mips = 1;
    for (UINT size = dimension == D3D11_RESOURCE_DIMENSION_BUFFER ? 1 : *std::max_element(extent.begin(), extent.end());
         size > 1; size >>= 1)
      mips++;
    image.mips = mips;
    image.layers = layers;
    image.extents.resize(mips * layers);
    image.words.resize(mips * layers);
    std::vector<D3D11_SUBRESOURCE_DATA> data(mips * layers);
    for (UINT layer = 0; layer < layers; layer++)
      for (UINT mip = 0; mip < mips; mip++) {
        UINT sub = D3D11CalcSubresource(mip, layer, mips);
        auto &e = image.extents[sub];
        for (UINT axis = 0; axis < e.size(); axis++)
          e[axis] = std::max(1u, extent[axis] >> mip);
        auto &words = image.words[sub];
        words.resize(e[0] * e[1] * e[2]);
        for (auto &word : words)
          word = ++serial;
        data[sub] = {words.data(), e[0] * word_bytes, e[0] * e[1] * word_bytes};
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D && staging) {
          data[sub].SysMemPitch = sub % 2 ? random() : 0;
          data[sub].SysMemSlicePitch = random();
        }
      }
    const auto usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    const UINT access = staging ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE : 0;
    switch (dimension) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      const D3D11_BUFFER_DESC desc{extent[0] * word_bytes, usage, 0, access};
      ComPtr<ID3D11Buffer> buffer;
      const HRESULT hr = device->CreateBuffer(&desc, data.data(), &buffer);
      image.resource = buffer;
      return hr;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC desc{extent[0], mips, layers, DXGI_FORMAT_R32_UINT, usage, 0, access};
      ComPtr<ID3D11Texture1D> texture;
      HRESULT hr = device->CreateTexture1D(&desc, data.data(), &texture);
      image.resource = texture;
      return hr;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC desc{extent[0], extent[1], mips, layers, DXGI_FORMAT_R32_UINT, {1, 0}, usage, 0, access};
      ComPtr<ID3D11Texture2D> texture;
      HRESULT hr = device->CreateTexture2D(&desc, data.data(), &texture);
      image.resource = texture;
      return hr;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC desc{extent[0], extent[1], extent[2], mips, DXGI_FORMAT_R32_UINT, usage, 0, access};
      ComPtr<ID3D11Texture3D> texture;
      HRESULT hr = device->CreateTexture3D(&desc, data.data(), &texture);
      image.resource = texture;
      return hr;
    }
    default:
      return E_INVALIDARG;
    }
  };
  auto check = [&](Image &image, Image &readback) -> int {
    if (!expect(SUCCEEDED(copy_all(readback.resource.Get(), image.resource.Get())), "CopyResource interfaces"))
      return 1;
    for (UINT sub = 0; sub < image.words.size(); sub++) {
      D3D11_MAPPED_SUBRESOURCE mapped{};
      HRESULT hr = immediate->Map(readback.resource.Get(), sub, D3D11_MAP_READ, 0, &mapped);
      if (!expect(SUCCEEDED(hr), "Map subresource %u returned %#lx", sub, hr))
        return 1;
      auto e = image.extents[sub];
      for (UINT z = 0; z < e[2]; z++)
        for (UINT y = 0; y < e[1]; y++)
          for (UINT x = 0; x < e[0]; x++) {
            UINT got;
            memcpy(&got, (const char *)mapped.pData + z * mapped.DepthPitch + y * mapped.RowPitch + x * sizeof(UINT),
                   sizeof(got));
            UINT want = image.words[sub][(z * e[1] + y) * e[0] + x];
            if (!expect(got == want, "subresource %u texel (%u,%u,%u): %#x, want %#x", sub, x, y, z, got, want)) {
              immediate->Unmap(readback.resource.Get(), sub);
              return 1;
            }
          }
      immediate->Unmap(readback.resource.Get(), sub);
    }
    return 0;
  };
  const UINT page_words = MULLION_PAGE_SIZE / sizeof(UINT);
  const UINT widths[] = {1, page_words - 1, page_words, page_words + 1};
  UINT variant = 0;
  for (UINT src_dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
       src_dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D; src_dimension++)
    for (UINT dst_dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
         dst_dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D; dst_dimension++)
      for (UINT width : widths)
        for (UINT staging = 0; staging < 4; staging++)
          for (bool listed : {false, true}) {
            if (device10 && (listed || src_dimension != dst_dimension))
              continue;
            variant++;
            auto context = listed ? deferred1.Get() : immediate1.Get();
            const std::array<UINT, 3> extent{width, width == 1 ? 1u : 3u + random() % 5,
                                          width == 1 ? 1u : 2u + random() % 3};
            Image src, dst, src_readback, dst_readback;
            const UINT src_layers = width == 1 ? 1 : 3, dst_layers = width == 1 ? 1 : 2;
            step("seed %u variant %u: create dimensions %u -> %u, width %u, staging %u, listed %u",
                 seed, variant, src_dimension, dst_dimension, width, staging, listed);
            CHECK(make(src_dimension, extent, src_layers, staging & 1, src));
            auto dst_extent = extent;
            dst_extent[0] += width != 1;
            CHECK(make(dst_dimension, dst_extent, dst_layers, staging & 2, dst));
            CHECK(make(src_dimension, extent, src_layers, true, src_readback));
            CHECK(make(dst_dimension, dst_extent, dst_layers, true, dst_readback));
            if (check(src, src_readback) || check(dst, dst_readback))
              return verdict();
            if (device10 && staging == 0) {
              ComPtr<ID3D10Resource> destination;
              CHECK(dst.resource.As(&destination));
              for (UINT mode = 0; mode < 9; mode++) {
                step(
                    "D3D10 level=%#x dimension=%u width=%u UpdateSubresource mode=%u", legacy_level, dst_dimension,
                    width, mode
                );
                for (UINT sub = 0; sub < dst.words.size(); sub++) {
                  const auto e = dst.extents[sub];
                  std::array<UINT, 3> begin{}, end = e;
                  if (mode == 1)
                    for (UINT axis = 0; axis < e.size(); axis++)
                      begin[axis] = e[axis] - 1;
                  else if (mode == 2)
                    for (UINT axis = 0; axis < e.size(); axis++) {
                      begin[axis] = e[axis] > 1;
                      end[axis] -= e[axis] > 2;
                    }
                  else if (mode >= 3) {
                    const UINT axis = (mode - 3) / 2;
                    if ((mode - 3) % 2)
                      std::swap(begin[axis], end[axis]);
                    else
                      end[axis] = begin[axis];
                  }
                  const UINT row_words = e[0] + 1, slice_words = row_words * (e[1] + 1);
                  std::vector<UINT> upload(slice_words * e[2]);
                  for (auto &word : upload)
                    word = ++serial;
                  if (mode < 3)
                    for (UINT z = begin[2]; z < end[2]; z++)
                      for (UINT y = begin[1]; y < end[1]; y++)
                        for (UINT x = begin[0]; x < end[0]; x++)
                          dst.words[sub][(z * e[1] + y) * e[0] + x] =
                              upload[(z - begin[2]) * slice_words + (y - begin[1]) * row_words + x - begin[0]];
                  D3D10_BOX region{begin[0], begin[1], begin[2], end[0], end[1], end[2]};
                  device10->UpdateSubresource(
                      destination.Get(), sub, mode ? &region : nullptr, upload.data(), row_words * word_bytes,
                      slice_words * word_bytes
                  );
                  std::fill(upload.begin(), upload.end(), 0);
                }
                if (check(dst, dst_readback))
                  return verdict();
              }
            }
            for (UINT mip : {0u, src.mips - 1})
              for (UINT mode = 0; mode < 9; mode++) {
                UINT src_sub = D3D11CalcSubresource(mip, random() % src.layers, src.mips);
                UINT dst_mip = random() % dst.mips, dst_layer = random() % dst.layers;
                UINT dst_sub = D3D11CalcSubresource(dst_mip, dst_layer, dst.mips);
                auto s = src.extents[src_sub], d = dst.extents[dst_sub];
                std::array<UINT, 3> begin{}, end{}, placed{};
                bool whole = mode == 0;
                for (UINT axis = 0; axis < s.size(); axis++) {
                  whole &= s[axis] <= d[axis];
                  UINT count = mode == 1 ? std::min(s[axis], d[axis])
                                         : 1 + random() % std::min(s[axis], d[axis]);
                  begin[axis] = mode == 1 ? s[axis] - count : random() % (s[axis] - count + 1);
                  placed[axis] = mode == 1 ? d[axis] - count : random() % (d[axis] - count + 1);
                  end[axis] = begin[axis] + count;
                }
                if (whole) {
                  begin = {};
                  end = s;
                  for (UINT axis = 0; axis < s.size(); axis++)
                    placed[axis] = random() % (d[axis] - s[axis] + 1);
                }
                if (mode >= 3) {
                  UINT axis = (mode - 3) / 2;
                  if ((mode - 3) % 2)
                    std::swap(begin[axis], end[axis]);
                  else
                    end[axis] = begin[axis];
                }
                D3D11_BOX box{begin[0], begin[1], begin[2], end[0], end[1], end[2]};
                step("seed %u variant %u mip %u mode %u: sub %u -> %u, box (%u,%u,%u)-(%u,%u,%u), at (%u,%u,%u)",
                     seed, variant, mip, mode, src_sub, dst_sub, box.left, box.top, box.front, box.right, box.bottom,
                     box.back, placed[0], placed[1], placed[2]);
                if (device10) {
                  CHECK(copy_region(
                      dst.resource.Get(), dst_sub, placed[0], placed[1], placed[2], src.resource.Get(), src_sub,
                      whole ? nullptr : &box
                  ));
                } else if (mode % 2)
                  context->CopySubresourceRegion1(dst.resource.Get(), dst_sub, placed[0], placed[1], placed[2],
                                                  src.resource.Get(), src_sub, whole ? nullptr : &box, 0);
                else
                  context->CopySubresourceRegion(dst.resource.Get(), dst_sub, placed[0], placed[1], placed[2],
                                                 src.resource.Get(), src_sub, whole ? nullptr : &box);
                CHECK(submit(listed));
                if (mode < 3)
                  for (UINT z = begin[2]; z < end[2]; z++)
                    for (UINT y = begin[1]; y < end[1]; y++)
                      for (UINT x = begin[0]; x < end[0]; x++)
                        dst.words[dst_sub][((placed[2] + z - begin[2]) * d[1] + placed[1] + y - begin[1]) * d[0] +
                                            placed[0] + x - begin[0]] = src.words[src_sub][(z * s[1] + y) * s[0] + x];
                if (check(dst, dst_readback) || check(src, src_readback))
                  return verdict();
              }
          }

  for (UINT width : widths)
    for (UINT staging = 0; staging < 4; staging++)
      for (bool listed : {false, true}) {
        if (device10 && listed)
          continue;
        auto context = listed ? deferred1.Get() : immediate1.Get();
        variant++;
        std::vector<UINT> initial(width), source(width);
        for (UINT i = 0; i < width; i++) {
          initial[i] = ++serial;
          source[i] = ++serial;
        }
        ComPtr<ID3D11Buffer> src;
        D3D11_BUFFER_DESC desc{width * word_bytes, staging & 1 ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT, 0,
                              staging & 1 ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE : 0u};
        D3D11_SUBRESOURCE_DATA data{source.data()};
        CHECK(device->CreateBuffer(&desc, &data, &src));
        desc.Usage = staging & 2 ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = staging & 2 ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE : 0;
        for (UINT mode = 0; mode < 8; mode++) {
          step("seed %u variant %u buffer width %u staging %u listed %u mode %u",
               seed, variant, width, staging, listed, mode);
          ComPtr<ID3D11Buffer> dst;
          data.pSysMem = initial.data();
          CHECK(device->CreateBuffer(&desc, &data, &dst));
          D3D11_BOX box{0, 0, 0, desc.ByteWidth, 1, 1};
          if (mode >= 2) {
            UINT *begin[] = {&box.left, &box.top, &box.front}, *end[] = {&box.right, &box.bottom, &box.back};
            UINT axis = (mode - 2) / 2;
            if ((mode - 2) % 2)
              std::swap(*begin[axis], *end[axis]);
            else
              *end[axis] = *begin[axis];
          }
          if (device10) {
            CHECK(copy_region(dst.Get(), 0, 0, 0, 0, src.Get(), 0, mode == 0 ? nullptr : &box));
          } else
            context->CopySubresourceRegion(dst.Get(), 0, 0, 0, 0, src.Get(), 0, mode == 0 ? nullptr : &box);
          CHECK(submit(listed));
          auto got = read(device.Get(), immediate.Get(), dst.Get());
          if (!expect(got == (mode < 2 ? source : initial), "buffer copy returned wrong words"))
            return verdict();
          if (device10 && staging == 0) {
            ComPtr<ID3D10Buffer> destination;
            CHECK(dst.As(&destination));
            auto expected = mode < 2 ? source : initial;
            for (UINT update = 0; update < 3; update++) {
              auto upload = source;
              for (auto &word : upload)
                word = ++serial;
              D3D10_BOX region{word_bytes * (width - 1), 0, 0, width * word_bytes, 1, 1};
              if (!update)
                expected = upload;
              else if (update == 1)
                expected.back() = upload.front();
              else
                region.right = region.left;
              device10->UpdateSubresource(destination.Get(), 0, update ? &region : nullptr, upload.data(), 0, 0);
              std::fill(upload.begin(), upload.end(), 0);
              got = read(device.Get(), immediate.Get(), dst.Get());
              if (!expect(got == expected, "buffer UpdateSubresource changed the wrong bytes"))
                return verdict();
            }
          }
        }
      }
  for (UINT dimension = D3D11_RESOURCE_DIMENSION_BUFFER; dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D; dimension++)
    for (UINT width : {1u, 7u, 8u, 9u, 31u, 32u, 33u, page_words - 1, page_words, page_words + 1})
      for (bool listed : {false, true})
        for (UINT flags : {0u, UINT(D3D11_COPY_NO_OVERWRITE), UINT(D3D11_COPY_DISCARD)}) {
          auto context = listed ? deferred1.Get() : immediate1.Get();
          Image src, dst, before, after, source_after;
          const std::array<UINT, 3> extent{width, 3, 2};
          CHECK(make(dimension, extent, 3, false, src));
          CHECK(make(dimension, extent, 3, false, dst));
          CHECK(make(dimension, extent, 3, true, before));
          CHECK(make(dimension, extent, 3, true, after));
          CHECK(make(dimension, extent, 3, true, source_after));
          for (UINT sub = 0; sub < src.words.size(); sub++)
            for (UINT empty_axis = 0; empty_axis <= extent.size(); empty_axis++) {
              const auto e = src.extents[sub];
              std::array<UINT, 3> count, placed;
              for (UINT axis = 0; axis < count.size(); axis++) {
                count[axis] = std::max(1u, e[axis] / 2);
                placed[axis] = e[axis] - count[axis];
              }
              if (empty_axis)
                count[empty_axis - 1] = 0;
              const bool empty = empty_axis != 0;
              step("copy flags dimension %u width %u listed %u flags %#x sub %u empty axis %u",
                   dimension, width, listed, flags, sub, empty_axis);
              context->UpdateSubresource1(dst.resource.Get(), sub, nullptr, dst.words[sub].data(),
                                           e[0] * word_bytes, e[0] * e[1] * word_bytes, D3D11_COPY_DISCARD);
              context->UpdateSubresource(src.resource.Get(), sub, nullptr, src.words[sub].data(),
                                         e[0] * word_bytes, e[0] * e[1] * word_bytes);
              if (flags != D3D11_COPY_NO_OVERWRITE)
                context->CopySubresourceRegion(before.resource.Get(), sub, 0, 0, 0, dst.resource.Get(), sub, nullptr);
              const UINT unit = dimension == D3D11_RESOURCE_DIMENSION_BUFFER ? word_bytes : 1;
              const D3D11_BOX box{0, 0, 0, count[0] * unit, count[1], count[2]};
              context->CopySubresourceRegion1(dst.resource.Get(), sub, placed[0] * unit, placed[1], placed[2],
                                               src.resource.Get(), sub, &box, flags);
              auto changed = src.words[sub];
              for (auto &word : changed)
                word = ~word;
              context->UpdateSubresource(src.resource.Get(), sub, nullptr, changed.data(),
                                         e[0] * word_bytes, e[0] * e[1] * word_bytes);
              context->CopySubresourceRegion(after.resource.Get(), sub, 0, 0, 0, dst.resource.Get(), sub, nullptr);
              context->CopySubresourceRegion(source_after.resource.Get(), sub, 0, 0, 0, src.resource.Get(), sub, nullptr);
              CHECK(submit(listed));
              Image *captured[] = {&before, &after, &source_after};
              for (UINT capture = flags == D3D11_COPY_NO_OVERWRITE ? 1 : 0; capture < std::size(captured); capture++) {
                D3D11_MAPPED_SUBRESOURCE mapped;
                CHECK(immediate->Map(captured[capture]->resource.Get(), sub, D3D11_MAP_READ, 0, &mapped));
                bool ok = true;
                for (UINT z = 0; z < e[2] && ok; z++)
                  for (UINT y = 0; y < e[1] && ok; y++)
                    for (UINT x = 0; x < e[0] && ok; x++) {
                      const bool copied = !empty && x >= placed[0] && y >= placed[1] && z >= placed[2];
                      if (capture == 1 && flags == D3D11_COPY_DISCARD && !empty && !copied)
                        continue;
                      const UINT index = (z * e[1] + y) * e[0] + x;
                      UINT want = capture == 2 ? changed[index] : dst.words[sub][index];
                      if (capture == 1 && copied)
                        want = src.words[sub][((z - placed[2]) * e[1] + y - placed[1]) * e[0] + x - placed[0]];
                      UINT got;
                      memcpy(&got, static_cast<char *>(mapped.pData) + z * mapped.DepthPitch + y * mapped.RowPitch +
                                       x * word_bytes, sizeof(got));
                      ok = expect(got == want, "capture %u texel (%u,%u,%u): %#x, want %#x", capture, x, y, z, got, want);
                    }
                immediate->Unmap(captured[capture]->resource.Get(), sub);
                if (!ok)
                  return verdict();
              }
            }
        }
  if (!options.CopyWithOverlap) {
    step("CopyWithOverlap is not advertised at feature level 11.0");
    return verdict();
  }
  for (UINT dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
       dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D; dimension++)
    for (UINT width : {3u, page_words - 1, page_words, page_words + 1})
      for (bool staging : {false, true})
        for (bool listed : {false, true}) {
          Image image, readback;
          const std::array<UINT, 3> extent{width, 5, 3};
          CHECK(make(dimension, extent, 3, staging, image));
          CHECK(make(dimension, extent, 3, true, readback));
          auto context = listed ? deferred1.Get() : immediate1.Get();
          step("identical texture copies dimension %u width %u staging %u listed %u", dimension, width, staging,
               listed);
          for (UINT sub = 0; sub < image.words.size(); sub++)
            context->CopySubresourceRegion1(image.resource.Get(), sub, 0, 0, 0, image.resource.Get(), sub, nullptr, 0);
          CHECK(submit(listed));
          if (check(image, readback))
            return verdict();
          for (UINT sub = 0; sub < image.words.size(); sub++)
            for (UINT axis = 0; axis < extent.size(); axis++) {
              const auto e = image.extents[sub];
              if (e[axis] < 2)
                continue;
              for (bool forward : {false, true}) {
                step("overlapping texture dimension %u width %u sub %u axis %u forward %u staging %u listed %u",
                     dimension, width, sub, axis, forward, staging, listed);
                const auto snapshot = image.words[sub];
                std::array<UINT, 3> begin{}, end = e, placed{};
                begin[axis] = forward ? 0 : 1;
                end[axis] -= forward ? 1 : 0;
                placed[axis] = forward ? 1 : 0;
                const D3D11_BOX box{begin[0], begin[1], begin[2], end[0], end[1], end[2]};
                context->CopySubresourceRegion1(image.resource.Get(), sub, placed[0], placed[1], placed[2],
                                                image.resource.Get(), sub, &box, 0);
                CHECK(submit(listed));
                for (UINT z = begin[2]; z < end[2]; z++)
                  for (UINT y = begin[1]; y < end[1]; y++)
                    for (UINT x = begin[0]; x < end[0]; x++)
                      image.words[sub][((z - begin[2] + placed[2]) * e[1] + y - begin[1] + placed[1]) * e[0] +
                                       x - begin[0] + placed[0]] = snapshot[(z * e[1] + y) * e[0] + x];
                if (check(image, readback))
                  return verdict();
              }
            }
        }
  for (UINT width : {3u, page_words - 1, page_words, page_words + 1})
    for (bool staging : {false, true})
      for (bool listed : {false, true})
        for (bool forward : {false, true})
          for (UINT shift : {1u, word_bytes, word_bytes + 1})
            for (UINT bytes : {0u, 1u, shift, shift + 1, width * word_bytes - shift}) {
              step("buffer words %u bytes %u shift %u forward %u staging %u listed %u", width, bytes, shift, forward,
                   staging, listed);
              std::vector<UINT> words(width);
              for (auto &word : words)
                word = ++serial;
              D3D11_BUFFER_DESC desc{width * word_bytes, staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT, 0,
                                    staging ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE : 0u};
              D3D11_SUBRESOURCE_DATA initial{words.data()};
              ComPtr<ID3D11Buffer> resource;
              CHECK(device->CreateBuffer(&desc, &initial, &resource));
              const UINT source = forward ? 0 : shift, dest = forward ? shift : 0;
              const D3D11_BOX box{source, 0, 0, source + bytes, 1, 1};
              (listed ? deferred1.Get() : immediate1.Get())->CopySubresourceRegion1(resource.Get(), 0, dest, 0, 0,
                                                                                  resource.Get(), 0, &box, 0);
              CHECK(submit(listed));
              memmove(reinterpret_cast<char *>(words.data()) + dest, reinterpret_cast<char *>(words.data()) + source,
                      bytes);
              expect(read(device.Get(), immediate.Get(), resource.Get()) == words,
                     "overlap differs from pre-copy bytes");
            }
  return verdict();
}

int
main(int argc, char **argv) {
  for (auto level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1})
    if (int status = copying(argc, argv, level))
      return status;
  return copying(argc, argv);
}
