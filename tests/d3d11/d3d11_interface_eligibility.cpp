// contract: a legacy factory rejects IDXGIFactory1; RESOURCE_CLAMP textures and structured buffers reject
// D3D10 resource interfaces through every query route. Microsoft Learn, IDXGIFactory1: "To create a factory,
// call the CreateDXGIFactory1 function." QueryInterface: "E_NOINTERFACE otherwise."
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nn-dxgi-idxgifactory1
// https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nf-unknwn-iunknown-queryinterface(refiid_void)
// eligibility is Windows' observed contract in Wine's dlls/dxgi/tests/dxgi.c, test_create_factory, and
// dlls/d3d11/tests/d3d11.c, test_texture{1,2,3}d_interfaces and test_create_buffer: "Structured buffers do not
// implement ID3D10Buffer." IDXGIFactory2 remains available from legacy creation. queries must clear a refused
// output, preserve reference counts and share COM identity. ordinary resources remain eligible on both devices.
// debug devices expose a working message queue. Microsoft Learn, ID3D11InfoQueue: "To get this interface, turn on
// debug layer and use IUnknown::QueryInterface from the ID3D11Device."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11sdklayers/nn-d3d11sdklayers-id3d11infoqueue
// immediate contexts expose ID3D11VideoContext (Wine's test_immediate_context); its device-child methods must
// preserve its device, private data and COM identity.
// structured strides reach the SDK maximum: "The StructureByteStride can be at most 2048 bytes."
// (D3D11.3 5.4.2). ByteWidth is a whole number of structures at every stride.
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <dxgi1_3.h>
#include <d3d11sdklayers.h>
#include <limits>
#include <algorithm>

static void check_interface(IUnknown *object, REFIID iid, bool supported) {
  object->AddRef();
  auto before = object->Release();
  IUnknown *output = object;
  auto hr = object->QueryInterface(iid, reinterpret_cast<void **>(&output));
  expect(hr == (supported ? S_OK : E_NOINTERFACE) && (supported ? output != nullptr : output == nullptr),
         "IID %lu returned %#lx, output %p, expected support %u", iid.Data1, hr, output, supported);
  object->AddRef();
  expect(object->Release() == before + (SUCCEEDED(hr) && output != nullptr),
         "IID %lu changed the reference count incorrectly", iid.Data1);
  if (SUCCEEDED(hr) && output) {
    ComPtr<IUnknown> original, queried;
    auto original_hr = object->QueryInterface(IID_PPV_ARGS(&original));
    auto queried_hr = output->QueryInterface(IID_PPV_ARGS(&queried));
    expect(original_hr == S_OK && queried_hr == S_OK && original && original == queried,
           "IID %lu has a different COM identity", iid.Data1);
    output->Release();
  }
  object->AddRef();
  expect(object->Release() == before, "IID %lu leaked a reference", iid.Data1);
  expect(object->QueryInterface(iid, nullptr) == E_POINTER, "IID %lu accepts a null output address", iid.Data1);
}

static void check_resource(ID3D11Resource *resource, REFIID typed10, bool supported) {
  ComPtr<IUnknown> identity;
  ComPtr<ID3D11DeviceChild> child;
  ComPtr<IDXGIResource> dxgi;
  if (!expect(resource->QueryInterface(IID_PPV_ARGS(&identity)) == S_OK &&
                  resource->QueryInterface(IID_PPV_ARGS(&child)) == S_OK &&
                  resource->QueryInterface(IID_PPV_ARGS(&dxgi)) == S_OK && identity && child && dxgi,
              "the D3D11 resource or DXGI interface is absent"))
    return;
  for (IUnknown *route : {static_cast<IUnknown *>(resource), identity.Get(), static_cast<IUnknown *>(child.Get()),
                          static_cast<IUnknown *>(dxgi.Get())}) {
    for (const auto &iid : {__uuidof(ID3D10DeviceChild), __uuidof(ID3D10Resource), typed10})
      check_interface(route, iid, supported);
    if (supported) {
      ComPtr<ID3D10Resource> resource10;
      if (!expect(route->QueryInterface(IID_PPV_ARGS(&resource10)) == S_OK && resource10,
                  "the accepted resource has no D3D10 interface"))
        continue;
      check_interface(resource10.Get(), typed10, true);
      check_interface(resource10.Get(), __uuidof(ID3D11Resource), true);
      D3D11_RESOURCE_DIMENSION dimension11{};
      D3D10_RESOURCE_DIMENSION dimension10{};
      resource->GetType(&dimension11);
      resource10->GetType(&dimension10);
      expect(UINT(dimension11) == UINT(dimension10), "resource dimensions differ: %u, %u", dimension11, dimension10);
    }
  }
}

static LONG breaks;

static LONG CALLBACK breakpoint(EXCEPTION_POINTERS *info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT)
    return EXCEPTION_CONTINUE_SEARCH;
  InterlockedIncrement(&breaks);
  // the context stands at the breakpoint instruction: resume after it, one byte on x86 and four on ARM64
  // (brk #0xf000)
#if defined(__aarch64__) && !defined(__arm64ec__)
  info->ContextRecord->Pc += 4;
#elif defined(_WIN64)
  info->ContextRecord->Rip++;
#else
  info->ContextRecord->Eip++;
#endif
  return EXCEPTION_CONTINUE_EXECUTION;
}

static int check_queue(ID3D11InfoQueue *queue) {
  // Microsoft Learn, ID3D11InfoQueue::PushEmptyStorageFilter: "An empty storage filter allows all messages to
  // pass through." GetMessage: "This method does not remove any messages from the message queue."
  // SetMessageCountLimit: "new messages coming in will push old messages out."
  expect(queue->GetMessageCountLimit() == D3D11_INFO_QUEUE_DEFAULT_MESSAGE_COUNT_LIMIT,
         "default message limit differs");
  queue->SetMuteDebugOutput(TRUE);
  expect(queue->GetMuteDebugOutput(), "debug output did not mute");
  for (bool retrieval : {false, true}) {
    step("info queue %s filter stack", retrieval ? "retrieval" : "storage");
    auto get = [&](D3D11_INFO_QUEUE_FILTER *filter, SIZE_T *bytes) {
      return retrieval ? queue->GetRetrievalFilter(filter, bytes) : queue->GetStorageFilter(filter, bytes);
    };
    auto depth = [&] { return retrieval ? queue->GetRetrievalFilterStackSize() : queue->GetStorageFilterStackSize(); };
    auto clear = [&] { retrieval ? queue->ClearRetrievalFilter() : queue->ClearStorageFilter(); };
    auto pop = [&] { retrieval ? queue->PopRetrievalFilter() : queue->PopStorageFilter(); };
    auto initial_depth = depth();
    CHECK(retrieval ? queue->PushEmptyRetrievalFilter() : queue->PushEmptyStorageFilter());
    expect(depth() == initial_depth + 1, "empty push lost a filter");
    SIZE_T bytes = 0;
    CHECK(get(nullptr, &bytes));
    expect(bytes == sizeof(D3D11_INFO_QUEUE_FILTER), "empty filter has data: %zu", bytes);
    D3D11_INFO_QUEUE_FILTER empty{};
    CHECK(get(&empty, &bytes));
    for (SIZE_T short_size : {SIZE_T(0), SIZE_T(sizeof(empty) - 1)}) {
      auto supplied = short_size;
      expect(FAILED(get(&empty, &supplied)) && supplied == sizeof(empty), "short filter output was accepted");
    }
    expect(FAILED(get(nullptr, nullptr)), "filter size output may not be null");
    expect(!empty.AllowList.NumCategories && !empty.AllowList.NumSeverities && !empty.AllowList.NumIDs &&
               !empty.DenyList.NumCategories && !empty.DenyList.NumSeverities && !empty.DenyList.NumIDs,
           "empty push inherited entries");
    D3D11_MESSAGE_CATEGORY categories[] = {D3D11_MESSAGE_CATEGORY_APPLICATION_DEFINED,
                                           D3D11_MESSAGE_CATEGORY_EXECUTION};
    D3D11_MESSAGE_SEVERITY severities[] = {D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_WARNING};
    D3D11_MESSAGE_ID ids[] = {D3D11_MESSAGE_ID_STRING_FROM_APPLICATION, D3D11_MESSAGE_ID_UNKNOWN};
    for (bool deny : {false, true}) {
      clear();
      D3D11_INFO_QUEUE_FILTER filter{};
      auto &list = deny ? filter.DenyList : filter.AllowList;
      list = {ARRAYSIZE(categories), categories, ARRAYSIZE(severities), severities, ARRAYSIZE(ids), ids};
      CHECK(retrieval ? queue->AddRetrievalFilterEntries(&filter) : queue->AddStorageFilterEntries(&filter));
      bytes = 0;
      CHECK(get(nullptr, &bytes));
      const SIZE_T wanted = sizeof(filter) + sizeof(categories) + sizeof(severities) + sizeof(ids);
      expect(bytes == wanted, "filter byte length %zu differs from %zu", bytes, wanted);
      std::vector<unsigned char> data(wanted);
      auto returned = reinterpret_cast<D3D11_INFO_QUEUE_FILTER *>(data.data());
      CHECK(get(returned, &bytes));
      auto &out = deny ? returned->DenyList : returned->AllowList;
      expect(
          out.NumCategories == ARRAYSIZE(categories) && out.NumSeverities == ARRAYSIZE(severities) &&
              out.NumIDs == ARRAYSIZE(ids) && out.pCategoryList && out.pSeverityList && out.pIDList &&
              std::is_permutation(out.pCategoryList, out.pCategoryList + out.NumCategories, std::begin(categories)) &&
              std::is_permutation(out.pSeverityList, out.pSeverityList + out.NumSeverities, std::begin(severities)) &&
              std::is_permutation(out.pIDList, out.pIDList + out.NumIDs, std::begin(ids)),
          "filter entries changed");
      CHECK(retrieval ? queue->PushCopyOfRetrievalFilter() : queue->PushCopyOfStorageFilter());
      clear();
      pop();
      bytes = wanted;
      CHECK(get(returned, &bytes));
      expect((deny ? returned->DenyList.NumIDs : returned->AllowList.NumIDs) == ARRAYSIZE(ids),
             "clearing a copied filter changed its parent");
      CHECK(retrieval ? queue->PushRetrievalFilter(&filter) : queue->PushStorageFilter(&filter));
      pop();
      expect(depth() == initial_depth + 1, "push/pop changed the parent depth");
    }
    expect(FAILED(retrieval ? queue->PushRetrievalFilter(nullptr) : queue->PushStorageFilter(nullptr)),
           "null filter was pushed");
    expect(FAILED(retrieval ? queue->AddRetrievalFilterEntries(nullptr) : queue->AddStorageFilterEntries(nullptr)),
           "null filter was added");
    D3D11_INFO_QUEUE_FILTER invalid{};
    invalid.DenyList.NumIDs = 1;
    expect(FAILED(retrieval ? queue->AddRetrievalFilterEntries(&invalid) : queue->AddStorageFilterEntries(&invalid)),
           "filter with a missing list was added");
    clear();
    pop();
    expect(depth() == initial_depth, "filter stack leaked");
  }
  queue->ClearStorageFilter();
  queue->ClearRetrievalFilter();
  for (UINT64 limit : {UINT64(0), UINT64(1), UINT64(D3D11_INFO_QUEUE_DEFAULT_MESSAGE_COUNT_LIMIT)}) {
    step("info queue message limit %llu", (unsigned long long)limit);
    queue->ClearStoredMessages();
    CHECK(queue->SetMessageCountLimit(limit));
    expect(queue->GetMessageCountLimit() == limit, "message limit changed");
    auto before = queue->GetNumMessagesDiscardedByMessageCountLimit();
    for (UINT64 i = 0; i <= limit; ++i) {
      auto text = std::to_string(i);
      CHECK(queue->AddApplicationMessage(D3D11_MESSAGE_SEVERITY_MESSAGE, text.c_str()));
    }
    expect(queue->GetNumStoredMessages() == limit && queue->GetNumMessagesDiscardedByMessageCountLimit() == before + 1,
           "message count limit did not evict the oldest message");
    for (UINT64 i = 0; i < limit; ++i) {
      SIZE_T bytes = 0;
      CHECK(queue->GetMessage(i, nullptr, &bytes));
      auto text = std::to_string(i + 1);
      expect(bytes == sizeof(D3D11_MESSAGE) + text.size() + 1, "message size differs: %zu", bytes);
      std::vector<unsigned char> data(bytes, std::numeric_limits<unsigned char>::max());
      auto message = reinterpret_cast<D3D11_MESSAGE *>(data.data());
      CHECK(queue->GetMessage(i, message, &bytes));
      for (SIZE_T short_size : {SIZE_T(0), bytes - 1}) {
        auto supplied = short_size;
        expect(FAILED(queue->GetMessage(i, message, &supplied)) && supplied == bytes,
               "short message output was accepted");
      }
      expect(message->Category == D3D11_MESSAGE_CATEGORY_APPLICATION_DEFINED &&
                 message->Severity == D3D11_MESSAGE_SEVERITY_MESSAGE &&
                 message->ID == D3D11_MESSAGE_ID_STRING_FROM_APPLICATION &&
                 message->DescriptionByteLength == text.size() + 1 && message->pDescription &&
                 !memcmp(message->pDescription, text.c_str(), text.size() + 1),
             "stored message %llu changed", (unsigned long long)i);
    }
    expect(queue->GetNumStoredMessages() == limit, "retrieval removed messages");
    SIZE_T bytes = 0;
    expect(FAILED(queue->GetMessage(limit, nullptr, &bytes)), "retrieval accepted an out-of-range index");
  }
  expect(FAILED(queue->GetMessage(0, nullptr, nullptr)), "message size output may not be null");
  expect(FAILED(queue->AddApplicationMessage(D3D11_MESSAGE_SEVERITY_MESSAGE, nullptr)), "null message was accepted");
  for (auto severity : {D3D11_MESSAGE_SEVERITY(D3D11_MESSAGE_SEVERITY_CORRUPTION - 1),
                        D3D11_MESSAGE_SEVERITY(D3D11_MESSAGE_SEVERITY_MESSAGE + 1)}) {
    expect(FAILED(queue->AddApplicationMessage(severity, "invalid")), "invalid severity was accepted");
    expect(FAILED(queue->SetBreakOnSeverity(severity, TRUE)), "invalid break severity was accepted");
  }
  for (auto category : {D3D11_MESSAGE_CATEGORY(D3D11_MESSAGE_CATEGORY_APPLICATION_DEFINED - 1),
                        D3D11_MESSAGE_CATEGORY(D3D11_MESSAGE_CATEGORY_SHADER + 1)}) {
    expect(FAILED(queue->AddMessage(category, D3D11_MESSAGE_SEVERITY_MESSAGE, D3D11_MESSAGE_ID_UNKNOWN, "invalid")),
           "invalid category was accepted");
    expect(FAILED(queue->SetBreakOnCategory(category, TRUE)), "invalid break category was accepted");
  }
  CHECK(queue->SetMessageCountLimit(std::numeric_limits<UINT64>::max()));
  queue->ClearStoredMessages();
  for (bool retrieval : {false, true}) {
    const auto fields = {&D3D11_INFO_QUEUE_FILTER_DESC::NumCategories, &D3D11_INFO_QUEUE_FILTER_DESC::NumSeverities,
                         &D3D11_INFO_QUEUE_FILTER_DESC::NumIDs};
    UINT field = 0;
    for (auto count : fields) {
      step("info queue %s deny field %u", retrieval ? "retrieval" : "storage", field);
      queue->ClearStoredMessages();
      D3D11_MESSAGE_CATEGORY category = D3D11_MESSAGE_CATEGORY_APPLICATION_DEFINED;
      D3D11_MESSAGE_SEVERITY severity = D3D11_MESSAGE_SEVERITY_MESSAGE;
      D3D11_MESSAGE_ID id = D3D11_MESSAGE_ID_STRING_FROM_APPLICATION;
      D3D11_INFO_QUEUE_FILTER filter{};
      filter.DenyList.*count = 1;
      filter.DenyList.pCategoryList = &category;
      filter.DenyList.pSeverityList = &severity;
      filter.DenyList.pIDList = &id;
      CHECK(retrieval ? queue->PushRetrievalFilter(&filter) : queue->PushStorageFilter(&filter));
      auto denied = queue->GetNumMessagesDeniedByStorageFilter();
      auto allowed = queue->GetNumMessagesAllowedByStorageFilter();
      CHECK(queue->AddApplicationMessage(severity, "denied"));
      expect(queue->GetNumStoredMessages() == UINT64(retrieval) &&
                 queue->GetNumStoredMessagesAllowedByRetrievalFilter() == 0 &&
                 queue->GetNumMessagesDeniedByStorageFilter() == denied + !retrieval &&
                 queue->GetNumMessagesAllowedByStorageFilter() == allowed + retrieval,
             "deny filter or counters differ");
      retrieval ? queue->PopRetrievalFilter() : queue->PopStorageFilter();
      expect(queue->GetNumStoredMessagesAllowedByRetrievalFilter() == UINT64(retrieval),
             "retrieval filter lost storage");
      queue->ClearStoredMessages();
      filter.AllowList = filter.DenyList;
      filter.DenyList = {};
      CHECK(retrieval ? queue->PushRetrievalFilter(&filter) : queue->PushStorageFilter(&filter));
      CHECK(queue->AddMessage(D3D11_MESSAGE_CATEGORY_EXECUTION, D3D11_MESSAGE_SEVERITY_WARNING,
                              D3D11_MESSAGE_ID_UNKNOWN, "outside allow list"));
      CHECK(queue->AddApplicationMessage(severity, "inside allow list"));
      expect(queue->GetNumStoredMessages() == (retrieval ? 2 : 1) &&
                 queue->GetNumStoredMessagesAllowedByRetrievalFilter() == 1,
             "allow filter differs");
      SIZE_T bytes = 0;
      CHECK(queue->GetMessage(0, nullptr, &bytes));
      std::vector<unsigned char> data(bytes, std::numeric_limits<unsigned char>::max());
      auto message = reinterpret_cast<D3D11_MESSAGE *>(data.data());
      CHECK(queue->GetMessage(0, message, &bytes));
      expect(message->pDescription && !strcmp(message->pDescription, "inside allow list"),
             "retrieval did not reindex messages");
      retrieval ? queue->PopRetrievalFilter() : queue->PopStorageFilter();
      ++field;
    }
  }
  auto handler = AddVectoredExceptionHandler(TRUE, breakpoint);
  if (!expect(handler != nullptr, "could not install breakpoint handler"))
    return 1;
  for (UINT category = D3D11_MESSAGE_CATEGORY_APPLICATION_DEFINED; category <= D3D11_MESSAGE_CATEGORY_SHADER;
       ++category) {
    for (UINT severity = D3D11_MESSAGE_SEVERITY_CORRUPTION; severity <= D3D11_MESSAGE_SEVERITY_MESSAGE; ++severity) {
      step("info queue category %u severity %u break", category, severity);
      auto cat = D3D11_MESSAGE_CATEGORY(category);
      auto sev = D3D11_MESSAGE_SEVERITY(severity);
      for (auto condition : {&D3D11_INFO_QUEUE_FILTER_DESC::NumCategories, &D3D11_INFO_QUEUE_FILTER_DESC::NumSeverities,
                             &D3D11_INFO_QUEUE_FILTER_DESC::NumIDs}) {
        queue->SetBreakOnCategory(cat, condition == &D3D11_INFO_QUEUE_FILTER_DESC::NumCategories);
        queue->SetBreakOnSeverity(sev, condition == &D3D11_INFO_QUEUE_FILTER_DESC::NumSeverities);
        queue->SetBreakOnID(D3D11_MESSAGE_ID_UNKNOWN, condition == &D3D11_INFO_QUEUE_FILTER_DESC::NumIDs);
        expect(queue->GetBreakOnCategory(cat) == (condition == &D3D11_INFO_QUEUE_FILTER_DESC::NumCategories) &&
                   queue->GetBreakOnSeverity(sev) == (condition == &D3D11_INFO_QUEUE_FILTER_DESC::NumSeverities) &&
                   queue->GetBreakOnID(D3D11_MESSAGE_ID_UNKNOWN) ==
                       (condition == &D3D11_INFO_QUEUE_FILTER_DESC::NumIDs),
               "break state differs");
        auto before = breaks;
        CHECK(queue->AddMessage(cat, sev, D3D11_MESSAGE_ID_UNKNOWN, "break"));
        expect(breaks == before + 1, "break condition did not break");
      }
      queue->SetBreakOnCategory(cat, FALSE);
      queue->SetBreakOnSeverity(sev, FALSE);
      queue->SetBreakOnID(D3D11_MESSAGE_ID_UNKNOWN, FALSE);
    }
  }
  D3D11_MESSAGE_SEVERITY severity = D3D11_MESSAGE_SEVERITY_MESSAGE;
  D3D11_INFO_QUEUE_FILTER denied{};
  denied.DenyList.NumSeverities = 1;
  denied.DenyList.pSeverityList = &severity;
  CHECK(queue->PushStorageFilter(&denied));
  CHECK(queue->SetBreakOnSeverity(severity, TRUE));
  auto before = breaks;
  CHECK(queue->AddApplicationMessage(severity, "denied breakpoint"));
  expect(breaks == before, "a storage-denied message broke");
  CHECK(queue->SetBreakOnSeverity(severity, FALSE));
  queue->PopStorageFilter();
  RemoveVectoredExceptionHandler(handler);
  queue->SetMuteDebugOutput(FALSE);
  expect(!queue->GetMuteDebugOutput(), "debug output stayed muted");
  return 0;
}

int main(int argc, char **argv) {
  bool observe = argc == 2 && !strcmp(argv[1], "--observe-views");
  auto module = LoadLibraryW(L"d3d10_1.dll");
  if (!expect(module != nullptr, "d3d10_1.dll could not load: %lu", GetLastError()))
    return verdict();
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(GetProcAddress(module, "D3D10CreateDevice1"));
  if (!expect(create10 != nullptr, "D3D10CreateDevice1 could not load")) {
    FreeLibrary(module);
    return verdict();
  }
  for (UINT version = 0; version <= 2; ++version) {
    for (UINT flags : {0u, UINT(DXGI_CREATE_FACTORY_DEBUG)}) {
      if (version != 2 && flags)
        continue;
      for (const auto &requested : {__uuidof(IUnknown), __uuidof(IDXGIObject), __uuidof(IDXGIFactory),
                                    __uuidof(IDXGIFactory1), __uuidof(IDXGIFactory2), __uuidof(IDXGIDevice)}) {
        step("factory creation %u flags %#x IID %lu", version, flags, requested.Data1);
        IUnknown *raw = reinterpret_cast<IUnknown *>(&module);
        auto hr = version == 0   ? CreateDXGIFactory(requested, reinterpret_cast<void **>(&raw))
                  : version == 1 ? CreateDXGIFactory1(requested, reinterpret_cast<void **>(&raw))
                                 : CreateDXGIFactory2(flags, requested, reinterpret_cast<void **>(&raw));
        bool supported = requested != __uuidof(IDXGIDevice) && (version != 0 || requested != __uuidof(IDXGIFactory1));
        if (!expect(hr == (supported ? S_OK : E_NOINTERFACE) && (supported ? raw != nullptr : raw == nullptr),
                    "creation returned %#lx, output %p, expected support %u", hr, raw, supported) ||
            FAILED(hr) || !raw) {
          if (SUCCEEDED(hr) && raw)
            raw->Release();
          continue;
        }
        ComPtr<IUnknown> object;
        object.Attach(raw);
        ComPtr<IDXGIFactory> factory;
        ComPtr<IDXGIFactory2> factory2;
        ComPtr<IDXGIFactory3> factory3;
        CHECK(object.As(&factory));
        CHECK(object.As(&factory2));
        CHECK(object.As(&factory3));
        expect(factory3->GetCreationFlags() == flags, "factory flags differ: %#x", factory3->GetCreationFlags());
        for (IUnknown *route :
             {object.Get(), static_cast<IUnknown *>(factory.Get()), static_cast<IUnknown *>(factory2.Get())}) {
          check_interface(route, __uuidof(IDXGIFactory1), version != 0);
          check_interface(route, __uuidof(IDXGIFactory2), true);
        }
        ComPtr<IDXGIAdapter> adapter;
        CHECK(factory->EnumAdapters(0, &adapter));
        ComPtr<IDXGIFactory> parent;
        CHECK(adapter->GetParent(IID_PPV_ARGS(&parent)));
        check_interface(parent.Get(), __uuidof(IDXGIFactory1), version != 0);
        ComPtr<IDXGIFactory1> parent1;
        auto parent_hr = adapter->GetParent(IID_PPV_ARGS(&parent1));
        expect(parent_hr == (version ? S_OK : E_NOINTERFACE) && bool(parent1) == bool(version),
               "adapter parent returned %#lx for IDXGIFactory1", parent_hr);
      }
    }
  }
  if (!observe) {
    for (bool created10 : {false, true}) {
      for (bool debug : {false, true}) {
        step("D3D%u info queue debug %u", created10 ? 10 : 11, debug);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D10Device1> device10;
        if (created10) {
          CHECK(create10(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, debug ? D3D10_CREATE_DEVICE_DEBUG : 0,
                         D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &device10));
          CHECK(device10.As(&device));
        } else {
          CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, debug ? D3D11_CREATE_DEVICE_DEBUG : 0,
                                  nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
        }
        check_interface(device.Get(), __uuidof(ID3D11InfoQueue), debug);
        if (debug) {
          ComPtr<ID3D11InfoQueue> queue;
          CHECK(device.As(&queue));
          CHECK(check_queue(queue.Get()) ? E_FAIL : S_OK);
        }
      }
    }
  }
  for (bool created10 : {false, true}) {
    step("create D3D%u device", created10 ? 10 : 11);
    ComPtr<ID3D10Device1> device10;
    ComPtr<ID3D11Device> device;
    if (created10) {
      CHECK(create10(nullptr, observe ? D3D10_DRIVER_TYPE_WARP : D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
                     D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &device10));
      CHECK(device10.As(&device));
    } else {
      const auto level = D3D_FEATURE_LEVEL_11_0;
      CHECK(D3D11CreateDevice(nullptr, observe ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
                              D3D11_SDK_VERSION, &device, nullptr, nullptr));
    }
    step("D3D%u default adapter parent", created10 ? 10 : 11);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory> parent;
    CHECK(device.As(&dxgi));
    CHECK(dxgi->GetAdapter(&adapter));
    CHECK(adapter->GetParent(IID_PPV_ARGS(&parent)));
    check_interface(parent.Get(), __uuidof(IDXGIFactory1), !created10);
    step("D3D%u immediate video context", created10 ? 10 : 11);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    check_interface(context.Get(), __uuidof(ID3D11VideoContext), true);
    ComPtr<ID3D11VideoContext> video;
    if (SUCCEEDED(context.As(&video)) && video) {
      ComPtr<ID3D11Device> owner;
      video->GetDevice(&owner);
      expect(owner == device, "video context returned another device");
      const auto &key = __uuidof(ID3D11VideoContext);
      UINT value = D3D11_SDK_VERSION, returned = 0, bytes = sizeof(returned);
      CHECK(video->SetPrivateData(key, sizeof(value), &value));
      CHECK(context->GetPrivateData(key, &bytes, &returned));
      expect(bytes == sizeof(value) && returned == value, "video context did not share private data");
      CHECK(video->SetPrivateData(key, 0, nullptr));
      // an interface comes back as the pointer it was stored as (Wine's d3d11 tests, test_private_data)
      CHECK(video->SetPrivateDataInterface(key, device.Get()));
      ComPtr<IUnknown> stored;
      bytes = sizeof(IUnknown *);
      CHECK(context->GetPrivateData(key, &bytes, stored.GetAddressOf()));
      expect(stored.Get() == static_cast<IUnknown *>(device.Get()) && bytes == sizeof(IUnknown *),
             "video context changed the stored interface");
      // a null interface replaces it: the key then holds a null pointer (test_private_data)
      CHECK(context->SetPrivateDataInterface(key, nullptr));
      IUnknown *cleared = reinterpret_cast<IUnknown *>(UINT_PTR(0xdeadbeef));
      bytes = sizeof(cleared);
      expect(video->GetPrivateData(key, &bytes, &cleared) == S_OK && !cleared && bytes == sizeof(IUnknown *),
             "video context retained the replaced interface");
    }
    for (UINT dimension = D3D11_RESOURCE_DIMENSION_BUFFER; dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D;
         ++dimension) {
      bool is_buffer = dimension == D3D11_RESOURCE_DIMENSION_BUFFER;
      for (UINT use = D3D11_USAGE_DEFAULT; use <= D3D11_USAGE_STAGING; ++use) {
        auto usage = D3D11_USAGE(use);
        for (bool restricted : {false, true}) {
          if (created10 && restricted)
            continue;
          if (restricted && (usage == D3D11_USAGE_DYNAMIC || usage == D3D11_USAGE_STAGING))
            continue;
          for (UINT bind : {0u, UINT(D3D11_BIND_SHADER_RESOURCE), UINT(D3D11_BIND_UNORDERED_ACCESS)}) {
            if (created10 && bind == D3D11_BIND_UNORDERED_ACCESS)
              continue;
            if ((usage == D3D11_USAGE_STAGING && bind) ||
                (bind == D3D11_BIND_UNORDERED_ACCESS && usage != D3D11_USAGE_DEFAULT) ||
                (usage != D3D11_USAGE_DEFAULT && !bind && usage != D3D11_USAGE_STAGING))
              continue;
            for (bool combined : {false, true}) {
              if (combined &&
                  (usage != D3D11_USAGE_DEFAULT || created10 || (!is_buffer && bind != D3D11_BIND_SHADER_RESOURCE)))
                continue;
              for (UINT stride :
                   {UINT(sizeof(UINT)), UINT(sizeof(UINT[4])), UINT(D3D11_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES)}) {
                if ((!is_buffer || !restricted) && stride != sizeof(UINT))
                  continue;
                for (UINT width : {1u, 2u, 3u}) {
                  step("D3D%u dimension %u usage %u restricted %u bind %#x combined %u width %u stride %u",
                       created10 ? 10 : 11, dimension, usage, restricted, bind, combined, width, stride);
                  UINT misc = restricted ? UINT(is_buffer ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED
                                                          : D3D11_RESOURCE_MISC_RESOURCE_CLAMP)
                                         : 0;
                  if (combined)
                    misc |= is_buffer ? D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS : D3D11_RESOURCE_MISC_GENERATE_MIPS;
                  UINT resource_bind = bind | (combined && !is_buffer ? D3D11_BIND_RENDER_TARGET : 0);
                  UINT access = usage == D3D11_USAGE_DYNAMIC   ? D3D11_CPU_ACCESS_WRITE
                                : usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ
                                                               : 0;
                  std::vector<UINT> data(is_buffer ? width * stride / sizeof(UINT) : width * width * width);
                  D3D11_SUBRESOURCE_DATA initial{data.data(), width * UINT(sizeof(UINT)),
                                                 width * width * UINT(sizeof(UINT))};
                  auto upload = usage == D3D11_USAGE_IMMUTABLE ? &initial : nullptr;
                  ComPtr<ID3D11Resource> resource;
                  const GUID *typed10 = nullptr;
                  switch (dimension) {
                  case D3D11_RESOURCE_DIMENSION_BUFFER: {
                    D3D11_BUFFER_DESC desc{width * stride, usage, resource_bind, access, misc, restricted ? stride : 0};
                    ComPtr<ID3D11Buffer> object;
                    if (created10) {
                      D3D10_BUFFER_DESC desc10{desc.ByteWidth, D3D10_USAGE(usage), bind, access};
                      ComPtr<ID3D10Buffer> object10;
                      CHECK(device10->CreateBuffer(&desc10, reinterpret_cast<const D3D10_SUBRESOURCE_DATA *>(upload),
                                                   &object10));
                      CHECK(object10.As(&object));
                    } else {
                      CHECK(device->CreateBuffer(&desc, upload, &object));
                    }
                    if (!restricted) {
                      ComPtr<ID3D10Buffer> object10;
                      CHECK(object.As(&object10));
                      D3D10_BUFFER_DESC returned{};
                      object10->GetDesc(&returned);
                      expect(returned.ByteWidth == desc.ByteWidth && returned.BindFlags == desc.BindFlags &&
                                 returned.MiscFlags == 0,
                             "D3D10 buffer descriptor differs");
                    }
                    resource = object;
                    typed10 = &__uuidof(ID3D10Buffer);
                    break;
                  }
                  case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
                    D3D11_TEXTURE1D_DESC desc{width,         1,      1,   DXGI_FORMAT_R8G8B8A8_UNORM, usage,
                                              resource_bind, access, misc};
                    ComPtr<ID3D11Texture1D> object;
                    if (created10) {
                      D3D10_TEXTURE1D_DESC desc10{width, 1, 1, desc.Format, D3D10_USAGE(usage), bind, access};
                      ComPtr<ID3D10Texture1D> object10;
                      CHECK(device10->CreateTexture1D(&desc10, reinterpret_cast<const D3D10_SUBRESOURCE_DATA *>(upload),
                                                      &object10));
                      CHECK(object10.As(&object));
                    } else {
                      CHECK(device->CreateTexture1D(&desc, upload, &object));
                    }
                    if (!restricted) {
                      ComPtr<ID3D10Texture1D> object10;
                      CHECK(object.As(&object10));
                      D3D10_TEXTURE1D_DESC returned{};
                      object10->GetDesc(&returned);
                      expect(returned.Width == desc.Width && returned.BindFlags == desc.BindFlags &&
                                 returned.MiscFlags == (combined ? D3D10_RESOURCE_MISC_GENERATE_MIPS : 0),
                             "D3D10 1D texture descriptor differs");
                    }
                    resource = object;
                    typed10 = &__uuidof(ID3D10Texture1D);
                    break;
                  }
                  case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
                    D3D11_TEXTURE2D_DESC desc{width, width,         1,      1,   DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                              usage, resource_bind, access, misc};
                    ComPtr<ID3D11Texture2D> object;
                    if (created10) {
                      D3D10_TEXTURE2D_DESC desc10{width, width, 1, 1, desc.Format, {1, 0}, D3D10_USAGE(usage),
                                                  bind,  access};
                      ComPtr<ID3D10Texture2D> object10;
                      CHECK(device10->CreateTexture2D(&desc10, reinterpret_cast<const D3D10_SUBRESOURCE_DATA *>(upload),
                                                      &object10));
                      CHECK(object10.As(&object));
                    } else {
                      CHECK(device->CreateTexture2D(&desc, upload, &object));
                    }
                    if (!restricted) {
                      ComPtr<ID3D10Texture2D> object10;
                      CHECK(object.As(&object10));
                      D3D10_TEXTURE2D_DESC returned{};
                      object10->GetDesc(&returned);
                      expect(returned.Width == desc.Width && returned.Height == desc.Height &&
                                 returned.BindFlags == desc.BindFlags &&
                                 returned.MiscFlags == (combined ? D3D10_RESOURCE_MISC_GENERATE_MIPS : 0),
                             "D3D10 2D texture descriptor differs");
                    }
                    resource = object;
                    typed10 = &__uuidof(ID3D10Texture2D);
                    break;
                  }
                  case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
                    D3D11_TEXTURE3D_DESC desc{width, width,         width,  1,   DXGI_FORMAT_R8G8B8A8_UNORM,
                                              usage, resource_bind, access, misc};
                    ComPtr<ID3D11Texture3D> object;
                    if (created10) {
                      D3D10_TEXTURE3D_DESC desc10{width, width, width, 1, desc.Format, D3D10_USAGE(usage),
                                                  bind,  access};
                      ComPtr<ID3D10Texture3D> object10;
                      CHECK(device10->CreateTexture3D(&desc10, reinterpret_cast<const D3D10_SUBRESOURCE_DATA *>(upload),
                                                      &object10));
                      CHECK(object10.As(&object));
                    } else {
                      CHECK(device->CreateTexture3D(&desc, upload, &object));
                    }
                    if (!restricted) {
                      ComPtr<ID3D10Texture3D> object10;
                      CHECK(object.As(&object10));
                      D3D10_TEXTURE3D_DESC returned{};
                      object10->GetDesc(&returned);
                      expect(returned.Width == desc.Width && returned.Height == desc.Height &&
                                 returned.Depth == desc.Depth && returned.BindFlags == desc.BindFlags &&
                                 returned.MiscFlags == (combined ? D3D10_RESOURCE_MISC_GENERATE_MIPS : 0),
                             "D3D10 3D texture descriptor differs");
                    }
                    resource = object;
                    typed10 = &__uuidof(ID3D10Texture3D);
                    break;
                  }
                  }
                  check_resource(resource.Get(), *typed10, !restricted);
                  if (observe && bind == D3D11_BIND_SHADER_RESOURCE) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
                    view_desc.Format = DXGI_FORMAT_R32_UINT;
                    view_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                    view_desc.Buffer.NumElements = width * stride / sizeof(UINT);
                    ComPtr<ID3D11ShaderResourceView> view;
                    CHECK(device->CreateShaderResourceView(resource.Get(),
                                                           is_buffer && !restricted ? &view_desc : nullptr, &view));
                    ComPtr<ID3D10ShaderResourceView> view10;
                    ComPtr<ID3D10Resource> queried, returned;
                    auto resource_hr = resource.As(&queried);
                    auto view_hr = view.As(&view10);
                    if (view10)
                      view10->GetResource(&returned);
                    printf("observed: resource QI %#lx resource %u view QI %#lx GetResource %u\n", resource_hr,
                           !!queried, view_hr, !!returned);
                  }
                }
              }
            }
          }
        }
      }
    }
    // downlevel raw buffers require this capability (D3D11.3 18.7.3.2).
    D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS options{};
    CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &options, sizeof(options)));
    if (options.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x) {
      step("D3D%u raw buffer is still a D3D10 resource", created10 ? 10 : 11);
      D3D11_BUFFER_DESC raw_desc{UINT(sizeof(UINT)), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                 D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS};
      ComPtr<ID3D11Buffer> raw;
      CHECK(device->CreateBuffer(&raw_desc, nullptr, &raw));
      check_resource(raw.Get(), __uuidof(ID3D10Buffer), true);
    } else {
      expect(device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0, "feature level 11 has no raw-buffer support");
      printf("not checked: raw-buffer control is unsupported on this D3D%u device\n", created10 ? 10 : 11);
    }
  }
  step("unload the D3D10 front end after releasing its devices");
  FreeLibrary(module);
  if (observe) {
    printf("not checked: view eligibility needs the recorded observations on native Windows\n");
    return trace::wrong ? 1 : 77;
  }
  return verdict();
}
