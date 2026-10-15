#pragma once

#include "Metal.hpp"
#include "d3d11_input_layout.hpp"
#include "sha1/sha1_util.hpp"
#include "thread.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct MTL_GRAPHICS_PIPELINE_DESC;

namespace dxmt {

/**
The state of one pipeline that DXMT compiled, by value: shader hashes
instead of shader objects, and descriptions instead of state objects. A
later launch rebuilds MTL_GRAPHICS_PIPELINE_DESC from it once the game has
created the same shaders again.

Only pipelines without stream output are recorded, and only ordinary
graphics pipelines (no geometry or tessellation emulation) and compute
pipelines.
 */
struct PipelineRecord {
  enum Kind : uint8_t {
    Graphics = 1,
    Compute = 2,
  };
  enum Slot : uint8_t {
    SlotVertex = 0,
    SlotPixel = 1,
    SlotHull = 2,
    SlotDomain = 3,
    SlotGeometry = 4,
    NumSlots = 5,
    SlotCompute = 0,
  };

  Kind kind = Graphics;
  /* bit N set: shaders[N] is used */
  uint8_t shader_mask = 0;
  Sha1Digest shaders[NumSlots] = {};

  /* graphics only */
  uint32_t num_color_attachments = 0;
  uint32_t color_formats[8] = {};
  uint32_t depth_stencil_format = 0;
  uint32_t topology_class = 0;
  uint32_t rasterization_enabled = 0;
  uint32_t sample_count = 0;
  uint32_t gs_strip_topology = 0;
  uint32_t index_buffer_format = 0;
  uint32_t sample_mask = 0;
  uint32_t gs_passthrough = 0;
  D3D11_BLEND_DESC1 blend = {};
  bool has_input_layout = false;
  MTL_INPUT_LAYOUT_DESC input_layout;

  /* false if the description cannot be stored (for example stream output) */
  static bool FromGraphicsDesc(const MTL_GRAPHICS_PIPELINE_DESC &desc, PipelineRecord &out);

  void Serialize(std::vector<uint8_t> &out) const;
  /* false on any malformed or out-of-range field */
  bool Deserialize(const uint8_t *data, size_t size);
};

/**
A persistent Metal pipeline archive (MTLBinaryArchive) plus a list of the
pipelines a game compiled, both stored next to DXMT's shader cache.

- Every pipeline compile looks up the archive first. On a hit, Metal loads
  the GPU binary instead of compiling it. On a miss, the pipeline is
  compiled normally, and then added to the archive off the critical path.
- Every newly compiled pipeline description is recorded. At the next launch,
  a recorded pipeline is queued for compilation (warm-up) as soon as the
  game has created all of its shaders, so it is often ready before the
  first draw that uses it.
- A background thread writes both files when no pipeline was added for a
  few seconds, and once more at device teardown. Nothing is written on the
  present path.

Only one device per process owns the archive, so two devices never write
the same files.
 */
class PipelineArchive {
public:
  static std::unique_ptr<PipelineArchive> Create(WMT::Device device, WMTMetalVersion metal_version);

  ~PipelineArchive();

  PipelineArchive(const PipelineArchive &) = delete;
  PipelineArchive &operator=(const PipelineArchive &) = delete;

  /**
  Create a pipeline with an archive lookup. `add_later` is set when the
  archive missed; the caller should call AddRenderPipeline() with the same
  info after it has published the pipeline.
   */
  WMT::Reference<WMT::RenderPipelineState>
  NewRenderPipelineState(WMTRenderPipelineInfo &info, WMT::Reference<WMT::Error> &err, bool &add_later);
  void AddRenderPipeline(WMTRenderPipelineInfo &info);

  WMT::Reference<WMT::ComputePipelineState>
  NewComputePipelineState(WMTComputePipelineInfo &info, WMT::Reference<WMT::Error> &err, bool &add_later);
  void AddComputePipeline(WMTComputePipelineInfo &info);

  void Record(const PipelineRecord &record);

  /**
  Called once for each new shader. Appends to `ready` every recorded
  pipeline whose shaders now all exist. Each record is returned once.
   */
  void OnShaderCreated(const Sha1Digest &shader, std::vector<PipelineRecord> &ready);

  void OnWarmUpQueued() { warm_queued_.fetch_add(1, std::memory_order_relaxed); }
  void OnWarmUpFinished(bool success);

private:
  PipelineArchive(WMT::Device device, std::string archive_path, std::string list_path);

  void LoadArchive();
  void LoadList();
  void SaveIfDirty();
  void LogStats(const char *when);
  void SaverThread();
  void MarkChanged();

  using clock = std::chrono::steady_clock;

  WMT::Reference<WMT::Device> device_;
  std::string archive_path_;
  std::string list_path_;

  /* Shared by pipeline compiles that read or add to the archive, exclusive
     while the archive is serialized. */
  std::shared_mutex archive_rw_;
  WMT::Reference<WMT::BinaryArchive> archive_;
  obj_handle_t archive_handle_ = NULL_OBJECT_HANDLE;
  /* false when the archive started empty: then every lookup would miss, so
     the extra fail-on-miss attempt is skipped */
  bool archive_loaded_ = false;

  dxmt::mutex records_mutex_;
  std::unordered_set<Sha1Digest> known_records_;
  std::vector<std::vector<uint8_t>> record_blobs_;

  dxmt::mutex warm_mutex_;
  std::vector<PipelineRecord> warm_records_;
  std::vector<uint8_t> warm_missing_;
  std::unordered_map<Sha1Digest, std::vector<uint32_t>> warm_waiting_;

  std::atomic<bool> archive_dirty_ = false;
  std::atomic<bool> list_dirty_ = false;
  std::atomic<int64_t> last_change_ = 0;
  dxmt::mutex save_mutex_;

  std::atomic<uint64_t> hits_ = 0;
  std::atomic<uint64_t> misses_ = 0;
  std::atomic<uint64_t> added_ = 0;
  std::atomic<uint64_t> warm_queued_ = 0;
  std::atomic<uint64_t> warm_finished_ = 0;
  std::atomic<uint64_t> warm_failed_ = 0;
  uint64_t loaded_records_ = 0;
  uint64_t logged_stats_sum_ = 0;

  dxmt::mutex saver_mutex_;
  dxmt::condition_variable saver_cond_;
  bool saver_stop_ = false;
  dxmt::thread saver_;
};

} // namespace dxmt
