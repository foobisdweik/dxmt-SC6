// contract: DIRECT, COMPUTE and BUNDLE lists answer ID3D12GraphicsCommandList8; references and masks are independent.
// "front and back stencils in D3D12 can be specified with both separate stencil mask and separate reference values."
// (Agility SDK 1.706.3, Independent Front/Back Stencil Refs and Masks).
// https://devblogs.microsoft.com/directx/preview-agility-sdk-1-706-3-preview-sm-6-7-enhanced-barriers-and-more/
// bundle state "affects the state of its parent direct command list." (Graphics pipeline state inheritance).
// https://learn.microsoft.com/windows/win32/direct3d12/managing-graphics-pipeline-state-in-direct3d-12
// D3D11.3 15.12: "FrontCounterClockwise governs whether clockwise primitives are considered front- or back- facing."
// each case has three columns: direct, bundle, then parent, with opposite windings on separate rows. REPLACE writes
// (old & ~write_mask) | (reference & write_mask); EQUAL compares (stored & read_mask) to (reference & read_mask).
// full-mask reads with a shared reference check the writes independently of the per-face reference setter.
// mask semantics: D3D12_DEPTH_STENCIL_DESC (StencilReadMask, StencilWriteMask), and D3D12_STENCIL_OP_REPLACE.
// https://learn.microsoft.com/windows/win32/api/d3d12/ns-d3d12-d3d12_depth_stencil_desc
// https://learn.microsoft.com/windows/win32/api/d3d12/ne-d3d12-d3d12_stencil_op
#include "d3d12_test.hpp"

#ifndef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
#define __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
MIDL_INTERFACE("ee936ef9-599d-4d28-938e-23c4ad05ce51")
ID3D12GraphicsCommandList8 : public ID3D12GraphicsCommandList7 {
  virtual void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT FrontStencilRef, UINT BackStencilRef) = 0;
};
__CRT_UUID_DECL(ID3D12GraphicsCommandList8, 0xee936ef9, 0x599d, 0x4d28, 0x93, 0x8e, 0x23, 0xc4, 0xad, 0x05, 0xce, 0x51)
// DirectX-Headers, d3d12.idl; absent from the installed SDK together with interface 8
constexpr auto D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2 = (D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)26;
struct D3D12_DEPTH_STENCILOP_DESC1 {
  D3D12_STENCIL_OP StencilFailOp, StencilDepthFailOp, StencilPassOp;
  D3D12_COMPARISON_FUNC StencilFunc;
  UINT8 StencilReadMask, StencilWriteMask;
};
struct D3D12_DEPTH_STENCIL_DESC2 {
  BOOL DepthEnable;
  D3D12_DEPTH_WRITE_MASK DepthWriteMask;
  D3D12_COMPARISON_FUNC DepthFunc;
  BOOL StencilEnable;
  D3D12_DEPTH_STENCILOP_DESC1 FrontFace, BackFace;
  BOOL DepthBoundsTestEnable;
};
#endif

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Data> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  Data data{};
};

static const char hlsl[] = R"hlsl(
float4 cover(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
#ifdef REVERSED
  uv = uv.yx;
#endif
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint ps(bool front : SV_IsFrontFace) : SV_Target { return front ? 1 : 2; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const std::string vs[] = {compiler.compile(hlsl, "cover", "vs"),
                            compiler.compile(hlsl, "cover", "vs", {"REVERSED=1"})};
  auto ps = compiler.compile(hlsl, "ps", "ps");
  if (vs[0].empty() || vs[1].empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device2> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  step("OPTIONS14 promises independent references and masks, with no unrelated features");
  D3D12_FEATURE_DATA_D3D12_OPTIONS14 options{TRUE, TRUE, FALSE};
  HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &options, sizeof(options));
  expect(hr == S_OK && options.IndependentFrontAndBackStencilRefMaskSupported &&
             !options.AdvancedTextureOpsSupported && !options.WriteableMSAATexturesSupported,
         "OPTIONS14 returned %#lx, advanced %d, MSAA %d, independent %d", hr, options.AdvancedTextureOpsSupported,
         options.WriteableMSAATexturesSupported, options.IndependentFrontAndBackStencilRefMaskSupported);
  for (UINT size : {UINT(sizeof(options) - 1), UINT(sizeof(options) + 1)})
    expect(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &options, size) == E_INVALIDARG,
           "OPTIONS14 accepted size %u", size);
  step("COMPUTE command list answers ID3D12GraphicsCommandList8");
  ComPtr<ID3D12CommandAllocator> compute_allocator;
  ComPtr<ID3D12GraphicsCommandList> compute;
  ComPtr<ID3D12GraphicsCommandList8> compute8;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&compute_allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, compute_allocator.Get(), nullptr,
                                IID_PPV_ARGS(&compute)));
  hr = compute.As(&compute8);
  if (!expect(hr == S_OK && compute8, "COMPUTE QueryInterface returned %#lx", hr))
    return verdict();
  CHECK(compute8->Close());

  auto rs = root_signature(device.Get(), {});
  const DXGI_FORMAT format = DXGI_FORMAT_R32_UINT, depth_format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  const UINT refs[] = {0, 0x5a, D3D12_DEFAULT_STENCIL_READ_MASK, D3D12_DEFAULT_STENCIL_READ_MASK + 1u, ~0u};
  struct References { UINT front, back; bool single; };
  std::vector<References> cases;
  for (UINT front : refs)
    for (UINT back : refs)
      cases.push_back({front, back, false});
  for (UINT ref : refs)
    cases.push_back({ref, ref, true});
  const UINT8 initial = 0x5a;
  const UINT8 masks[][2] = {{0, D3D12_DEFAULT_STENCIL_READ_MASK}, {0x0f, 0xf0},
                            {0xf0, 0x0f}, {D3D12_DEFAULT_STENCIL_READ_MASK, 0}};
  const UINT columns_per_case = 3, faces = std::size(vs), modes = 2;
  const UINT width = columns_per_case * cases.size(), height = modes * std::size(masks) * faces;
  struct {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2, D3D12_DEPTH_STENCIL_DESC2> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depth_format;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
  } stream{};
  stream.root.data = rs.Get(), stream.ps.data = bytecode(ps);
  stream.depth_format.data = depth_format;
  stream.targets.data = {{format}, 1};
  stream.topology.data = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  stream.raster.data = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  stream.raster.data.DepthClipEnable = TRUE;
  auto pipeline = [&](UINT face, bool write, UINT8 front_mask, UINT8 back_mask) {
    stream.vs.data = bytecode(vs[face]);
    stream.depth.data = {FALSE, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS, TRUE};
    auto op = write ? D3D12_STENCIL_OP_REPLACE : D3D12_STENCIL_OP_KEEP;
    auto compare = write ? D3D12_COMPARISON_FUNC_ALWAYS : D3D12_COMPARISON_FUNC_EQUAL;
    stream.depth.data.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, op, compare,
                                  write ? UINT8(D3D12_DEFAULT_STENCIL_READ_MASK) : front_mask,
                                  write ? front_mask : UINT8(0)};
    stream.depth.data.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, op, compare,
                                 write ? UINT8(D3D12_DEFAULT_STENCIL_READ_MASK) : back_mask,
                                 write ? back_mask : UINT8(0)};
    stream.blend.data.RenderTarget[0].RenderTargetWriteMask = write ? 0 : D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
    ComPtr<ID3D12PipelineState> pso;
    HRESULT result = device->CreatePipelineState(&desc, IID_PPV_ARGS(&pso));
    expect(result == S_OK && pso, "face %u, write %d, masks %#x/%#x: pipeline %#lx", face, write,
           front_mask, back_mask, result);
    return pso;
  };
  ComPtr<ID3D12PipelineState> psos[modes][std::size(masks)][faces], verify[faces];
  for (UINT face = 0; face < faces; face++) {
    verify[face] = pipeline(face, false, D3D12_DEFAULT_STENCIL_READ_MASK, D3D12_DEFAULT_STENCIL_READ_MASK);
    if (!verify[face])
      return verdict();
    for (UINT mode = 0; mode < modes; mode++)
      for (UINT mask = 0; mask < std::size(masks); mask++) {
        psos[mode][mask][face] = pipeline(face, mode == 0, masks[mask][0], masks[mask][1]);
        if (!psos[mode][mask][face])
          return verdict();
      }
  }

  step("pipeline library identity includes both back masks");
  ComPtr<ID3D12PipelineLibrary1> library;
  CHECK(device->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library)));
  CHECK(library->StorePipeline(L"stencil", psos[modes - 1][std::size(masks) - 1][faces - 1].Get()));
  D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
  ComPtr<ID3D12PipelineState> loaded;
  CHECK(library->LoadPipeline(L"stencil", &stream_desc, IID_PPV_ARGS(&loaded)));
  for (auto member : {&D3D12_DEPTH_STENCILOP_DESC1::StencilReadMask, &D3D12_DEPTH_STENCILOP_DESC1::StencilWriteMask}) {
    stream.depth.data.BackFace.*member ^= 1;
    loaded.Reset();
    hr = library->LoadPipeline(L"stencil", &stream_desc, IID_PPV_ARGS(&loaded));
    expect(hr == E_INVALIDARG && !loaded, "a different back mask loaded: %#lx", hr);
    stream.depth.data.BackFace.*member ^= 1;
  }

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  auto depth_desc = target_desc;
  depth_desc.Format = depth_format, depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  ComPtr<ID3D12Resource> target, depth;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT row;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &row, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  ComPtr<ID3D12GraphicsCommandList8> list8;
  step("DIRECT command list answers ID3D12GraphicsCommandList8");
  hr = list.As(&list8);
  if (!expect(hr == S_OK && list8, "DIRECT QueryInterface returned %#lx", hr))
    return verdict();
  ComPtr<ID3D12CommandAllocator> bundle_allocator;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  std::vector<ComPtr<ID3D12GraphicsCommandList8>> bundles;

  const float zero[4] = {};
  list8->ClearRenderTargetView(rtv, zero, 0, nullptr);
  list8->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1, initial, 0, nullptr);
  list8->SetGraphicsRootSignature(rs.Get());
  list8->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
  list8->RSSetViewports(1, &viewport);
  list8->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  for (UINT mode = 0; mode < modes; mode++) {
    for (UINT mask = 0; mask < std::size(masks); mask++) {
      for (UINT face = 0; face < faces; face++) {
        const UINT y = (mode * std::size(masks) + mask) * faces + face;
        auto &pso = psos[mode][mask][face];
        list8->SetPipelineState(pso.Get());
        for (UINT c = 0; c < cases.size(); c++) {
          auto &k = cases[c];
          step("mode %u, masks %#x/%#x, face %u, references %#x/%#x, shared %d: direct, bundle, parent",
               mode, masks[mask][0], masks[mask][1], face, k.front, k.back, k.single);
          D3D12_RECT scissor{LONG(c * columns_per_case), LONG(y), LONG(c * columns_per_case + 1), LONG(y + 1)};
          list8->RSSetScissorRects(1, &scissor);
          list8->OMSetFrontAndBackStencilRef(k.front, k.back ^ 1);
          if (k.single)
            list8->OMSetStencilRef(k.front);
          else
            list8->OMSetFrontAndBackStencilRef(k.front, k.back);
          list8->DrawInstanced(3, 1, 0, 0);

          ComPtr<ID3D12GraphicsCommandList> bundle;
          CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), pso.Get(),
                                        IID_PPV_ARGS(&bundle)));
          auto &bundle8 = bundles.emplace_back();
          hr = bundle.As(&bundle8);
          if (!expect(hr == S_OK && bundle8, "BUNDLE QueryInterface returned %#lx", hr))
            return verdict();
          if (k.single)
            bundle8->OMSetStencilRef(k.front);
          else
            bundle8->OMSetFrontAndBackStencilRef(k.front, k.back);
          bundle8->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          bundle8->DrawInstanced(3, 1, 0, 0);
          CHECK(bundle8->Close());
          scissor.left++, scissor.right++;
          list8->RSSetScissorRects(1, &scissor);
          list8->OMSetFrontAndBackStencilRef(k.front, k.back ^ 1);
          list8->ExecuteBundle(bundle8.Get());
          scissor.left++, scissor.right++;
          list8->RSSetScissorRects(1, &scissor);
          list8->DrawInstanced(3, 1, 0, 0);
        }
        if (mode == 0) {
          list8->SetPipelineState(verify[face].Get());
          for (UINT c = 0; c < cases.size(); c++) {
            auto &k = cases[c];
            UINT8 ref = face ? k.back : k.front, write_mask = masks[mask][face];
            UINT8 stored = (initial & ~write_mask) | (ref & write_mask);
            list8->OMSetStencilRef(stored);
            D3D12_RECT scissor{LONG(c * columns_per_case), LONG(y),
                               LONG((c + 1) * columns_per_case), LONG(y + 1)};
            list8->RSSetScissorRects(1, &scissor);
            list8->DrawInstanced(3, 1, 0, 0);
          }
        }
      }
    }
  }
  transition(list8.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
      dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = row}};
  list8->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list8.Get()));

  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  step("read references and masks after direct, bundle and parent draws");
  for (UINT y = 0; y < height; y++) {
    const UINT face = y % faces, mask = (y / faces) % std::size(masks), mode = y / (faces * std::size(masks));
    auto pixels = (const UINT *)((const char *)got + y * row.Footprint.RowPitch);
    for (UINT column = 0; column < width; column++) {
      auto &k = cases[column / columns_per_case];
      UINT8 ref = face ? k.back : k.front, read_mask = masks[mask][face];
      UINT want = mode == 0 || (initial & read_mask) == (ref & read_mask) ? face + 1 : 0;
      expect(pixels[column] == want, "mode %u, masks %#x/%#x, face %u, column %u, references %#x/%#x: %u, want %u",
             mode, masks[mask][0], masks[mask][1], face, column, k.front, k.back, pixels[column], want);
    }
  }
  readback->Unmap(0, nullptr);
  return verdict();
}
