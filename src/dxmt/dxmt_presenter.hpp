#pragma once
#include "Metal.hpp"
#include "dxmt_command.hpp"
#include "rc/util_rc.hpp"
#include "util_cpu_fence.hpp"
#include "winemetal.h"
#include <atomic>

namespace dxmt {

struct DXMTPresentMetadata {
  float edr_scale;
  float max_content_luminance;
  float max_display_luminance;
};

// SC6 fork: what the presenter last applied to the screen layer and what
// macOS reported for that layer. The sc6vrr DLL reads a copy through the
// d3d11.dll export DXMT_SC6_GetPresentStatus, to show its HDR watermark only
// when every part of the presentation chain reports HDR.
struct DXMTSC6PresentStatus {
  uint32_t size;               // sizeof(DXMTSC6PresentStatus), a version check
  uint32_t colorspace;         // WMTColorSpace sent to the layer (5 = scRGB, 4 = PQ)
  uint32_t source_format;      // WMTPixelFormat of the back buffer (115 = RGBA16Float)
  uint32_t layer_pixel_format; // CAMetalLayer.pixelFormat, read back from the layer
  uint32_t colorspace_ok;      // 1 = the last MetalLayer_setColorSpace call succeeded
  uint32_t drawable_failures;  // presents where nextDrawable returned no drawable
  float edr_current;           // NSScreen maximumExtendedDynamicRangeColorComponentValue (1.0 unless the layer wants EDR)
  float edr_potential;         // NSScreen maximumPotentialExtendedDynamicRangeColorComponentValue
  uint64_t frames;             // presents prepared
  uint64_t drawables;          // presents that got a drawable
};
extern DXMTSC6PresentStatus g_sc6_present_status;

constexpr uint32_t DXMT_GAMMA_CP_COUNT = 1024;

struct DXMTGammaRamp {
  float red[DXMT_GAMMA_CP_COUNT];
  float green[DXMT_GAMMA_CP_COUNT];
  float blue[DXMT_GAMMA_CP_COUNT];
  uint64_t version;
};

class Presenter : public RcObject {
public:
  Presenter(
      WMT::Device device, WMT::MetalLayer layer, InternalCommandLibrary &lib, float scale_factor, uint8_t sample_count,
      bool display_sync = false
  );

  bool changeLayerProperties(
      WMTPixelFormat format, WMTColorSpace colorspace, double width, double height, uint8_t sample_count
  );

  bool changeLayerColorSpace(WMTColorSpace colorspace);

  void changeHDRMetadata(const WMTHDRMetadata *metadata);

  void changeGammaRamp(const DXMTGammaRamp *gamma_ramp);

  class PresentState {
  public:
    DXMTPresentMetadata metadata;
    uint64_t frame_id;
    Presenter *presenter;
    PresentState(DXMTPresentMetadata metadata, uint64_t frame_id, Presenter *presenter) :
        metadata(metadata),
        frame_id(frame_id),
        presenter(presenter) {}
    PresentState(const PresentState &copy) = delete;
    PresentState(PresentState &&move)  {
      metadata = move.metadata;
      frame_id = move.frame_id;
      presenter = move.presenter;
      move.presenter = nullptr;
    };
    ~PresentState() {
      if (presenter) {
        presenter->frame_presented_.signal(frame_id);
        presenter = nullptr;
      }
    };
  };

  PresentState synchronizeLayerProperties();

  WMT::MetalDrawable encodeCommands(
      WMT::CommandBuffer cmdbuf, WMT::Texture backbuffer, DXMTPresentMetadata metadata,
      std::function<void(WMT::RenderCommandEncoder)> &&wait_fences,
      std::function<void(WMT::RenderCommandEncoder)> &&update_fences
  );

private:
  void buildRenderPipelineState(bool is_pq, bool with_hdr_metadata, bool is_ms, bool gamma_enable);

  WMT::Device device_;
  WMT::MetalLayer layer_;
  InternalCommandLibrary &lib_;
  WMTLayerProps layer_props_;
  uint32_t sample_count_;
  WMTColorSpace colorspace_ = WMTColorSpaceSRGB;
  WMTHDRMetadata hdr_metadata_;
  bool has_hdr_metadata_ = false;
  WMTPixelFormat source_format_ = WMTPixelFormatInvalid;
  uint64_t display_setting_version_ = 0;
  WMTColorSpace display_colorspace_ = WMTColorSpaceSRGB;
  WMTHDRMetadata display_hdr_metadata_;
  WMTEDRValue display_edr_value_{0.0, 1.0};
  uint64_t gamma_version_ = 0;
  std::array<float, DXMT_GAMMA_CP_COUNT * 4> gamma_lut_rgba_;
  WMT::Reference<WMT::Texture> gamma_lut_texture_;
  WMT::Reference<WMT::RenderPipelineState> present_blit_;
  WMT::Reference<WMT::RenderPipelineState> present_scale_;
  std::atomic_flag pso_valid = 0;
  uint64_t frame_requested_ = 0;
  CpuFence frame_presented_ = 0;
};
} // namespace dxmt