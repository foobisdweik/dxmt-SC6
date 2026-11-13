// contract: factory creation preserves its flags and exposes coherent adapter
// enumeration through every interface. "When DXGI_GPU_PREFERENCE_UNSPECIFIED is
// specified ... this method is equivalent to calling
// IDXGIFactory1::EnumAdapters1." (Microsoft Learn,
// IDXGIFactory6::EnumAdapterByGpuPreference, Remarks.)
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_6/nf-dxgi1_6-idxgifactory6-enumadapterbygpupreference
// MakeWindowAssociation: "This parameter can be NULL; but only if Flags is also
// 0."
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgifactory-makewindowassociation
// Wine's Windows test_window_association accepts NULL with nonzero flags and
// observes a NULL association after successful MakeWindowAssociation. those
// observed exceptions are kept separate from the documented restriction.
#include "../d3d12/d3d12_test.hpp"
#include <dxgi1_6.h>
#include <algorithm>
#include <array>
#include <limits>

static bool
same(LUID a, LUID b) {
  return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

int
main() {
  const IID versions[] = {__uuidof(IUnknown),      __uuidof(IDXGIObject),   __uuidof(IDXGIFactory),
                          __uuidof(IDXGIFactory1), __uuidof(IDXGIFactory2), __uuidof(IDXGIFactory3),
                          __uuidof(IDXGIFactory4), __uuidof(IDXGIFactory5), __uuidof(IDXGIFactory6),
                          __uuidof(IDXGIFactory7)};
  const IID adapter_versions[] = {
      __uuidof(IDXGIAdapter), __uuidof(IDXGIAdapter1), __uuidof(IDXGIAdapter2), __uuidof(IDXGIAdapter3),
      __uuidof(IDXGIAdapter4)
  };
  HWND window = CreateWindowA(
      "static", "dxgi_factory", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr
  );
  if (!expect(window != nullptr, "CreateWindow: %lu", GetLastError()))
    return verdict();
  for (UINT entry = 0; entry < 3; entry++) {
    for (UINT flags : {0u, UINT(DXGI_CREATE_FACTORY_DEBUG)}) {
      if (entry != 2 && flags)
        continue;
      step("factory entry %u flags %#x", entry, flags);
      auto create = [&](REFIID iid, void **output) {
        return entry == 0   ? CreateDXGIFactory(iid, output)
               : entry == 1 ? CreateDXGIFactory1(iid, output)
                            : CreateDXGIFactory2(flags, iid, output);
      };
      ComPtr<IDXGIFactory> base;
      HRESULT hr = create(IID_PPV_ARGS(&base));
      if (!expect(hr == S_OK && base, "factory creation: %#lx", hr))
        continue;
      ComPtr<IUnknown> identity;
      CHECK(base.As(&identity));
      for (UINT v = 0; v < ARRAYSIZE(versions); v++) {
        step("factory entry %u flags %#x interface %u", entry, flags, v);
        ComPtr<IUnknown> object, unknown;
        hr = base->QueryInterface(versions[v], &object);
        ComPtr<IUnknown> requested;
        HRESULT made = create(versions[v], reinterpret_cast<void **>(requested.GetAddressOf()));
        expect(made == hr && bool(requested) == bool(object), "creation IID differs from QI: %#lx / %#lx", made, hr);
        // DXGI 1.0 deliberately hides Factory1; newer optional interfaces may
        // be absent.
        if (!entry && versions[v] == __uuidof(IDXGIFactory1)) {
          expect(hr == E_NOINTERFACE && !object, "legacy Factory1: %#lx", hr);
          continue;
        }
        if (v == ARRAYSIZE(versions) - 1 && hr == E_NOINTERFACE)
          continue;
        if (expect(hr == S_OK && object, "factory QI: %#lx", hr)) {
          CHECK(object.As(&unknown));
          expect(unknown == identity, "factory interface changes IUnknown");
          ComPtr<IDXGIFactory3> factory3;
          if (SUCCEEDED(object.As(&factory3)))
            expect(factory3->GetCreationFlags() == flags, "creation flags %#x", factory3->GetCreationFlags());
        }
      }
      ComPtr<IDXGIAdapter> bad;
      expect(base.As(&bad) == E_NOINTERFACE && !bad, "factory supports adapter IID");
      void *unsupported = base.Get();
      expect(
          create(__uuidof(IDXGIAdapter), &unsupported) == E_NOINTERFACE && !unsupported,
          "unsupported creation IID was not cleared"
      );
      expect(FAILED(create(__uuidof(IDXGIFactory), nullptr)), "NULL factory output accepted");
      ComPtr<IDXGIFactory> parent;
      expect(base->GetParent(IID_PPV_ARGS(&parent)) == E_NOINTERFACE && !parent, "factory has a parent");
      ComPtr<IDXGIFactory6> factory;
      if (FAILED(base.As(&factory))) {
        expect(false, "no Factory6 for the supported DXGI path");
        continue;
      }
      expect(factory->IsCurrent(), "new factory is stale");
      std::vector<LUID> luids;
      for (UINT index = 0;; index++) {
        step("factory entry %u flags %#x adapter %u", entry, flags, index);
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIAdapter> legacy;
        hr = factory->EnumAdapters1(index, &adapter);
        HRESULT old = factory->EnumAdapters(index, &legacy);
        expect(hr == old, "enumerators disagree: %#lx / %#lx", hr, old);
        if (hr == DXGI_ERROR_NOT_FOUND) {
          expect(!adapter && !legacy, "past-end outputs not cleared");
          break;
        }
        if (!expect(hr == S_OK && adapter && legacy, "adapter enumeration: %#lx", hr))
          break;
        DXGI_ADAPTER_DESC1 desc{};
        DXGI_ADAPTER_DESC old_desc{};
        CHECK(adapter->GetDesc1(&desc));
        CHECK(legacy->GetDesc(&old_desc));
        expect(same(desc.AdapterLuid, old_desc.AdapterLuid), "enumerators disagree on LUID");
        expect(
            std::none_of(luids.begin(), luids.end(), [&](LUID l) { return same(l, desc.AdapterLuid); }),
            "duplicate adapter LUID"
        );
        luids.push_back(desc.AdapterLuid);
        for (const IID &iid : adapter_versions) {
          ComPtr<IDXGIAdapter> found;
          hr = factory->EnumAdapterByLuid(desc.AdapterLuid, iid, reinterpret_cast<void **>(found.GetAddressOf()));
          if (expect(hr == S_OK && found, "LUID lookup: %#lx", hr)) {
            DXGI_ADAPTER_DESC got{};
            CHECK(found->GetDesc(&got));
            expect(same(got.AdapterLuid, desc.AdapterLuid), "lookup returned a different LUID");
          }
        }
      }
      expect(!luids.empty(), "no adapters");
      for (auto preference :
           {DXGI_GPU_PREFERENCE_UNSPECIFIED, DXGI_GPU_PREFERENCE_MINIMUM_POWER, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE}) {
        std::vector<LUID> seen;
        for (UINT index = 0; index <= luids.size(); index++) {
          step("factory entry %u flags %#x preference %u index %u", entry, flags, preference, index);
          ComPtr<IDXGIAdapter1> adapter;
          hr = factory->EnumAdapterByGpuPreference(index, preference, IID_PPV_ARGS(&adapter));
          if (index == luids.size()) {
            expect(hr == DXGI_ERROR_NOT_FOUND && !adapter, "preference past end: %#lx", hr);
            auto sentinel = reinterpret_cast<IDXGIAdapter1 *>(base.Get());
            IDXGIAdapter1 *populated = sentinel;
            HRESULT failed = factory->EnumAdapterByGpuPreference(index, preference, IID_PPV_ARGS(&populated));
            expect(failed == DXGI_ERROR_NOT_FOUND && !populated, "past-end preference did not clear populated output");
            if (SUCCEEDED(failed) && populated && populated != sentinel)
              populated->Release();
            continue;
          }
          if (!expect(hr == S_OK && adapter, "preference enumeration: %#lx", hr))
            continue;
          DXGI_ADAPTER_DESC1 desc{};
          CHECK(adapter->GetDesc1(&desc));
          expect(
              std::any_of(luids.begin(), luids.end(), [&](LUID l) { return same(l, desc.AdapterLuid); }),
              "preference returned an unenumerated adapter"
          );
          expect(
              std::none_of(seen.begin(), seen.end(), [&](LUID l) { return same(l, desc.AdapterLuid); }),
              "preference duplicated an adapter"
          );
          if (preference == DXGI_GPU_PREFERENCE_UNSPECIFIED)
            expect(same(desc.AdapterLuid, luids[index]), "unspecified preference changes order");
          seen.push_back(desc.AdapterLuid);
        }
      }
      LUID missing{};
      while (std::any_of(luids.begin(), luids.end(), [&](LUID l) { return same(l, missing); }))
        missing.LowPart++;
      ComPtr<IDXGIAdapter> absent;
      expect(
          factory->EnumAdapterByLuid(missing, IID_PPV_ARGS(&absent)) == DXGI_ERROR_NOT_FOUND && !absent,
          "absent LUID lookup"
      );
      expect(
          factory->EnumAdapters(std::numeric_limits<UINT>::max(), &absent) == DXGI_ERROR_NOT_FOUND && !absent,
          "UINT_MAX adapter index"
      );
      alignas(BOOL) std::array<BYTE, sizeof(BOOL) + 1> data;
      for (UINT size = 0; size <= data.size(); size++) {
        step("factory entry %u flags %#x tearing data size %u", entry, flags, size);
        data.fill(std::numeric_limits<BYTE>::max());
        hr = factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, data.data(), size);
        expect(hr == (size == sizeof(BOOL) ? S_OK : E_INVALIDARG), "feature size: %#lx", hr);
        expect(data.back() == std::numeric_limits<BYTE>::max(), "feature query wrote beyond BOOL");
        if (size == sizeof(BOOL)) {
          BOOL supported;
          memcpy(&supported, data.data(), sizeof(supported));
          expect(supported == FALSE || supported == TRUE, "non-BOOL tearing capability");
        }
      }
      expect(
          factory->CheckFeatureSupport(
              DXGI_FEATURE(DXGI_FEATURE_PRESENT_ALLOW_TEARING + 1), data.data(), sizeof(BOOL)
          ) == E_INVALIDARG,
          "unknown feature"
      );
      for (UINT flags = 0; flags <= DXGI_MWA_VALID; flags++) {
        step("factory entry %u association flags %#x", entry, flags);
        expect(factory->MakeWindowAssociation(window, flags) == S_OK, "valid window association");
        HWND associated = nullptr;
        CHECK(factory->GetWindowAssociation(&associated));
        expect(!associated, "GetWindowAssociation differs from the Windows observation");
        HRESULT association = factory->MakeWindowAssociation(nullptr, flags);
        expect(association == S_OK || (flags && association == DXGI_ERROR_INVALID_CALL), "NULL window with flags");
        if (!flags) {
          associated = window;
          CHECK(factory->GetWindowAssociation(&associated));
          expect(!associated, "window association did not clear");
        }
      }
      expect(factory->GetWindowAssociation(nullptr) == DXGI_ERROR_INVALID_CALL, "NULL association output");
      expect(
          factory->MakeWindowAssociation(window, DXGI_MWA_VALID + 1) == DXGI_ERROR_INVALID_CALL,
          "invalid association flags"
      );
      step("factory entry %u stereo/occlusion registration lifetime", entry);
      // RegisterOcclusionStatusEvent: "multiple times with the same event handle" fails with INVALID_CALL.
      // https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-registerocclusionstatusevent
      std::array<HANDLE, 2> events{
          CreateEventW(nullptr, FALSE, FALSE, nullptr), CreateEventW(nullptr, FALSE, FALSE, nullptr)
      };
      if (!expect(events[0] && events[1], "CreateEvent notifications"))
        return verdict();
      for (bool stereo : {false, true}) {
        std::array<DWORD, 2> cookies{};
        std::array<HRESULT, 2> registered{};
        for (UINT i = 0; i < events.size(); i++) {
          registered[i] = stereo ? factory->RegisterStereoStatusEvent(events[i], &cookies[i])
                                 : factory->RegisterOcclusionStatusEvent(events[i], &cookies[i]);
          expect(registered[i] == S_OK, "status event registration: %#lx", registered[i]);
        }
        if (registered[0] == S_OK && registered[1] == S_OK)
          expect(cookies[0] != cookies[1], "distinct registrations have the same cookie");
        if (!stereo && registered[0] == S_OK) {
          DWORD duplicate = 0;
          expect(
              factory->RegisterOcclusionStatusEvent(events[0], &duplicate) == DXGI_ERROR_INVALID_CALL,
              "duplicate occlusion registration accepted"
          );
        }
        for (UINT i = 0; i < events.size(); i++)
          if (registered[i] == S_OK) {
            if (stereo)
              factory->UnregisterStereoStatus(cookies[i]);
            else
              factory->UnregisterOcclusionStatus(cookies[i]);
          }
        DWORD cookie = 0;
        HRESULT reused = stereo ? factory->RegisterStereoStatusEvent(events[0], &cookie)
                                : factory->RegisterOcclusionStatusEvent(events[0], &cookie);
        expect(reused == S_OK, "unregistered event cannot be reused: %#lx", reused);
        if (reused == S_OK) {
          if (stereo)
            factory->UnregisterStereoStatus(cookie);
          else
            factory->UnregisterOcclusionStatus(cookie);
        }
        if (!stereo && factory->RegisterOcclusionStatusEvent(events[0], &cookie) == S_OK) {
          // a minimized window is occluded (IDXGISwapChain::Present, DXGI_STATUS_OCCLUDED), so both changes notify
          for (int show : {SW_MINIMIZE, SW_RESTORE}) {
            ShowWindow(window, show);
            expect(WaitForSingleObject(events[0], INFINITE) == WAIT_OBJECT_0, "occlusion change %d not signalled", show);
          }
          factory->UnregisterOcclusionStatus(cookie);
          ShowWindow(window, SW_MINIMIZE);
          ShowWindow(window, SW_RESTORE);
          expect(WaitForSingleObject(events[0], 0) == WAIT_TIMEOUT, "unregistered event signalled");
        }
        HRESULT window_status = stereo ? factory->RegisterStereoStatusWindow(window, WM_APP, &cookie)
                                       : factory->RegisterOcclusionStatusWindow(window, WM_APP, &cookie);
        expect(window_status == S_OK, "status window registration: %#lx", window_status);
        if (window_status == S_OK) {
          if (stereo)
            factory->UnregisterStereoStatus(cookie);
          else
            factory->UnregisterOcclusionStatus(cookie);
        }
      }
      DWORD invalid_cookie = 0;
      expect(
          factory->RegisterOcclusionStatusEvent(nullptr, &invalid_cookie) == DXGI_ERROR_INVALID_CALL,
          "invalid occlusion event accepted"
      );
      expect(
          factory->RegisterOcclusionStatusWindow(nullptr, WM_APP, &invalid_cookie) == DXGI_ERROR_INVALID_CALL,
          "invalid occlusion window accepted"
      );
      for (HANDLE event : events)
        expect(CloseHandle(event), "CloseHandle notification");
      expect(factory->IsCurrent(), "enumerating adapters made factory stale");
    }
  }
  DestroyWindow(window);
  return verdict();
}
