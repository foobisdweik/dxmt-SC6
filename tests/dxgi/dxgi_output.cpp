// contract: mode enumeration respects format/flags/capacity, and matching an
// available mode honors its specified fields. "If either Width or Height is 0, both must be
// 0." (FindClosestMatchingMode1, pModeToMatch.)
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutput1-findclosestmatchingmode1
// "DXGI_ERROR_MORE_DATA is returned (if there is not enough room for all the
// display modes)."
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutput1-getdisplaymodelist1
// every named format is supplied by the same generated header as
// d3d11_swapchain_formats. guards surround the caller's array, including when
// its capacity is zero; descriptors and nearest-mode results are checked by
// field.
#include "dxgi_test.hpp"
#include "../depth_formats.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

static bool
same(const auto &a, const auto &b) {
  return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.RefreshRate.Denominator &&
         b.RefreshRate.Denominator &&
         UINT64(a.RefreshRate.Numerator) * b.RefreshRate.Denominator ==
             UINT64(b.RefreshRate.Numerator) * a.RefreshRate.Denominator &&
         a.ScanlineOrdering == b.ScanlineOrdering && a.Scaling == b.Scaling;
}

static bool
honors(const DXGI_MODE_DESC1 &requested, const auto &got) {
  bool refresh = (!requested.RefreshRate.Numerator && !requested.RefreshRate.Denominator) ||
                 UINT64(requested.RefreshRate.Numerator) * got.RefreshRate.Denominator ==
                     UINT64(got.RefreshRate.Numerator) * requested.RefreshRate.Denominator;
  return got.Width == requested.Width && got.Height == requested.Height && got.Format == requested.Format && refresh &&
         got.RefreshRate.Denominator &&
         (requested.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED ||
          got.ScanlineOrdering == requested.ScanlineOrdering) &&
         (requested.Scaling == DXGI_MODE_SCALING_UNSPECIFIED || got.Scaling == requested.Scaling) &&
         got.ScanlineOrdering >= DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED &&
         got.ScanlineOrdering <= DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST &&
         got.Scaling >= DXGI_MODE_SCALING_UNSPECIFIED && got.Scaling <= DXGI_MODE_SCALING_STRETCHED;
}

int
main() {
  const struct {
    DXGI_FORMAT value;
    const char *name;
  } formats[] = {DXGI_FORMATS, {DXGI_FORMAT_FORCE_UINT, "DXGI_FORMAT_FORCE_UINT"}};
  constexpr BYTE canary = 0xa5;
  const UINT all_flags =
      DXGI_ENUM_MODES_INTERLACED | DXGI_ENUM_MODES_SCALING | DXGI_ENUM_MODES_STEREO | DXGI_ENUM_MODES_DISABLED_STEREO;
  const auto display_format = [](DXGI_FORMAT format) {
    // DXGI_MODE_DESC: formats outside this list have no specified enumeration
    // failure code. a successful query still has the ordinary array contract.
    // https://learn.microsoft.com/en-us/previous-versions/windows/desktop/legacy/bb173064(v=vs.85)
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R10G10B10A2_UNORM ||
           format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM;
  };
  ComPtr<IDXGIFactory1> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  for (UINT a = 0;; a++) {
    ComPtr<IDXGIAdapter1> adapter;
    HRESULT hr = factory->EnumAdapters1(a, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;
    CHECK(hr);
    for (UINT o = 0;; o++) {
      step("adapter %u output %u", a, o);
      ComPtr<IDXGIOutput> output;
      hr = adapter->EnumOutputs(o, &output);
      if (hr == DXGI_ERROR_NOT_FOUND) {
        expect(!output, "output past end not cleared");
        break;
      }
      CHECK(hr);
      ComPtr<IDXGIOutput6> modern;
      CHECK(output.As(&modern));
      step(
          "adapter %u output %u duplication rejects objects that are not "
          "Direct3D devices",
          a, o
      );
      // DuplicateOutput/1: E_INVALIDARG if "pDevice is invalid"; a factory is
      // not a Direct3D device.
      // https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgioutput5-duplicateoutput1
      for (IUnknown *invalid : {static_cast<IUnknown *>(nullptr), static_cast<IUnknown *>(factory.Get())}) {
        ComPtr<IDXGIOutputDuplication> duplicate;
        hr = modern->DuplicateOutput(invalid, &duplicate);
        expect(hr == E_INVALIDARG && !duplicate, "invalid duplication device: %#lx", hr);
        const DXGI_FORMAT supported = DXGI_FORMAT_B8G8R8A8_UNORM;
        hr = modern->DuplicateOutput1(invalid, 0, 1, &supported, &duplicate);
        expect(hr == E_INVALIDARG && !duplicate, "invalid duplication1 device: %#lx", hr);
        expect(modern->DuplicateOutput(invalid, nullptr) == E_INVALIDARG, "NULL duplication output");
        expect(
            modern->DuplicateOutput1(invalid, 0, 1, &supported, nullptr) == E_INVALIDARG,
            "NULL duplication1 output"
        );
      }
      ComPtr<IDXGIAdapter1> parent;
      CHECK(output->GetParent(IID_PPV_ARGS(&parent)));
      expect(parent == adapter, "output has a different adapter parent");
      DXGI_OUTPUT_DESC desc{};
      DXGI_OUTPUT_DESC1 desc1{};
      CHECK(output->GetDesc(&desc));
      CHECK(modern->GetDesc1(&desc1));
      expect(
          output->GetDesc(nullptr) == DXGI_ERROR_INVALID_CALL && modern->GetDesc1(nullptr) == DXGI_ERROR_INVALID_CALL,
          "NULL output description"
      );
      expect(
          !wcsncmp(desc.DeviceName, desc1.DeviceName, ARRAYSIZE(desc.DeviceName)) &&
              EqualRect(&desc.DesktopCoordinates, &desc1.DesktopCoordinates) &&
              desc.AttachedToDesktop == desc1.AttachedToDesktop && desc.Rotation == desc1.Rotation &&
              desc.Monitor == desc1.Monitor,
          "output description versions disagree"
      );
      MONITORINFOEXW monitor{};
      monitor.cbSize = sizeof(monitor);
      expect(
          GetMonitorInfoW(desc.Monitor, &monitor) &&
              !wcsncmp(desc.DeviceName, monitor.szDevice, ARRAYSIZE(desc.DeviceName)) &&
              EqualRect(&desc.DesktopCoordinates, &monitor.rcMonitor),
          "description disagrees with its monitor"
      );
      expect(
          desc1.BitsPerColor > 0 && std::isfinite(desc1.MinLuminance) && std::isfinite(desc1.MaxLuminance) &&
              std::isfinite(desc1.MaxFullFrameLuminance) && desc1.MinLuminance >= 0 &&
              desc1.MaxLuminance >= desc1.MinLuminance && desc1.MaxFullFrameLuminance >= desc1.MinLuminance &&
              desc1.MaxFullFrameLuminance <= desc1.MaxLuminance,
          "invalid luminance description"
      );
      for (const float *xy : {desc1.RedPrimary, desc1.GreenPrimary, desc1.BluePrimary, desc1.WhitePoint})
        for (UINT i = 0; i < ARRAYSIZE(desc1.WhitePoint); i++)
          expect(std::isfinite(xy[i]) && xy[i] >= 0 && xy[i] <= 1, "invalid chromaticity");
      expect(
          output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, nullptr, nullptr) == DXGI_ERROR_INVALID_CALL,
          "NULL mode count"
      );
      expect(
          modern->GetDisplayModeList1(DXGI_FORMAT_R8G8B8A8_UNORM, 0, nullptr, nullptr) == DXGI_ERROR_INVALID_CALL,
          "NULL mode1 count"
      );
      for (auto [format, name] : formats)
        for (UINT flags = 0; flags <= all_flags; flags++) {
          step("adapter %u output %u format %s flags %#x", a, o, name, flags);
          UINT count = 0, legacy_count = 0;
          HRESULT modern_hr = modern->GetDisplayModeList1(format, flags, &count, nullptr);
          HRESULT legacy_hr = output->GetDisplayModeList(format, flags, &legacy_count, nullptr);
          if (format == DXGI_FORMAT_UNKNOWN)
            expect(modern_hr == S_OK && legacy_hr == S_OK && !count && !legacy_count, "UNKNOWN mode query");
          if (FAILED(modern_hr) || FAILED(legacy_hr)) {
            if (display_format(format))
              expect(
                  (modern_hr == S_OK || modern_hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) &&
                      (legacy_hr == S_OK || legacy_hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE),
                  "display-format mode query: %#lx / %#lx", modern_hr, legacy_hr
              );
            printf("unchecked: mode array for %s flags %#x, query %#lx / %#lx\n", name, flags, modern_hr, legacy_hr);
            continue;
          }
          expect(count == legacy_count, "mode-list versions count %u / %u", count, legacy_count);
          std::vector<DXGI_MODE_DESC1> modes(count);
          UINT supplied = count;
          if (count)
            CHECK(modern->GetDisplayModeList1(format, flags, &supplied, modes.data()));
          expect(supplied == count, "mode count changed without topology change");
          for (UINT capacity : {0u, 1u, count ? count - 1 : 0u, count, count + 1}) {
            std::vector<DXGI_MODE_DESC1> got(capacity + 2);
            std::vector<DXGI_MODE_DESC> old(capacity + 2);
            memset(got.data(), canary, got.size() * sizeof(got[0]));
            memset(old.data(), canary, old.size() * sizeof(old[0]));
            std::array<BYTE, sizeof(DXGI_MODE_DESC1)> guard;
            guard.fill(canary);
            UINT n = capacity, old_n = capacity;
            HRESULT hr = modern->GetDisplayModeList1(format, flags, &n, got.data() + 1);
            HRESULT old_hr = output->GetDisplayModeList(format, flags, &old_n, old.data() + 1);
            HRESULT wanted = capacity < count ? DXGI_ERROR_MORE_DATA : S_OK;
            expect(
                hr == wanted && old_hr == wanted, "capacity %u: %#lx / %#lx, expected %#lx", capacity, hr, old_hr,
                wanted
            );
            expect(
                !memcmp(&got.front(), guard.data(), sizeof(got[0])) &&
                    !memcmp(&got.back(), guard.data(), sizeof(got[0])) &&
                    !memcmp(&old.front(), guard.data(), sizeof(old[0])) &&
                    !memcmp(&old.back(), guard.data(), sizeof(old[0])),
                "mode list overwrote a guard"
            );
            if (wanted != S_OK)
              continue;
            expect(n == count && old_n == count, "capacity %u returned count %u / %u", capacity, n, old_n);
            for (UINT i = 0; i < std::min(n, count); i++) {
              const auto &mode = got[i + 1];
              expect(
                  mode.Width && mode.Height && mode.Format == format && mode.RefreshRate.Denominator,
                  "mode %u dimensions/format/rate", i
              );
              expect(
                  (flags & (DXGI_ENUM_MODES_STEREO | DXGI_ENUM_MODES_DISABLED_STEREO)) || !mode.Stereo,
                  "unrequested stereo mode"
              );
              expect(
                  (flags & DXGI_ENUM_MODES_INTERLACED) ||
                      (mode.ScanlineOrdering != DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST &&
                       mode.ScanlineOrdering != DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST),
                  "unrequested interlaced mode"
              );
            }
            expect(
                std::is_permutation(
                    old.begin() + 1, old.begin() + 1 + count, got.begin() + 1, got.begin() + 1 + count,
                    [](const auto &a, const auto &b) { return same(a, b); }
                ),
                "mode-list versions differ"
            );
          }
          if (format != DXGI_FORMAT_R8G8B8A8_UNORM || flags != all_flags)
            continue;
          for (const auto &mode : modes) {
            if (mode.Stereo)
              continue;
            step(
                "adapter %u output %u exact mode %ux%u %u/%u scan %u scaling "
                "%u",
                a, o, mode.Width, mode.Height, mode.RefreshRate.Numerator, mode.RefreshRate.Denominator,
                mode.ScanlineOrdering, mode.Scaling
            );
            DXGI_MODE_DESC old{mode.Width,  mode.Height,           mode.RefreshRate,
                               mode.Format, mode.ScanlineOrdering, mode.Scaling};
            DXGI_MODE_DESC matched{};
            DXGI_MODE_DESC1 matched1{};
            hr = output->FindClosestMatchingMode(&old, &matched, nullptr);
            expect(hr == S_OK && honors(mode, matched), "exact legacy mode: %#lx", hr);
            hr = modern->FindClosestMatchingMode1(&mode, &matched1, nullptr);
            expect(hr == S_OK && honors(mode, matched1) && matched1.Stereo == mode.Stereo, "exact mode1: %#lx", hr);
          }
        }
      step("adapter %u output %u invalid closest-mode inputs", a, o);
      DXGI_MODE_DESC1 request{}, matched{};
      expect(
          modern->FindClosestMatchingMode1(&request, &matched, nullptr) == DXGI_ERROR_INVALID_CALL,
          "UNKNOWN format without concerned device"
      );
      request.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      for (bool width : {false, true}) {
        request.Width = width ? 1 : 0;
        request.Height = width ? 0 : 1;
        expect(
            modern->FindClosestMatchingMode1(&request, &matched, nullptr) == DXGI_ERROR_INVALID_CALL,
            "only one zero dimension"
        );
      }
      expect(
          modern->FindClosestMatchingMode1(nullptr, &matched, nullptr) == DXGI_ERROR_INVALID_CALL, "NULL requested mode"
      );
      request.Width = request.Height = 0;
      expect(
          modern->FindClosestMatchingMode1(&request, nullptr, nullptr) == DXGI_ERROR_INVALID_CALL,
          "NULL closest mode output"
      );
    }
  }
  return verdict();
}
