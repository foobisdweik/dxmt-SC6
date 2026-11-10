// contract: ResizeBuffers1 assigns a creation node and a presentation queue to
// each back buffer. "the swapchain will also rotate through these command
// queues." (ResizeBuffers1, ppPresentQueue.)
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-resizebuffers1
// a fence holds the selected queue. completing every other queue cannot release
// the frame's waitable credit; releasing the selected queue must. copied bytes
// and the window's pixel identify the presented buffer.
// "Synchronize presentation for at least n vertical blanks." (Present, SyncInterval 1 through 4.)
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present
// overlapping presents must precede subsequent writes when their back buffer becomes current again.
#include "dxgi_test.hpp"
#include <array>
#include <limits>

int
main() {
  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");
  DxgiDevice graphics;
  CHECK(graphics.create(12));
  auto device = graphics.d12.Get();
  const UINT nodes = device->GetNodeCount();
  if (!expect(nodes && nodes < std::numeric_limits<UINT>::digits, "unsupported node count %u", nodes))
    return verdict();
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  std::array<ComPtr<ID3D12CommandQueue>, 2> queues;
  for (auto &queue : queues)
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> empty_allocator;
  ComPtr<ID3D12GraphicsCommandList> empty;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&empty_allocator)));
  CHECK(
      device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, empty_allocator.Get(), nullptr, IID_PPV_ARGS(&empty))
  );
  CHECK(empty->Close());
  WNDCLASSA wc{0,       DefWindowProcA, 0,       0,       GetModuleHandleA(nullptr),
               nullptr, nullptr,        nullptr, nullptr, "dxgi_resize_queues"};
  if (!expect(RegisterClassA(&wc), "RegisterClass"))
    return verdict();
  constexpr UINT width = 17, height = 13;
  HWND window = CreateWindowA(
      wc.lpszClassName, wc.lpszClassName, WS_POPUP | WS_VISIBLE, 0, 0, width, height, nullptr, nullptr, wc.hInstance,
      nullptr
  );
  if (!expect(window != nullptr, "CreateWindow"))
    return verdict();
  UpdateWindow(window);
  ComPtr<IDXGIFactory2> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<ID3D12CommandQueue> compute, copy;
  for (auto [type, queue] :
       {std::pair{D3D12_COMMAND_LIST_TYPE_COMPUTE, &compute}, std::pair{D3D12_COMMAND_LIST_TYPE_COPY, &copy}}) {
    D3D12_COMMAND_QUEUE_DESC desc{type};
    CHECK(device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue)));
  }
  DxgiDevice other;
  CHECK(other.create(12));
  for (auto effect : {DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_SWAP_EFFECT_FLIP_DISCARD}) {
    DXGI_SWAP_CHAIN_DESC1 desc{width,
                               height,
                               DXGI_FORMAT_R8G8B8A8_UNORM,
                               FALSE,
                               {1, 0},
                               DXGI_USAGE_RENDER_TARGET_OUTPUT,
                               2,
                               DXGI_SCALING_STRETCH,
                               effect};
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> first;
    CHECK(factory->CreateSwapChainForHwnd(graphics.input.Get(), window, &desc, nullptr, nullptr, &first));
    ComPtr<IDXGISwapChain3> chain;
    CHECK(first.As(&chain));
    HANDLE ready = chain->GetFrameLatencyWaitableObject();
    if (!expect(ready != nullptr, "no frame readiness handle"))
      return verdict();
    expect(WaitForSingleObject(ready, 0) == WAIT_OBJECT_0, "no initial credit");
    for (UINT count : {2u, 3u, UINT(DXGI_MAX_SWAP_CHAIN_BUFFERS)}) {
      step("effect %u resize %u buffers with alternating direct queues", effect, count);
      std::vector<UINT> masks(count, 1);
      // zero denotes the first node as it does in D3D12_HEAP_PROPERTIES (Wine's ResizeBuffers1 accepts both).
      for (UINT i = 0; i < masks.size(); i += 2)
        masks[i] = 0;
      std::vector<IUnknown *> present_queues(count);
      for (UINT i = 0; i < count; i++)
        present_queues[i] = queues[i % queues.size()].Get();
      CHECK(chain->ResizeBuffers1(
          count, width, height, DXGI_FORMAT_UNKNOWN, desc.Flags, masks.data(), present_queues.data()
      ));
      expect(chain->GetCurrentBackBufferIndex() < count, "resized current buffer is out of range");
      // flip buffers number at least two and at most
      // DXGI_MAX_SWAP_CHAIN_BUFFERS (ResizeBuffers1, BufferCount).
      for (UINT invalid : {1u, UINT(DXGI_MAX_SWAP_CHAIN_BUFFERS + 1)}) {
        expect(
            FAILED(chain->ResizeBuffers(invalid, width, height, DXGI_FORMAT_UNKNOWN, desc.Flags)), "buffer count %u",
            invalid
        );
        DXGI_SWAP_CHAIN_DESC1 after{};
        CHECK(chain->GetDesc1(&after));
        expect(after.BufferCount == count, "failed count resize changed buffers");
      }
      for (UINT frame = 0; frame <= count * 2; frame++) {
        UINT index = chain->GetCurrentBackBufferIndex(), selected = index % queues.size();
        step("effect %u buffers %u frame %u index %u queue %u", effect, count, frame, index, selected);
        if (!expect(index < count, "invalid current buffer index"))
          return verdict();
        ComPtr<ID3D12Resource> back;
        CHECK(chain->GetBuffer(index, IID_PPV_ARGS(&back)));
        D3D12_HEAP_PROPERTIES properties{};
        D3D12_HEAP_FLAGS heap_flags{};
        CHECK(back->GetHeapProperties(&properties, &heap_flags));
        // HEAP_PROPERTIES: zero is equivalent to one on a single GPU.
        expect(
            (properties.CreationNodeMask ? properties.CreationNodeMask : 1) == (masks[index] ? masks[index] : 1) &&
                (properties.VisibleNodeMask ? properties.VisibleNodeMask : 1) == (UINT(1) << nodes) - 1,
            "back buffer node masks %#x / %#x", properties.CreationNodeMask, properties.VisibleNodeMask
        );
        ComPtr<ID3D12DescriptorHeap> views;
        D3D12_DESCRIPTOR_HEAP_DESC view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
        CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&views)));
        auto rtv = views->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(back.Get(), nullptr, rtv);
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        CHECK(
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))
        );
        const BYTE value[] = {BYTE(frame + 1), BYTE(count), BYTE(effect), 255};
        const float color[] = {value[0] / 255.f, value[1] / 255.f, value[2] / 255.f, 1};
        transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ClearRenderTargetView(rtv, color, 0, nullptr);
        transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto resource_desc = back->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 size = 0;
        device->GetCopyableFootprints(&resource_desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
        auto readback = buffer(device, D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!expect(readback != nullptr, "readback allocation"))
          return verdict();
        CHECK(forget(readback.Get()));
        D3D12_TEXTURE_COPY_LOCATION source{back.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            target{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
        list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        CHECK(submit(device, queues[selected].Get(), list.Get()));
        BYTE *bytes = nullptr;
        CHECK(readback->Map(0, nullptr, reinterpret_cast<void **>(&bytes)));
        for (UINT y = 0; y < height; y++)
          for (UINT x = 0; x < width; x++)
            expect(
                !memcmp(bytes + y * footprint.Footprint.RowPitch + x * sizeof(value), value, sizeof(value)),
                "resized buffer pixel %u,%u differs", x, y
            );
        readback->Unmap(0, nullptr);
        ComPtr<ID3D12Fence> gate;
        CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        CHECK(queues[selected]->Wait(gate.Get(), 1));
        HRESULT presented = chain->Present(0, 0);
        HRESULT other_finished = execute(device, graphics.queue.Get(), empty.Get());
        for (UINT q = 0; q < queues.size(); q++)
          if (q != selected) {
            HRESULT hr = execute(device, queues[q].Get(), empty.Get());
            if (FAILED(hr))
              other_finished = hr;
          }
        DWORD premature = WaitForSingleObject(ready, 0);
        HRESULT released = gate->Signal(1);
        expect(
            presented == S_OK && other_finished == S_OK, "present/other queue failed: %#lx / %#lx", presented,
            other_finished
        );
        expect(premature == WAIT_TIMEOUT, "present used an unselected queue");
        CHECK(released);
        CHECK(execute(device, queues[selected].Get(), empty.Get()));
        if (premature == WAIT_TIMEOUT)
          expect(WaitForSingleObject(ready, 0) == WAIT_OBJECT_0, "selected queue did not release readiness");
        HDC dc = GetDC(window);
        COLORREF shown = dc ? GetPixel(dc, width / 2, height / 2) : CLR_INVALID;
        if (dc)
          ReleaseDC(window, dc);
        expect(shown == RGB(value[0], value[1], value[2]), "wrong resized buffer presented: %#lx", shown);
        expect(chain->GetCurrentBackBufferIndex() == (index + 1) % count, "present did not rotate buffers");
      }
      if (count == queues.size()) {
        step("effect %u overlapping queues and current-buffer reuse", effect);
        CHECK(chain->SetMaximumFrameLatency(3));
        while (WaitForSingleObject(ready, 0) == WAIT_OBJECT_0) {
        }
        ComPtr<ID3D12CommandAllocator> allocator;
        CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        std::array<ComPtr<ID3D12Resource>, 2> back;
        std::array<ComPtr<ID3D12GraphicsCommandList>, 3> lists;
        ComPtr<ID3D12DescriptorHeap> views;
        D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, UINT(back.size())};
        CHECK(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&views)));
        auto rtv = views->GetCPUDescriptorHandleForHeapStart();
        const auto stride = device->GetDescriptorHandleIncrementSize(heap.Type);
        const float colors[][4] = {{1, 0, 0, 1}, {0, 0, 1, 1}, {0, 1, 0, 1}};
        UINT first_index = chain->GetCurrentBackBufferIndex();
        for (UINT i = 0; i < back.size(); i++) {
          CHECK(chain->GetBuffer((first_index + i) % count, IID_PPV_ARGS(&back[i])));
          device->CreateRenderTargetView(back[i].Get(), nullptr, {rtv.ptr + i * stride});
        }
        for (UINT i = 0; i < lists.size(); i++) {
          CHECK(device->CreateCommandList(
              0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&lists[i])
          ));
          UINT buffer_index = i ? 1 : 0;
          transition(
              lists[i].Get(), back[buffer_index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET
          );
          lists[i]->ClearRenderTargetView({rtv.ptr + buffer_index * stride}, colors[i], 0, nullptr);
          transition(
              lists[i].Get(), back[buffer_index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT
          );
          CHECK(lists[i]->Close());
        }
        auto first_queue = queues[first_index % queues.size()].Get();
        auto second_queue = queues[((first_index + 1) % count) % queues.size()].Get();
        std::array<ID3D12CommandQueue *, 2> assigned{first_queue, second_queue};
        ComPtr<ID3D12Fence> gate;
        std::array<ComPtr<ID3D12Fence>, 2> markers;
        std::array<HANDLE, 2> completed{};
        CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        for (UINT i = 0; i < markers.size(); i++) {
          CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&markers[i])));
          completed[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
          if (!expect(completed[i] != nullptr, "overlap marker event"))
            return verdict();
          CHECK(markers[i]->SetEventOnCompletion(1, completed[i]));
        }
        CHECK(execute(device, first_queue, lists[0].Get()));
        CHECK(first_queue->Wait(gate.Get(), 1));
        CHECK(chain->Present(1, 0));
        CHECK(first_queue->Signal(markers[0].Get(), 1));
        CHECK(first_queue->Wait(gate.Get(), 2));
        CHECK(execute(device, second_queue, lists[1].Get()));
        CHECK(chain->Present(1, 0));
        CHECK(chain->Present(1, 0));
        // B is current again; its next clear must follow the presentation that reads its blue image.
        expect(chain->GetCurrentBackBufferIndex() == (first_index + 1) % count, "reused buffer is not current");
        ID3D12CommandList *overwrite[] = {lists[2].Get()};
        second_queue->ExecuteCommandLists(ARRAYSIZE(overwrite), overwrite);
        CHECK(second_queue->Signal(markers[1].Get(), 1));
        // allow a missing handoff to execute the overwrite while the first presentation is held.
        constexpr DWORD overlap_wait_ms = 1000;
        DWORD early = WaitForSingleObject(completed[1], overlap_wait_ms);
        DWORD premature = WaitForSingleObject(ready, 0);
        CHECK(gate->Signal(1));
        DWORD finished = WaitForSingleObject(completed[0], 10000);
        if (early == WAIT_TIMEOUT)
          finished = finished == WAIT_OBJECT_0 ? WaitForSingleObject(completed[1], 10000) : finished;
        COLORREF shown = CLR_INVALID;
        auto deadline = GetTickCount64() + 2000;
        do {
          HDC dc = GetDC(window);
          shown = dc ? GetPixel(dc, width / 2, height / 2) : CLR_INVALID;
          if (dc)
            ReleaseDC(window, dc);
          if (shown == RGB(0, 0, 255) || shown == RGB(0, 255, 0))
            break;
          Sleep(1);
        } while (GetTickCount64() < deadline);
        CHECK(gate->Signal(2));
        expect(early != WAIT_FAILED && finished == WAIT_OBJECT_0, "overlap marker wait failed");
        expect(premature == WAIT_TIMEOUT, "later queue bypassed an earlier pending synchronized present");
        expect(
            shown == RGB(0, 0, 255), "overlapping present read subsequent writes or displayed out of order: %#lx", shown
        );
        for (UINT i = 0; i < assigned.size(); i++) {
          CHECK(execute(device, assigned[i], empty.Get()));
          expect(CloseHandle(completed[i]), "CloseHandle overlap marker");
        }
        while (WaitForSingleObject(ready, 0) == WAIT_OBJECT_0) {
        }
        CHECK(chain->SetMaximumFrameLatency(1));
      }
      step("effect %u buffers %u reject nonexistent creation node", effect, count);
      masks.back() = UINT(1) << nodes;
      HRESULT hr = chain->ResizeBuffers1(
          count, width + 1, height + 1, DXGI_FORMAT_UNKNOWN, desc.Flags, masks.data(), present_queues.data()
      );
      expect(FAILED(hr), "nonexistent node accepted: %#lx", hr);
      DXGI_SWAP_CHAIN_DESC1 after{};
      CHECK(chain->GetDesc1(&after));
      expect(
          after.Width == width && after.Height == height && after.BufferCount == count,
          "rejected node resize changed buffers"
      );
      masks.back() = 1;
      auto saved_queue = present_queues.back();
      for (IUnknown *invalid :
           {static_cast<IUnknown *>(nullptr), static_cast<IUnknown *>(factory.Get()),
            static_cast<IUnknown *>(compute.Get()), static_cast<IUnknown *>(copy.Get()),
            static_cast<IUnknown *>(other.queue.Get())}) {
        if (invalid == other.queue.Get() && other.d12.Get() == device) {
          printf(
              "unchecked: D3D12CreateDevice reused the device; no "
              "distinct-device queue\n"
          );
          continue;
        }
        step("effect %u buffers %u reject queue %p", effect, count, invalid);
        present_queues.back() = invalid;
        expect(
            chain->ResizeBuffers1(
                0, width + 1, height + 1, DXGI_FORMAT_UNKNOWN, desc.Flags, masks.data(), present_queues.data()
            ) == DXGI_ERROR_INVALID_CALL,
            "invalid presentation queue accepted"
        );
        CHECK(chain->GetDesc1(&after));
        expect(
            after.Width == width && after.Height == height && after.BufferCount == count,
            "invalid queue changed buffers"
        );
      }
      present_queues.back() = saved_queue;
      masks.back() = (UINT(1) << nodes) | 1;
      expect(
          chain->ResizeBuffers1(
              0, width + 1, height + 1, DXGI_FORMAT_UNKNOWN, desc.Flags, masks.data(), present_queues.data()
          ) == DXGI_ERROR_INVALID_CALL,
          "multiple creation nodes accepted"
      );
      CHECK(chain->GetDesc1(&after));
      expect(after.Width == width && after.Height == height, "multiple creation nodes changed buffers");
      masks.back() = 1;
      // both arrays are required even for a count of zero (Wine's Windows
      // test_swapchain_resize).
      for (auto [use_masks, use_queues] : {std::pair{false, false}, {false, true}, {true, false}}) {
        step("effect %u buffers %u reject arrays: nodes %u queues %u", effect, count, use_masks, use_queues);
        const auto before = after;
        expect(
            chain->ResizeBuffers1(
                0, width + 1, height + 1, DXGI_FORMAT_UNKNOWN, desc.Flags, use_masks ? masks.data() : nullptr,
                use_queues ? present_queues.data() : nullptr
            ) == DXGI_ERROR_INVALID_CALL,
            "incomplete queue/node arrays accepted"
        );
        CHECK(chain->GetDesc1(&after));
        expect(after.Width == before.Width && after.Height == before.Height, "incomplete arrays changed buffers");
      }
      CHECK(
          chain->ResizeBuffers1(0, width, height, DXGI_FORMAT_UNKNOWN, desc.Flags, masks.data(), present_queues.data())
      );
    }
    CHECK(graphics.idle());
    expect(CloseHandle(ready), "CloseHandle readiness");
  }
  DestroyWindow(window);
  UnregisterClassA(wc.lpszClassName, wc.hInstance);
  return verdict();
}
