// contract: device creation rejects invalid arguments with cleared outputs and selects the first supported
// requested level on success. "The SDK version; use D3D11_SDK_VERSION." (Microsoft Learn, D3D11CreateDevice,
// SDKVersion); the same page's parameters and remarks define the adapter, driver, software and level dependencies.
// pFeatureLevels defines "the order of feature levels to attempt to create."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-d3d11createdevice
// legal levels are named by D3D_FEATURE_LEVEL, not numeric values below a supported level:
// https://learn.microsoft.com/en-us/windows/win32/api/d3dcommon/ne-d3dcommon-d3d_feature_level
// Windows witnesses: Wine dlls/d3d11/tests/d3d11.c:2148-2186, 2206-2332 (optional outputs and swap-chain failure),
// dlls/d3d10_1/tests/d3d10_1.c:128-143, 214-269 (required output, invalid level, swap-chain failure),
// dlls/d3d10core/tests/d3d10core.c:1256, 1679-1681 (explicit adapter with HARDWARE, level 10_0), and
// dlls/d3d10/tests/device.c:37-44 (SDK versions). Direct3D 10 accepts an explicit adapter with HARDWARE, unlike 11.
// a valid level beyond device support returns DXGI_ERROR_UNSUPPORTED (Microsoft Learn, DXGI_ERROR):
// https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-error
// driver and flag controls use the named values, preserving creation for WARP and debug requests:
// https://learn.microsoft.com/en-us/windows/win32/api/d3dcommon/ne-d3dcommon-d3d_driver_type
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_create_device_flag
// Direct3D 10's HardwareLevel is a D3D10_FEATURE_LEVEL1, whose greatest value is D3D10_FEATURE_LEVEL_10_1:
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10_1/nf-d3d10_1-d3d10createdevice1
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10_1/ne-d3d10_1-d3d10_feature_level1
// D3D10 GetCreationFlags returns "A bitfield containing the flags used to create the device."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-getcreationflags
// every combination of the ordinary flags is checked, along with the default multithread protection. Debug
// layers and the Windows-7-only ALLOW_NULL_FROM_MAP flag have separate prerequisites and are not assumed present.
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <algorithm>
#include <limits>

int
main() {
  ComPtr<IDXGIFactory> factory;
  ComPtr<IDXGIAdapter> adapter;
  CHECK(CreateDXGIFactory(IID_PPV_ARGS(&factory)));
  CHECK(factory->EnumAdapters(0, &adapter));
  HWND window = CreateWindowA("static", "device arguments", 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
  if (!expect(window != nullptr, "could not create window"))
    return verdict();
  DXGI_SWAP_CHAIN_DESC desc{};
  desc.BufferDesc.Width = desc.BufferDesc.Height = 1;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 1;
  desc.OutputWindow = window;
  desc.Windowed = TRUE;

  const D3D_FEATURE_LEVEL probe_levels[] = {
      D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1};
  D3D_FEATURE_LEVEL maximum{};
  step("D3D11CreateDevice pFeatureLevel: query supported levels for valid-call controls");
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, probe_levels, ARRAYSIZE(probe_levels),
                          D3D11_SDK_VERSION, nullptr, &maximum, nullptr));
  const auto default_level = std::min(maximum, D3D_FEATURE_LEVEL_11_0);

  const D3D_FEATURE_LEVEL defaults[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                      D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1};
  struct Call {
    IDXGIAdapter *adapter = nullptr;
    D3D_DRIVER_TYPE driver = D3D_DRIVER_TYPE_HARDWARE;
    HMODULE software = nullptr;
    UINT flags = 0;
    const D3D_FEATURE_LEVEL *levels = nullptr;
    UINT count = 0, sdk = D3D11_SDK_VERSION;
    HRESULT result = S_OK;
    D3D_FEATURE_LEVEL chosen{};
  };
  auto check = [&](const char *rule, const Call &call, const DXGI_SWAP_CHAIN_DESC *swap_desc) {
    const auto expected_level = call.chosen ? call.chosen : default_level;
    for (bool combined : {false, true}) {
      for (bool device_output : {false, true}) {
        for (bool context_output : {false, true}) {
          for (bool level_output : {false, true}) {
            for (bool swap_output : {false, true}) {
              if (swap_output && !combined)
                continue;
              step("%s: adapter %p driver %u software %p flags %#x SDK %u levels %u, outputs %u/%u/%u/%u/%u",
                   rule, call.adapter, call.driver, call.software, call.flags, call.sdk, call.count, combined,
                   device_output, context_output, level_output, swap_output);
              auto device_sentinel = reinterpret_cast<ID3D11Device *>(factory.Get());
              auto context_sentinel = reinterpret_cast<ID3D11DeviceContext *>(factory.Get());
              auto swap_sentinel = reinterpret_cast<IDXGISwapChain *>(factory.Get());
              auto device = device_sentinel;
              auto context = context_sentinel;
              auto swap = swap_sentinel;
              auto chosen = D3D_FEATURE_LEVEL(~0u);
              auto hr = combined
                            ? D3D11CreateDeviceAndSwapChain(
                                  call.adapter, call.driver, call.software, call.flags, call.levels, call.count,
                                  call.sdk, swap_desc, swap_output ? &swap : nullptr, device_output ? &device : nullptr,
                                  level_output ? &chosen : nullptr, context_output ? &context : nullptr)
                            : D3D11CreateDevice(call.adapter, call.driver, call.software, call.flags, call.levels,
                                                call.count, call.sdk, device_output ? &device : nullptr,
                                                level_output ? &chosen : nullptr, context_output ? &context : nullptr);
              HRESULT want = call.result;
              if (SUCCEEDED(want)) {
                want = device_output || context_output || swap_output ? S_OK : S_FALSE;
                if (swap_output && !swap_desc)
                  want = E_INVALIDARG;
                else if (swap_output && !swap_desc->OutputWindow)
                  want = DXGI_ERROR_INVALID_CALL;
                else if (swap_output && swap_desc->BufferDesc.Format == DXGI_FORMAT_BC5_UNORM)
                  want = E_INVALIDARG;
              }
              expect(hr == want, "HRESULT %#lx, expected %#lx", hr, want);
              expect(!device_output || (FAILED(want) ? device == nullptr : device && device != device_sentinel),
                     "device output %p", device);
              expect(!context_output || (FAILED(want) ? context == nullptr : context && context != context_sentinel),
                     "context output %p", context);
              expect(!swap_output || (FAILED(want) ? swap == nullptr : swap && swap != swap_sentinel),
                     "swap-chain output %p", swap);
              expect(!level_output || chosen == (FAILED(want) ? D3D_FEATURE_LEVEL(0) : expected_level),
                     "level output %#x", chosen);
              if (device_output && device && device != device_sentinel) {
                expect(device->GetFeatureLevel() == expected_level || FAILED(want), "device level differs");
                if (SUCCEEDED(want))
                  expect(device->GetCreationFlags() == call.flags, "device flags differ");
                device->Release();
              }
              if (context_output && context && context != context_sentinel)
                context->Release();
              if (swap_output && swap && swap != swap_sentinel)
                swap->Release();
            }
          }
        }
      }
    }
  };

  // Wine d3d11.c:2181-2186 records E_INVALIDARG and clears every output for UNKNOWN without an adapter.
  Call call;
  // D3D11CreateDevice Remarks require UNKNOWN and no software module with an adapter. Software in
  // D3D11CreateDeviceAndSwapChain requires a module only for SOFTWARE; DriverType is a D3D_DRIVER_TYPE.
  for (auto driver : {D3D_DRIVER_TYPE_UNKNOWN, D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_REFERENCE,
                      D3D_DRIVER_TYPE_NULL, D3D_DRIVER_TYPE_SOFTWARE, D3D_DRIVER_TYPE_WARP,
                      D3D_DRIVER_TYPE(D3D_DRIVER_TYPE_WARP + 1), D3D_DRIVER_TYPE(~0u)}) {
    for (auto supplied : {static_cast<IDXGIAdapter *>(nullptr), adapter.Get()}) {
      for (auto module : {HMODULE(nullptr), GetModuleHandleA(nullptr)}) {
        call = {};
        call.adapter = supplied;
        call.driver = driver;
        call.software = module;
        if (supplied) {
          call.result = driver == D3D_DRIVER_TYPE_UNKNOWN && !module ? S_OK : E_INVALIDARG;
        } else if (driver == D3D_DRIVER_TYPE_UNKNOWN || driver > D3D_DRIVER_TYPE_WARP ||
                   (driver == D3D_DRIVER_TYPE_SOFTWARE ? !module : module != nullptr)) {
          call.result = E_INVALIDARG;
        } else if (driver != D3D_DRIVER_TYPE_HARDWARE && driver != D3D_DRIVER_TYPE_WARP) {
          continue;
        }
        check("Wine d3d11.c:2181-2186, D3D11CreateDevice Remarks: adapter/driver/software dependencies", call, &desc);
      }
    }
  }
  // D3D11CreateDevice SDKVersion specifies D3D11_SDK_VERSION.
  for (UINT sdk : {0u, UINT(D3D11_SDK_VERSION - 1), UINT(D3D11_SDK_VERSION), UINT(D3D11_SDK_VERSION + 1),
                   std::numeric_limits<UINT>::max()}) {
    call = {};
    call.sdk = sdk;
    call.result = sdk == D3D11_SDK_VERSION ? S_OK : E_INVALIDARG;
    check("D3D11CreateDevice SDKVersion", call, &desc);
  }
  // D3D11CreateDevice Flags allows D3D11_CREATE_DEVICE_FLAG values, including DEBUG, combined with OR.
  const UINT flags[] = {D3D11_CREATE_DEVICE_SINGLETHREADED, D3D11_CREATE_DEVICE_DEBUG,
                        D3D11_CREATE_DEVICE_SWITCH_TO_REF, D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,
                        D3D11_CREATE_DEVICE_BGRA_SUPPORT, D3D11_CREATE_DEVICE_DEBUGGABLE,
                        D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY,
                        D3D11_CREATE_DEVICE_DISABLE_GPU_TIMEOUT, D3D11_CREATE_DEVICE_VIDEO_SUPPORT};
  UINT named_flags = 0;
  for (auto flag : flags) {
    named_flags |= flag;
    call = {};
    call.flags = flag;
    check("D3D11_CREATE_DEVICE_FLAG: each named flag", call, &desc);
  }
  call = {};
  call.flags = named_flags;
  check("D3D11CreateDevice Flags: named values can be ORed", call, &desc);

  // D3D11CreateDevice pFeatureLevels defines the default array when the pointer is NULL.
  for (UINT count : {0u, 1u, UINT(ARRAYSIZE(defaults)), UINT(ARRAYSIZE(defaults) + 1)}) {
    call = {};
    call.count = count;
    check("D3D11CreateDevice pFeatureLevels: NULL uses the default array", call, &desc);
  }
  // D3D11CreateDevice pFeatureLevels takes D3D_FEATURE_LEVEL entries; its 11_1 runtime note rejects
  // the presence of 11_1 immediately on older runtimes, even after a supported entry.
  for (auto level : probe_levels) {
    call = {};
    call.levels = &level;
    call.count = 1;
    call.chosen = level;
    call.result = level <= maximum ? S_OK : DXGI_ERROR_UNSUPPORTED;
    check("D3D_FEATURE_LEVEL: each defined graphics level against queried support", call, &desc);
    for (auto invalid : {D3D_FEATURE_LEVEL(level - 1), D3D_FEATURE_LEVEL(level + 1), D3D_FEATURE_LEVEL(0),
                         D3D_FEATURE_LEVEL(~0u)}) {
      for (bool first : {false, true}) {
        D3D_FEATURE_LEVEL requested[] = {first ? invalid : level, first ? level : invalid};
        call.levels = requested;
        call.count = ARRAYSIZE(requested);
        call.result = E_INVALIDARG;
        check("D3D_FEATURE_LEVEL: invalid element before or after a supported level", call, &desc);
      }
    }
  }
  // D3D11CreateDevice pFeatureLevel returns the first requested level that succeeds, in array order.
  D3D_FEATURE_LEVEL ordered[] = {D3D_FEATURE_LEVEL_9_1, D3D_FEATURE_LEVEL_11_0};
  call = {};
  call.levels = ordered;
  call.count = ARRAYSIZE(ordered);
  call.chosen = ordered[0];
  check("D3D11CreateDevice pFeatureLevel: first supported entry, not maximum", call, &desc);
  call = {};
  call.levels = defaults;
  call.count = ARRAYSIZE(defaults);
  check("Wine d3d11.c:2158: explicit default feature levels", call, &desc);
  // D3D11CreateDevice FeatureLevels is the number of array elements, with no default-array length limit.
  std::vector<D3D_FEATURE_LEVEL> repeated(ARRAYSIZE(defaults) + 1, default_level);
  call.levels = repeated.data();
  call.count = repeated.size();
  check("D3D11CreateDevice FeatureLevels: explicit array longer than the default", call, &desc);
  repeated.back() = D3D_FEATURE_LEVEL(0);
  call.result = E_INVALIDARG;
  check("D3D_FEATURE_LEVEL: invalid final entry beyond the default array length", call, &desc);
  // DXGI_ERROR_UNSUPPORTED denotes a requested feature the device cannot support; the pFeatureLevel
  // sentence still permits a later supported entry.
  D3D_FEATURE_LEVEL unsupported[] = {D3D_FEATURE_LEVEL_12_2, default_level};
  call = {};
  call.levels = unsupported;
  call.count = 1;
  call.result = DXGI_ERROR_UNSUPPORTED;
  check("DXGI_ERROR: no requested level is supported", call, &desc);
  call.count = ARRAYSIZE(unsupported);
  call.result = S_OK;
  check("D3D11CreateDevice pFeatureLevel: skip an unsupported level", call, &desc);
  call = {};
  check("D3D11CreateDeviceAndSwapChain pSwapChainDesc: swap-chain initialization parameters", call, nullptr);
  // Wine d3d11.c:2297-2331 records both failures and success when no swap-chain output is requested.
  desc.OutputWindow = nullptr;
  check("Wine d3d11.c:2311-2317: invalid window, ignored without swap output", call, &desc);
  desc.OutputWindow = window;
  desc.BufferDesc.Format = DXGI_FORMAT_BC5_UNORM;
  check("Wine d3d11.c:2325-2332: BC5 swap-chain format, ignored without swap output", call, &desc);
  HMODULE core = LoadLibraryA("d3d10core.dll"), api = LoadLibraryA("d3d10_1.dll"), api10 = LoadLibraryA("d3d10.dll");
  using CoreCreate = HRESULT(WINAPI *)(IDXGIFactory *, IDXGIAdapter *, UINT, D3D_FEATURE_LEVEL, ID3D10Device **);
  auto create_core = reinterpret_cast<CoreCreate>(GetProcAddress(core, "D3D10CoreCreateDevice"));
  auto create1 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(GetProcAddress(api, "D3D10CreateDevice1"));
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice)>(GetProcAddress(api10, "D3D10CreateDevice"));
  auto create_swap1 = reinterpret_cast<decltype(&D3D10CreateDeviceAndSwapChain1)>(
      GetProcAddress(api, "D3D10CreateDeviceAndSwapChain1"));
  if (!expect(create_core && create1 && create10 && create_swap1, "Direct3D 10 creation exports unavailable"))
    return verdict();
  // Wine d3d10_1.c:128-142 requires an output and rejects level 0. D3D10CreateDevice1 HardwareLevel
  // takes a D3D10_FEATURE_LEVEL1; call the core directly so its own checks are exercised.
  for (auto level : {D3D_FEATURE_LEVEL(0), D3D_FEATURE_LEVEL_9_1, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_3,
                     D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_11_0,
                     D3D_FEATURE_LEVEL(D3D_FEATURE_LEVEL_10_0 + 1), D3D_FEATURE_LEVEL(~0u)}) {
    for (bool output : {false, true}) {
      step("Wine d3d10_1.c:128-142, D3D10_FEATURE_LEVEL1: core level %#x, output %u", level, output);
      auto sentinel = reinterpret_cast<ID3D10Device *>(factory.Get());
      auto device = sentinel;
      auto hr = create_core(factory.Get(), adapter.Get(), 0, level, output ? &device : nullptr);
      const bool valid = output && (level == D3D_FEATURE_LEVEL_9_1 || level == D3D_FEATURE_LEVEL_9_2 ||
                                    level == D3D_FEATURE_LEVEL_9_3 || level == D3D_FEATURE_LEVEL_10_0 ||
                                    level == D3D_FEATURE_LEVEL_10_1);
      expect(hr == (valid ? S_OK : E_INVALIDARG), "core HRESULT %#lx", hr);
      expect(!output || (valid ? device && device != sentinel : device == nullptr), "core output %p", device);
      if (output && device && device != sentinel) {
        ComPtr<ID3D11Device> device11;
        expect(device->QueryInterface(IID_PPV_ARGS(&device11)) == S_OK && device11 &&
                   device11->GetFeatureLevel() == level,
               "D3D10 core feature level differs");
        device->Release();
      }
    }
  }
  // Wine d3d10core.c:1256 accepts an explicit adapter with HARDWARE; 1679-1681 records level 10_0.
  for (auto supplied : {static_cast<IDXGIAdapter *>(nullptr), adapter.Get()}) {
    step("Wine d3d10core.c:1256, 1679-1681: D3D10 HARDWARE with adapter %p", supplied);
    ComPtr<ID3D10Device> device;
    CHECK(create10(supplied, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_SDK_VERSION, &device));
    ComPtr<ID3D11Device> device11;
    CHECK(device.As(&device11));
    expect(device11->GetFeatureLevel() == D3D_FEATURE_LEVEL_10_0, "D3D10CreateDevice did not select 10_0");
  }
  // Wine dlls/d3d10/tests/device.c:29, 36-41 accepts D3D10_SDK_VERSION and rejects every other value.
  for (UINT sdk : {0u, UINT(D3D10_SDK_VERSION - 1), UINT(D3D10_SDK_VERSION), UINT(D3D10_SDK_VERSION + 1),
                   std::numeric_limits<UINT>::max()}) {
    step("Wine dlls/d3d10/tests/device.c:37-44: SDK %u", sdk);
    ComPtr<ID3D10Device> device;
    auto hr = create10(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, sdk, &device);
    expect(hr == (sdk == D3D10_SDK_VERSION ? S_OK : E_INVALIDARG), "D3D10 SDK HRESULT %#lx", hr);
    expect(bool(device) == (sdk == D3D10_SDK_VERSION), "D3D10 SDK device %p", device.Get());
  }
  auto sentinel10 = reinterpret_cast<ID3D10Device1 *>(factory.Get());
  const UINT flags10[] = {
      D3D10_CREATE_DEVICE_SINGLETHREADED, D3D10_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,
      D3D10_CREATE_DEVICE_BGRA_SUPPORT, D3D10_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY
  };
  const D3D10_FEATURE_LEVEL1 levels10[] = {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1};
  bool bgra_support[std::size(levels10)] = {true, true};
  for (UINT i = 0; i < std::size(levels10); i++) {
    ComPtr<ID3D10Device1> control;
    CHECK(create1(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, levels10[i], D3D10_1_SDK_VERSION, &control));
    for (auto format :
         {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
          DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB}) {
      UINT support = 0;
      bgra_support[i] &=
          SUCCEEDED(control->CheckFormatSupport(format, &support)) && (support & D3D10_FORMAT_SUPPORT_TEXTURE2D);
    }
  }
  for (UINT combination = 0; combination < (1u << std::size(flags10)); combination++) {
    UINT mask = 0;
    for (UINT i = 0; i < std::size(flags10); i++)
      if (combination & (1u << i))
        mask |= flags10[i];
    for (UINT i = 0; i < std::size(levels10); i++) {
      auto level = levels10[i];
      step("Direct3D 10 level %#x creation flags %#x", level, mask);
      ComPtr<ID3D10Device1> device;
      auto hr = create1(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, mask, level, D3D10_1_SDK_VERSION, &device);
      if ((mask & D3D10_CREATE_DEVICE_BGRA_SUPPORT) && !bgra_support[i]) {
        expect(FAILED(hr) && !device, "BGRA flag accepted without BGRA support");
        continue;
      }
      if (!expect(hr == S_OK && device, "D3D10 flag creation returned %#lx", hr))
        return verdict();
      expect(device->GetCreationFlags() == mask, "D3D10 creation flags differ: %#x", device->GetCreationFlags());
      ComPtr<ID3D10Multithread> multithread;
      CHECK(device.As(&multithread));
      BOOL initial = !(mask & D3D10_CREATE_DEVICE_SINGLETHREADED);
      expect(multithread->GetMultithreadProtected() == initial, "initial D3D10 multithread protection");
      expect(
          multithread->SetMultithreadProtected(!initial) == initial &&
              multithread->GetMultithreadProtected() == !initial,
          "changed D3D10 multithread protection"
      );
      multithread->Enter();
      multithread->Enter();
      multithread->Leave();
      multithread->Leave();
      expect(multithread->SetMultithreadProtected(initial) == !initial, "restored D3D10 multithread protection");
    }
    ComPtr<ID3D10Device> device;
    CHECK(create10(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, mask, D3D10_SDK_VERSION, &device));
    expect(device->GetCreationFlags() == mask, "D3D10CreateDevice creation flags differ");
  }
  auto create_swap10 = reinterpret_cast<decltype(&D3D10CreateDeviceAndSwapChain)>(
      GetProcAddress(api10, "D3D10CreateDeviceAndSwapChain")
  );
  if (!expect(create_swap10 != nullptr, "D3D10CreateDeviceAndSwapChain unavailable"))
    return verdict();
  for (auto supplied : {static_cast<IDXGIAdapter *>(nullptr), adapter.Get()}) {
    step("D3D10CreateDeviceAndSwapChain explicit adapter %u", supplied != nullptr);
    ComPtr<ID3D10Device> device;
    ComPtr<IDXGISwapChain> swap;
    desc.OutputWindow = window;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    CHECK(create_swap10(supplied, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_SDK_VERSION, &desc, &swap, &device));
    ComPtr<ID3D10Device> swap_device;
    CHECK(swap->GetDevice(IID_PPV_ARGS(&swap_device)));
    expect(device == swap_device, "D3D10 swap chain belongs to a different device");
    ComPtr<ID3D10Texture2D> backbuffer;
    CHECK(swap->GetBuffer(0, IID_PPV_ARGS(&backbuffer)));
    D3D10_TEXTURE2D_DESC got;
    backbuffer->GetDesc(&got);
    expect(
        got.Width == desc.BufferDesc.Width && got.Height == desc.BufferDesc.Height &&
            got.Format == desc.BufferDesc.Format,
        "D3D10 combined creation backbuffer descriptor"
    );
  }
  // Wine d3d10_1.c:162, 214-265 records these output, level, window and format combinations.
  const struct {
    bool device, swap, description;
    D3D10_FEATURE_LEVEL1 level;
    HWND window;
    DXGI_FORMAT format;
    HRESULT result;
  } swap_calls[] = {
      {true, true, true, D3D10_FEATURE_LEVEL_10_0, window, DXGI_FORMAT_R8G8B8A8_UNORM, S_OK},
      {true, false, false, D3D10_FEATURE_LEVEL_10_0, window, DXGI_FORMAT_R8G8B8A8_UNORM, S_OK},
      {false, false, false, D3D10_FEATURE_LEVEL_10_0, window, DXGI_FORMAT_R8G8B8A8_UNORM, E_INVALIDARG},
      {false, false, true, D3D10_FEATURE_LEVEL_10_0, window, DXGI_FORMAT_R8G8B8A8_UNORM, E_INVALIDARG},
      {true, true, true, D3D10_FEATURE_LEVEL1(0), window, DXGI_FORMAT_R8G8B8A8_UNORM, E_INVALIDARG},
      {false, true, true, D3D10_FEATURE_LEVEL_10_0, window, DXGI_FORMAT_R8G8B8A8_UNORM, E_INVALIDARG},
      {true, false, true, D3D10_FEATURE_LEVEL_10_0, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, S_OK},
      {true, true, true, D3D10_FEATURE_LEVEL_10_0, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ERROR_INVALID_CALL},
      {true, true, true, D3D10_FEATURE_LEVEL_10_0, window, DXGI_FORMAT_BC5_UNORM, E_INVALIDARG},
  };
  for (const auto &args : swap_calls) {
    step("Wine d3d10_1.c:162, 214-269: device/swap/description %u/%u/%u, level %#x, window %p, format %#x",
         args.device, args.swap, args.description, args.level, args.window, args.format);
    desc.OutputWindow = args.window;
    desc.BufferDesc.Format = args.format;
    auto device = sentinel10;
    auto swap_sentinel = reinterpret_cast<IDXGISwapChain *>(factory.Get());
    auto swap = swap_sentinel;
    auto hr = create_swap1(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, args.level, D3D10_1_SDK_VERSION,
                           args.description ? &desc : nullptr, args.swap ? &swap : nullptr,
                           args.device ? &device : nullptr);
    expect(hr == args.result, "D3D10.1 swap HRESULT %#lx, expected %#lx", hr, args.result);
    expect(!args.device || (FAILED(args.result) ? device == nullptr : device && device != sentinel10),
           "D3D10.1 device %p", device);
    expect(!args.swap || (FAILED(args.result) ? swap == nullptr : swap && swap != swap_sentinel),
           "D3D10.1 swap %p", swap);
    if (args.device && device && device != sentinel10)
      device->Release();
    if (args.swap && swap && swap != swap_sentinel)
      swap->Release();
  }
  DestroyWindow(window);
  // Wine d3d10_1.c:108-142 and D3D10_FEATURE_LEVEL1 supply the accepted levels and invalid output/level cases.
  for (auto level : {D3D10_FEATURE_LEVEL1(0), D3D10_FEATURE_LEVEL_9_1, D3D10_FEATURE_LEVEL_9_2,
                     D3D10_FEATURE_LEVEL_9_3, D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    for (bool output : {false, true}) {
      step("Wine d3d10_1.c:108-143: level %#x, output %u", level, output);
      auto sentinel = reinterpret_cast<ID3D10Device1 *>(factory.Get());
      auto device = sentinel;
      auto hr = create1(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, level, D3D10_1_SDK_VERSION,
                        output ? &device : nullptr);
      const bool valid = level && output;
      expect(hr == (valid ? S_OK : E_INVALIDARG), "D3D10.1 HRESULT %#lx", hr);
      expect(!output || (valid ? device && device != sentinel : device == nullptr), "D3D10.1 output %p", device);
      if (output && device && device != sentinel)
        device->Release();
    }
  }
  FreeLibrary(core);
  FreeLibrary(api);
  FreeLibrary(api10);
  return verdict();
}
