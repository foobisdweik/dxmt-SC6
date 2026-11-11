#include "d3d11_pipeline_archive.hpp"
#include "d3d11_pipeline.hpp"
#include "mullion_shader_cache.hpp"
#include "config/config.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "d3d11_sc6_paths.hpp"
#include "util_string.hpp"
#include <cstring>

namespace mullion {

namespace {

/* Pipeline list file: header, then records. Each record is stored as its
   length, the SHA-1 of its bytes, and the bytes. The hash detects a damaged
   file and also identifies duplicate records. */
constexpr char kListMagic[8] = {'D', 'X', 'M', 'T', 'P', 'S', 'O', 'L'};
/* Bump when PipelineRecord or the meaning of a stored field changes. 2: Mullion's extra pipeline key fields. */
constexpr uint32_t kListFormatVersion = 2;
constexpr uint32_t kMaxRecords = 16384;
/* Write the files only after this long without a newly added pipeline, so
   that serialization does not compete with a burst of compiles. */
constexpr auto kSaveIdleTime = std::chrono::seconds(5);
constexpr auto kSaverWakeInterval = std::chrono::seconds(2);

std::atomic<bool> g_archive_owned = false;

class ByteWriter {
public:
  explicit ByteWriter(std::vector<uint8_t> &out) : out_(out) {}
  void u8(uint8_t v) { out_.push_back(v); }
  void u32(uint32_t v) { bytes(&v, sizeof(v)); }
  void bytes(const void *data, size_t size) {
    auto p = (const uint8_t *)data;
    out_.insert(out_.end(), p, p + size);
  }

private:
  std::vector<uint8_t> &out_;
};

class ByteReader {
public:
  ByteReader(const uint8_t *data, size_t size) : data_(data), size_(size) {}
  bool u8(uint8_t &v) { return bytes(&v, sizeof(v)); }
  bool u32(uint32_t &v) { return bytes(&v, sizeof(v)); }
  bool bytes(void *out, size_t size) {
    if (size_ - offset_ < size)
      return false;
    memcpy(out, data_ + offset_, size);
    offset_ += size;
    return true;
  }
  bool at_end() const { return offset_ == size_; }

private:
  const uint8_t *data_;
  size_t size_;
  size_t offset_ = 0;
};

bool
valid_blend(uint32_t v) {
  return v >= D3D11_BLEND_ZERO && v <= D3D11_BLEND_INV_SRC1_ALPHA;
}

bool
valid_blend_op(uint32_t v) {
  return v >= D3D11_BLEND_OP_ADD && v <= D3D11_BLEND_OP_MAX;
}

} // namespace

bool
PipelineRecord::FromGraphicsDesc(const MTL_GRAPHICS_PIPELINE_DESC &desc, PipelineRecord &out) {
  if (desc.SOLayout || !desc.VertexShader || !desc.BlendState)
    return false;
  if (desc.NumColorAttachments > 8)
    return false;
  out = {};
  out.kind = Graphics;
  ManagedShader shaders[NumSlots] = {
      desc.VertexShader, desc.PixelShader, desc.HullShader, desc.DomainShader, desc.GeometryShader,
  };
  for (unsigned i = 0; i < NumSlots; i++) {
    if (shaders[i]) {
      out.shader_mask |= 1 << i;
      out.shaders[i] = shaders[i]->sha1();
    }
  }
  out.num_color_attachments = desc.NumColorAttachments;
  for (unsigned i = 0; i < 8; i++)
    out.color_formats[i] = i < desc.NumColorAttachments ? desc.ColorAttachmentFormats[i] : WMTPixelFormatInvalid;
  out.depth_stencil_format = desc.DepthStencilFormat;
  out.topology_class = desc.TopologyClass;
  out.rasterization_enabled = desc.RasterizationEnabled;
  out.sample_count = desc.SampleCount;
  out.gs_strip_topology = desc.GSStripTopology;
  out.index_buffer_format = desc.IndexBufferFormat;
  out.sample_mask = desc.SampleMask;
  out.gs_passthrough = desc.GSPassthrough;
  out.centered_samples = desc.CenteredSamples;
  out.single_raster_sample = desc.SingleRasterSample;
  out.paired_samples = desc.PairedSamples;
  out.clip_distances = desc.ClipDistances;
  out.input_primitive = desc.InputPrimitive;
  out.pass_through = desc.PassThrough;
  out.quad_lines = desc.QuadLines;
  out.pipeline_statistics = desc.PipelineStatistics;
  desc.BlendState->GetDesc1(&out.blend);
  if (desc.InputLayout) {
    MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC *elements;
    uint32_t count = desc.InputLayout->input_layout_element(&elements);
    out.has_input_layout = true;
    out.input_layout.assign(elements, elements + count);
  }
  return true;
}

void
PipelineRecord::Serialize(std::vector<uint8_t> &out) const {
  ByteWriter w(out);
  w.u8(kind);
  w.u8(shader_mask);
  for (unsigned i = 0; i < NumSlots; i++) {
    if (shader_mask & (1 << i))
      w.bytes(shaders[i].data, sizeof(shaders[i].data));
  }
  if (kind != Graphics)
    return;
  w.u32(num_color_attachments);
  for (unsigned i = 0; i < 8; i++)
    w.u32(color_formats[i]);
  w.u32(depth_stencil_format);
  w.u32(topology_class);
  w.u32(rasterization_enabled);
  w.u32(sample_count);
  w.u32(gs_strip_topology);
  w.u32(index_buffer_format);
  w.u32(sample_mask);
  w.u32(gs_passthrough);
  w.u32(centered_samples);
  w.u32(single_raster_sample);
  w.u32(paired_samples);
  w.u32(clip_distances);
  w.u32(input_primitive);
  w.u32(pass_through);
  w.u32(quad_lines);
  w.u32(pipeline_statistics);
  /* field by field: the struct has padding bytes with undefined content */
  w.u32(blend.AlphaToCoverageEnable ? 1 : 0);
  w.u32(blend.IndependentBlendEnable ? 1 : 0);
  for (auto &rt : blend.RenderTarget) {
    w.u32(rt.BlendEnable ? 1 : 0);
    w.u32(rt.LogicOpEnable ? 1 : 0);
    w.u32(rt.SrcBlend);
    w.u32(rt.DestBlend);
    w.u32(rt.BlendOp);
    w.u32(rt.SrcBlendAlpha);
    w.u32(rt.DestBlendAlpha);
    w.u32(rt.BlendOpAlpha);
    w.u32(rt.LogicOp);
    w.u32(rt.RenderTargetWriteMask);
  }
  w.u32(has_input_layout ? 1 : 0);
  w.u32(input_layout.size());
  for (auto &el : input_layout) {
    w.u32(el.Index);
    w.u32(el.Slot);
    w.u32(el.Offset);
    w.u32(el.Format);
    w.u32(el.StepFunction);
    w.u32(el.InstanceStepRate);
  }
}

bool
PipelineRecord::Deserialize(const uint8_t *data, size_t size) {
  ByteReader r(data, size);
  uint8_t kind_;
  if (!r.u8(kind_) || !r.u8(shader_mask))
    return false;
  if (kind_ != Graphics && kind_ != Compute)
    return false;
  kind = Kind(kind_);
  if (shader_mask >> NumSlots)
    return false;
  for (unsigned i = 0; i < NumSlots; i++) {
    if ((shader_mask & (1 << i)) && !r.bytes(shaders[i].data, sizeof(shaders[i].data)))
      return false;
  }
  if (kind == Compute)
    return shader_mask == (1 << SlotCompute) && r.at_end();
  if (!(shader_mask & (1 << SlotVertex)))
    return false;

  bool ok = r.u32(num_color_attachments);
  for (unsigned i = 0; i < 8; i++)
    ok = ok && r.u32(color_formats[i]);
  ok = ok && r.u32(depth_stencil_format) && r.u32(topology_class) && r.u32(rasterization_enabled) &&
       r.u32(sample_count) && r.u32(gs_strip_topology) && r.u32(index_buffer_format) && r.u32(sample_mask) &&
       r.u32(gs_passthrough) && r.u32(centered_samples) && r.u32(single_raster_sample) && r.u32(paired_samples) &&
       r.u32(clip_distances) && r.u32(input_primitive) && r.u32(pass_through) && r.u32(quad_lines) &&
       r.u32(pipeline_statistics);
  if (!ok || centered_samples > 1 || single_raster_sample > 1 || paired_samples > 1 ||
      pass_through > 1 || quad_lines > 1 || pipeline_statistics > 1)
    return false;
  if (num_color_attachments > 8 || topology_class > WMTPrimitiveTopologyClassTriangle ||
      rasterization_enabled > 1 || sample_count > 32 || gs_strip_topology > 1 ||
      index_buffer_format > SM50_INDEX_BUFFER_FORMAT_UINT32)
    return false;

  uint32_t a2c, independent;
  if (!r.u32(a2c) || !r.u32(independent) || a2c > 1 || independent > 1)
    return false;
  blend = {};
  blend.AlphaToCoverageEnable = a2c;
  blend.IndependentBlendEnable = independent;
  for (auto &rt : blend.RenderTarget) {
    uint32_t v[10];
    for (auto &x : v) {
      if (!r.u32(x))
        return false;
    }
    if (v[0] > 1 || v[1] > 1 || !valid_blend(v[2]) || !valid_blend(v[3]) || !valid_blend_op(v[4]) ||
        !valid_blend(v[5]) || !valid_blend(v[6]) || !valid_blend_op(v[7]) || v[8] > D3D11_LOGIC_OP_OR_INVERTED ||
        v[9] > D3D11_COLOR_WRITE_ENABLE_ALL)
      return false;
    rt.BlendEnable = v[0];
    rt.LogicOpEnable = v[1];
    rt.SrcBlend = D3D11_BLEND(v[2]);
    rt.DestBlend = D3D11_BLEND(v[3]);
    rt.BlendOp = D3D11_BLEND_OP(v[4]);
    rt.SrcBlendAlpha = D3D11_BLEND(v[5]);
    rt.DestBlendAlpha = D3D11_BLEND(v[6]);
    rt.BlendOpAlpha = D3D11_BLEND_OP(v[7]);
    rt.LogicOp = D3D11_LOGIC_OP(v[8]);
    rt.RenderTargetWriteMask = v[9];
  }

  uint32_t has_il, count;
  if (!r.u32(has_il) || !r.u32(count) || has_il > 1 || count > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT)
    return false;
  has_input_layout = has_il;
  /* value-initialized like in AddInputLayout(), so that unused bits match the
     game's own input layout and the cache returns the same object */
  input_layout = MTL_INPUT_LAYOUT_DESC(count);
  for (auto &el : input_layout) {
    uint32_t v[6];
    for (auto &x : v) {
      if (!r.u32(x))
        return false;
    }
    if (v[1] >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT || v[4] > 1 || v[5] >> 31)
      return false;
    el.Index = v[0];
    el.Slot = v[1];
    el.Offset = v[2];
    el.Format = v[3];
    el.StepFunction = D3D11_INPUT_CLASSIFICATION(v[4]);
    el.InstanceStepRate = v[5];
  }
  return r.at_end();
}

std::unique_ptr<PipelineArchive>
PipelineArchive::Create(WMT::Device device, WMTMetalVersion metal_version) {
  if (!Config::getInstance().getOption<bool>("d3d11.pipelineArchive", false))
    return nullptr;
  // SC6 fork: the path switch pipeline-archive turns it off too (d3d11_sc6_paths.hpp)
  if (!Sc6PathEnabled(Sc6Path::PipelineArchive))
    return nullptr;
  auto dir = ShaderCacheDirectory();
  if (dir.empty()) {
    WARN("PipelineArchive: disabled, because MULLION_SHADER_CACHE=0 disables disk caches");
    return nullptr;
  }
  if (g_archive_owned.exchange(true)) {
    WARN("PipelineArchive: another device owns the archive; this device compiles without it");
    return nullptr;
  }
  return std::unique_ptr<PipelineArchive>(new PipelineArchive(
      device, dir + str::format("pso_archive_", (unsigned int)metal_version, ".bin"),
      dir + str::format("pso_list_", (unsigned int)metal_version, ".bin")
  ));
}

PipelineArchive::PipelineArchive(WMT::Device device, std::string archive_path, std::string list_path) :
    device_(device),
    archive_path_(std::move(archive_path)),
    list_path_(std::move(list_path)) {
  LoadArchive();
  LoadList();
  WARN(
      "PipelineArchive: loaded ", archive_loaded_ ? "an existing" : "an empty", " archive and ", loaded_records_,
      " recorded pipelines for warm-up"
  );
  saver_ = mullion::thread([this]() { SaverThread(); });
}

PipelineArchive::~PipelineArchive() {
  {
    std::unique_lock<mullion::mutex> lock(saver_mutex_);
    saver_stop_ = true;
  }
  saver_cond_.notify_all();
  if (saver_.joinable())
    saver_.join();
  SaveIfDirty();
  LogStats("at device teardown");
  archive_handle_ = NULL_OBJECT_HANDLE;
  archive_ = nullptr;
  g_archive_owned.store(false);
}

void
PipelineArchive::LoadArchive() {
  uint64_t size = 0;
  WMT::Reference<WMT::Error> err;
  if (WMTReadCacheFile(archive_path_.c_str(), nullptr, 0, &size) && size) {
    archive_ = device_.newBinaryArchive(archive_path_.c_str(), err);
    if (archive_) {
      archive_loaded_ = true;
    } else {
      /* for example an archive written by another macOS or GPU driver version */
      WARN(
          "PipelineArchive: cannot load ", archive_path_, ", starting an empty archive: ",
          err ? err.description().getUTF8String() : std::string("unknown error")
      );
      err = nullptr;
    }
  }
  if (!archive_) {
    archive_ = device_.newBinaryArchive(nullptr, err);
    if (!archive_) {
      WARN(
          "PipelineArchive: cannot create a Metal binary archive; only warm-up is active: ",
          err ? err.description().getUTF8String() : std::string("unknown error")
      );
    }
  }
  archive_handle_ = archive_.handle;
}

void
PipelineArchive::LoadList() {
  uint64_t size = 0;
  if (!WMTReadCacheFile(list_path_.c_str(), nullptr, 0, &size) || size < 24 || size > (256u << 20))
    return;
  std::vector<uint8_t> data(size);
  uint64_t read_size = 0;
  if (!WMTReadCacheFile(list_path_.c_str(), data.data(), data.size(), &read_size) || read_size != size)
    return;

  ByteReader r(data.data(), data.size());
  char magic[8];
  uint32_t format, airconv_version, metal_version, count;
  r.bytes(magic, sizeof(magic));
  r.u32(format);
  r.u32(airconv_version);
  r.u32(metal_version);
  r.u32(count);
  if (memcmp(magic, kListMagic, sizeof(magic)) || format != kListFormatVersion ||
      airconv_version != (uint32_t)kMullionShaderCacheVersion) {
    WARN("PipelineArchive: ignoring ", list_path_, ", because it was written by another version");
    return;
  }

  uint32_t skipped = 0;
  for (uint32_t i = 0; i < count && i < kMaxRecords; i++) {
    uint32_t length;
    Sha1Digest digest;
    if (!r.u32(length) || !r.bytes(digest.data, sizeof(digest.data)) || length > 65536)
      break;
    std::vector<uint8_t> blob(length);
    if (!r.bytes(blob.data(), length))
      break;
    PipelineRecord record;
    if (Sha1HashState::compute(blob.data(), blob.size()) != digest || !record.Deserialize(blob.data(), blob.size())) {
      skipped++;
      continue;
    }
    if (!known_records_.insert(digest).second)
      continue;
    record_blobs_.push_back(std::move(blob));

    uint32_t index = warm_records_.size();
    uint8_t missing = 0;
    for (unsigned slot = 0; slot < PipelineRecord::NumSlots; slot++) {
      if (!(record.shader_mask & (1 << slot)))
        continue;
      /* a shader in two slots counts once */
      auto &waiting = warm_waiting_[record.shaders[slot]];
      if (waiting.empty() || waiting.back() != index) {
        waiting.push_back(index);
        missing++;
      }
    }
    warm_records_.push_back(std::move(record));
    warm_missing_.push_back(missing);
  }
  loaded_records_ = warm_records_.size();
  if (skipped)
    WARN("PipelineArchive: skipped ", skipped, " damaged or unsupported records in ", list_path_);
}

WMT::Reference<WMT::RenderPipelineState>
PipelineArchive::NewRenderPipelineState(
    WMTRenderPipelineInfo &info, WMT::Reference<WMT::Error> &err, bool &add_later
) {
  add_later = false;
  if (!archive_handle_)
    return device_.newRenderPipelineState(info, err);

  std::shared_lock<std::shared_mutex> lock(archive_rw_);
  info.binary_archives_for_lookup.set(&archive_handle_);
  info.num_binary_archives_for_lookup = 1;
  info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
  if (archive_loaded_) {
    /* Fail instead of compiling on a miss. Metal does not report whether a
       lookup hit, so this is how hits are counted. */
    info.fail_on_binary_archive_miss = true;
    auto pso = device_.newRenderPipelineState(info, err);
    if (pso) {
      hits_.fetch_add(1, std::memory_order_relaxed);
      return pso;
    }
    err = nullptr;
  }
  misses_.fetch_add(1, std::memory_order_relaxed);
  info.fail_on_binary_archive_miss = false;
  auto pso = device_.newRenderPipelineState(info, err);
  add_later = pso != nullptr;
  return pso;
}

void
PipelineArchive::AddRenderPipeline(WMTRenderPipelineInfo &info) {
  if (!archive_handle_)
    return;
  {
    std::shared_lock<std::shared_mutex> lock(archive_rw_);
    info.binary_archives_for_lookup.set(&archive_handle_);
    info.num_binary_archives_for_lookup = 1;
    info.binary_archive_for_serialization = archive_handle_;
    info.fail_on_binary_archive_miss = false;
    /* winemetal adds the descriptor to the archive after it creates the
       pipeline. The second pipeline object is not needed; Metal's own cache
       makes creating it again cheap. */
    WMT::Reference<WMT::Error> err;
    auto pso = device_.newRenderPipelineState(info, err);
    if (!pso)
      return;
  }
  added_.fetch_add(1, std::memory_order_relaxed);
  archive_dirty_.store(true);
  MarkChanged();
}

WMT::Reference<WMT::ComputePipelineState>
PipelineArchive::NewComputePipelineState(
    WMTComputePipelineInfo &info, WMT::Reference<WMT::Error> &err, bool &add_later
) {
  add_later = false;
  if (!archive_handle_)
    return device_.newComputePipelineState(info, err);

  std::shared_lock<std::shared_mutex> lock(archive_rw_);
  info.binary_archives_for_lookup.set(&archive_handle_);
  info.num_binary_archives_for_lookup = 1;
  info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
  if (archive_loaded_) {
    info.fail_on_binary_archive_miss = true;
    auto pso = device_.newComputePipelineState(info, err);
    if (pso) {
      hits_.fetch_add(1, std::memory_order_relaxed);
      return pso;
    }
    err = nullptr;
  }
  misses_.fetch_add(1, std::memory_order_relaxed);
  info.fail_on_binary_archive_miss = false;
  auto pso = device_.newComputePipelineState(info, err);
  add_later = pso != nullptr;
  return pso;
}

void
PipelineArchive::AddComputePipeline(WMTComputePipelineInfo &info) {
  if (!archive_handle_)
    return;
  {
    std::shared_lock<std::shared_mutex> lock(archive_rw_);
    info.binary_archives_for_lookup.set(&archive_handle_);
    info.num_binary_archives_for_lookup = 1;
    info.binary_archive_for_serialization = archive_handle_;
    info.fail_on_binary_archive_miss = false;
    WMT::Reference<WMT::Error> err;
    auto pso = device_.newComputePipelineState(info, err);
    if (!pso)
      return;
  }
  added_.fetch_add(1, std::memory_order_relaxed);
  archive_dirty_.store(true);
  MarkChanged();
}

void
PipelineArchive::Record(const PipelineRecord &record) {
  std::vector<uint8_t> blob;
  record.Serialize(blob);
  auto digest = Sha1HashState::compute(blob.data(), blob.size());
  {
    std::lock_guard<mullion::mutex> lock(records_mutex_);
    if (record_blobs_.size() >= kMaxRecords)
      return;
    if (!known_records_.insert(digest).second)
      return;
    record_blobs_.push_back(std::move(blob));
  }
  list_dirty_.store(true);
  MarkChanged();
}

void
PipelineArchive::OnShaderCreated(const Sha1Digest &shader, std::vector<PipelineRecord> &ready) {
  std::lock_guard<mullion::mutex> lock(warm_mutex_);
  auto iter = warm_waiting_.find(shader);
  if (iter == warm_waiting_.end())
    return;
  for (auto index : iter->second) {
    if (--warm_missing_[index] == 0)
      ready.push_back(std::move(warm_records_[index]));
  }
  warm_waiting_.erase(iter);
}

void
PipelineArchive::OnWarmUpFinished(bool success) {
  warm_finished_.fetch_add(1, std::memory_order_relaxed);
  if (!success)
    warm_failed_.fetch_add(1, std::memory_order_relaxed);
}

void
PipelineArchive::MarkChanged() {
  last_change_.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

void
PipelineArchive::SaveIfDirty() {
  std::lock_guard<mullion::mutex> save_lock(save_mutex_);
  if (archive_handle_ && archive_dirty_.exchange(false)) {
    WMT::Reference<WMT::Error> err;
    {
      /* exclusive: no pipeline may be added while Metal writes the archive */
      std::unique_lock<std::shared_mutex> lock(archive_rw_);
      archive_.serialize(archive_path_.c_str(), err);
    }
    if (err) {
      WARN("PipelineArchive: cannot write ", archive_path_, ": ", err.description().getUTF8String());
    }
  }
  if (list_dirty_.exchange(false)) {
    std::vector<uint8_t> data;
    ByteWriter w(data);
    {
      std::lock_guard<mullion::mutex> lock(records_mutex_);
      w.bytes(kListMagic, sizeof(kListMagic));
      w.u32(kListFormatVersion);
      w.u32(kMullionShaderCacheVersion);
      w.u32(0); /* reserved; the file name has the Metal version */
      w.u32(record_blobs_.size());
      for (auto &blob : record_blobs_) {
        auto digest = Sha1HashState::compute(blob.data(), blob.size());
        w.u32(blob.size());
        w.bytes(digest.data, sizeof(digest.data));
        w.bytes(blob.data(), blob.size());
      }
    }
    if (!WMTWriteCacheFile(list_path_.c_str(), data.data(), data.size())) {
      WARN("PipelineArchive: cannot write ", list_path_);
    }
  }
}

void
PipelineArchive::LogStats(const char *when) {
  uint64_t records;
  {
    std::lock_guard<mullion::mutex> lock(records_mutex_);
    records = record_blobs_.size();
  }
  WARN(
      "PipelineArchive ", when, ": archive hits=", hits_.load(), " misses=", misses_.load(),
      " added=", added_.load(), "; warm-up queued=", warm_queued_.load(), " finished=", warm_finished_.load(),
      " failed=", warm_failed_.load(), "; recorded pipelines=", records
  );
}

void
PipelineArchive::SaverThread() {
  env::setThreadName("mullion-pso-archive");
  auto last_log = clock::now();
  while (true) {
    {
      std::unique_lock<mullion::mutex> lock(saver_mutex_);
      saver_cond_.wait_for(lock, kSaverWakeInterval, [this]() { return saver_stop_; });
      if (saver_stop_)
        break;
    }
    auto now = clock::now();
    auto last_change = clock::time_point(clock::duration(last_change_.load(std::memory_order_relaxed)));
    if ((archive_dirty_.load() || list_dirty_.load()) && now - last_change >= kSaveIdleTime) {
      SaveIfDirty();
    }
    uint64_t sum = hits_.load() + misses_.load() + warm_finished_.load() + warm_queued_.load();
    if (sum != logged_stats_sum_ && now - last_log >= std::chrono::seconds(10)) {
      logged_stats_sum_ = sum;
      last_log = now;
      LogStats("so far");
    }
  }
}

} // namespace mullion
