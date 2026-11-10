#include "d3d11_pipeline_cache.hpp"
#include "airconv_public.h"
#include "d3d11_device.hpp"
#include "d3d11_shader.hpp"
#include "d3d11_pipeline.hpp"
#include "d3d11_pipeline_archive.hpp"
#include "mullion_shader_cache.hpp"
#include "mullion_stream_output.hpp"
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/d3d12tokenizedprogramformat.hpp"
#include "mullion_tasks.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"
#include "util_md5.hpp"
#include "../d3d10/d3d10_shader.hpp"
#include <optional>
#include "../d3d10/d3d10_input_layout.hpp"
#include <cstring>
#include <shared_mutex>
#include <unordered_set>

namespace mullion {

class MTLD3D11InputLayout final
    : public MTLD3D11DeviceChild<IMTLD3D11InputLayout> {
public:
  MTLD3D11InputLayout(MTLD3D11Device *device, ManagedInputLayout input_layout)
      : MTLD3D11DeviceChild<IMTLD3D11InputLayout>(device),
        input_layout(input_layout), d3d10(this) {}

  ~MTLD3D11InputLayout() {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11InputLayout) ||
        riid == __uuidof(IMTLD3D11InputLayout)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10DeviceChild) ||
        riid == __uuidof(ID3D10InputLayout)) {
      *ppvObject = ref(&d3d10);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D11InputLayout), riid)) {
      WARN("D3D311InputLayout: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  };

  ManagedInputLayout GetManagedInputLayout() override { return input_layout; }

private:
  ManagedInputLayout input_layout;
  MTLD3D10InputLayout d3d10;
};

template <> struct task_trait<ThreadpoolWork *> {
  ThreadpoolWork *run_task(ThreadpoolWork *task) {
    return task->RunThreadpoolWork();
  }
  bool get_done(ThreadpoolWork *task) { return task->GetIsDone(); }
  void set_done(ThreadpoolWork *task) { task->SetIsDone(true); }
};

class PipelineCache : public MTLD3D11PipelineCacheBase {

  class CachedSM50Shader final : public Shader {
    PipelineCache *cache;
    sm50_shader_t shader = nullptr;
    Sha1Digest sha1_;
    MTL_SHADER_REFLECTION reflection_;
    MTL_SM50_SHADER_ARGUMENT *arguments_info_buffer;
    std::unordered_map<ShaderVariant, std::unique_ptr<CompiledShader>> variants;
    // a pipeline asks for variants where it is made, and again where it is built
    mullion::mutex variants_mutex;

  public:
    CachedSM50Shader(PipelineCache *cache, sm50_shader_t shader_transferred,
                     const Sha1Digest &hash, MTL_SHADER_REFLECTION &reflection)
        : cache(cache), shader(shader_transferred), sha1_(hash),
          reflection_(reflection) {
      if (reflection_.NumConstantBuffers + reflection_.NumArguments) {
        arguments_info_buffer = (MTL_SM50_SHADER_ARGUMENT *)malloc(
            sizeof(MTL_SM50_SHADER_ARGUMENT) *
            (reflection_.NumConstantBuffers + reflection_.NumArguments));
        SM50GetArgumentsInfo(shader, arguments_info_buffer,
                             arguments_info_buffer +
                                 reflection_.NumConstantBuffers);
      } else {
        arguments_info_buffer = nullptr;
      }
    }

    ~CachedSM50Shader() {
      if (shader) {
        SM50Destroy(shader);
        if (arguments_info_buffer)
          free(arguments_info_buffer);
        shader = nullptr;
      }
    };

    CachedSM50Shader(CachedSM50Shader &&moved) = delete;
    CachedSM50Shader(const CachedSM50Shader &copy) = delete;

    virtual sm50_shader_t handle() { return shader; };
    virtual MTL_SHADER_REFLECTION &reflection() { return reflection_; }
    virtual MTL_SM50_SHADER_ARGUMENT *constant_buffers_info() {
      return arguments_info_buffer;
    };
    virtual MTL_SM50_SHADER_ARGUMENT *arguments_info() {
      return arguments_info_buffer + reflection_.NumConstantBuffers;
    };
    virtual CompiledShader *get_shader(ShaderVariant variant) {
      std::lock_guard<mullion::mutex> lock(variants_mutex);
      auto c = variants.insert({variant, nullptr});
      if (c.second) {
        c.first->second = std::visit(
            [=, this](auto var) {
              return CreateVariantShader(cache->device, this, var);
            },
            variant);
        cache->scheduler_.submit(c.first->second.get());
      }
      return c.first->second.get();
    }
    virtual const Sha1Digest &sha1() { return sha1_; };

#ifdef MULLION_DEBUG
    void *bytecode;
    size_t bytecode_length;

    virtual void dump() {
      std::fstream dump_out;
      dump_out.open("shader_dump_" + sha1_.string() + ".cso",
                    std::ios::out | std::ios::binary);
      if (dump_out) {
        dump_out.write((char *)bytecode, bytecode_length);
      }
      dump_out.close();
      WARN("shader dumped to ./shader_dump_" + sha1_.string() + ".cso");
    }
#else
    virtual void dump() {}
#endif

    virtual WMT::Reference<WMT::DispatchData> find_cached_variant(Sha1Digest &variant_digest) final {
      auto reader = cache->scache_.getReader();
      if (reader)
        return reader->get(std::make_pair(sha1_, variant_digest));
      return {};
    };
    virtual void update_cached_variant(Sha1Digest &variant_digest, WMT::DispatchData data) final {
      auto writer = cache->scache_.getWriter();
      if (writer)
        writer->set(std::make_pair(sha1_, variant_digest), data);
    }
  };

  class CachedInputLayout final : public InputLayout {
  private:
  public:
    CachedInputLayout(
        std::vector<MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC> &&attributes,
        uint32_t input_slot_mask)
        : attributes_(attributes), input_slot_mask_(input_slot_mask) {
      Sha1HashState h;
      h.update(input_slot_mask);
      h.update(attributes_.size());
      for (auto &el : attributes_) {
        h.update(el);
      }
      sha1_ = h.final();
    }

    virtual uint32_t input_slot_mask() final { return input_slot_mask_; }

    virtual uint32_t input_layout_element(
        MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC **ppElements) final {
      *ppElements = attributes_.data();
      return attributes_.size();
    }

    virtual Sha1Digest &sha1() final { return sha1_; }

    std::vector<MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC> attributes_;
    Sha1Digest sha1_;
    uint32_t input_slot_mask_;
  };

  ShaderCache& scache_;

  MTLD3D11Device *device;
  StateObjectCache<D3D11_BLEND_DESC1, IMTLD3D11BlendState> blend_states;

  std::unordered_map<MTL_INPUT_LAYOUT_DESC, std::unique_ptr<CachedInputLayout>> input_layouts;
  mullion::mutex mutex_ia_;

  StateObjectCache<MTL_STREAM_OUTPUT_DESC, IMTLD3D11StreamOutputLayout>
      so_layouts;
  mullion::mutex mutex_so_;

  std::unordered_map<Sha1Digest, std::unique_ptr<CachedSM50Shader>> shaders_;
  std::shared_mutex mutex_shares;

  std::unordered_map<MTL_GRAPHICS_PIPELINE_DESC, std::unique_ptr<MTLCompiledGraphicsPipeline>> pipelines_;
  // SC6 fork: warm-up pipelines that the game has not asked for yet
  std::unordered_set<ThreadpoolWork *> warm_pending_;
  mullion::mutex mutex_;

  std::unordered_map<MTL_GRAPHICS_PIPELINE_DESC, std::unique_ptr<MTLCompiledGeometryPipeline>> pipelines_gs_;
  mullion::mutex mutex_gs_;

  std::unordered_map<MTL_GRAPHICS_PIPELINE_DESC, std::unique_ptr<MTLCompiledTessellationMeshPipeline>> pipelines_ts_;
  mullion::mutex mutex_ts_;

  std::unordered_map<ManagedShader, std::unique_ptr<MTLCompiledComputePipeline>> pipelines_cs_;
  std::unordered_set<ThreadpoolWork *> warm_pending_cs_;
  mullion::mutex mutex_cs_;

  // SC6 fork: declared before scheduler_, so it is destroyed after the workers stop and can write its files last.
  // Null unless d3d11.pipelineArchive is on.
  std::unique_ptr<PipelineArchive> archive_;

  task_scheduler<ThreadpoolWork *> scheduler_;

  CachedSM50Shader *CreateShader(const void *pBytecode,
                                 uint32_t BytecodeLength) {
    auto sha1 = Sha1HashState::compute(pBytecode, BytecodeLength);
    {
      std::shared_lock<std::shared_mutex> lock(mutex_shares);
      auto result = shaders_.find(sha1);
      if (result != shaders_.end()) {
        return shaders_.at(sha1).get();
      }
    }
    sm50_error_t err;
    sm50_shader_t sm50;
    MTL_SHADER_REFLECTION reflection;
    if (SM50Initialize(pBytecode, BytecodeLength, &sm50, &reflection, &err)) {
      ERR("Failed to initialize shader: ", SM50GetErrorMessageString(err));
      SM50FreeError(err);
      return nullptr;
    }
    auto shader = std::make_unique<CachedSM50Shader>(this, sm50, sha1, reflection);
    CachedSM50Shader *created;
    {
      std::unique_lock<std::shared_mutex> lock(mutex_shares);
      auto result = shaders_.find(sha1);
      if (result != shaders_.end()) {
        return shaders_.at(sha1).get();
      }
#ifdef MULLION_DEBUG
      shader->bytecode = malloc(BytecodeLength);
      shader->bytecode_length = BytecodeLength;
      memcpy(shader->bytecode, pBytecode, BytecodeLength);
#endif
      created = shaders_.emplace(sha1, std::move(shader)).first->second.get();
    }
    if (archive_)
      WarmUpPipelines(sha1);
    return created;
  }

  CachedSM50Shader *
  FindShader(const Sha1Digest &sha1) {
    std::shared_lock<std::shared_mutex> lock(mutex_shares);
    auto result = shaders_.find(sha1);
    return result != shaders_.end() ? result->second.get() : nullptr;
  }

  /**
  SC6 fork: queue the recorded pipelines whose shaders all exist now that `sha1` was created. A record that no
  longer fits (a shader or state object cannot be rebuilt) is skipped. A pipeline that the game already created is
  not compiled again, because the lookup finds it.
   */
  void
  WarmUpPipelines(const Sha1Digest &sha1) {
    std::vector<PipelineRecord> ready;
    archive_->OnShaderCreated(sha1, ready);
    for (auto &record : ready) {
      if (record.kind == PipelineRecord::Compute) {
        auto cs = FindShader(record.shaders[PipelineRecord::SlotCompute]);
        if (!cs)
          continue;
        MTL_COMPUTE_PIPELINE_DESC desc{cs};
        MTLCompiledComputePipeline *pipeline;
        if (GetComputePipelineImpl(&desc, &pipeline, true))
          archive_->OnWarmUpQueued();
        continue;
      }

      MTL_GRAPHICS_PIPELINE_DESC desc{};
      ManagedShader shaders[PipelineRecord::NumSlots] = {};
      bool missing = false;
      for (unsigned slot = 0; slot < PipelineRecord::NumSlots; slot++) {
        if (record.shader_mask & (1 << slot)) {
          shaders[slot] = FindShader(record.shaders[slot]);
          missing |= !shaders[slot];
        }
      }
      if (missing)
        continue;
      desc.VertexShader = shaders[PipelineRecord::SlotVertex];
      desc.PixelShader = shaders[PipelineRecord::SlotPixel];
      desc.HullShader = shaders[PipelineRecord::SlotHull];
      desc.DomainShader = shaders[PipelineRecord::SlotDomain];
      desc.GeometryShader = shaders[PipelineRecord::SlotGeometry];

      IMTLD3D11BlendState *blend_state = nullptr;
      if (FAILED(blend_states.CreateStateObject(&record.blend, &blend_state)) || !blend_state)
        continue;
      // the state object cache keeps the object alive after this release
      desc.BlendState = blend_state;
      blend_state->Release();

      desc.InputLayout =
          record.has_input_layout ? GetOrCreateInputLayout(MTL_INPUT_LAYOUT_DESC(record.input_layout)) : nullptr;
      desc.SOLayout = nullptr;
      desc.NumColorAttachments = record.num_color_attachments;
      for (unsigned i = 0; i < 8; i++)
        desc.ColorAttachmentFormats[i] = WMTPixelFormat(record.color_formats[i]);
      desc.DepthStencilFormat = WMTPixelFormat(record.depth_stencil_format);
      desc.TopologyClass = WMTPrimitiveTopologyClass(record.topology_class);
      desc.RasterizationEnabled = record.rasterization_enabled;
      desc.SampleCount = record.sample_count;
      desc.GSStripTopology = record.gs_strip_topology;
      desc.IndexBufferFormat = SM50_INDEX_BUFFER_FORMAT(record.index_buffer_format);
      desc.SampleMask = record.sample_mask;
      desc.CenteredSamples = record.centered_samples;
      desc.SingleRasterSample = record.single_raster_sample;
      desc.PairedSamples = record.paired_samples;
      desc.ClipDistances = record.clip_distances;
      desc.GSPassthrough = record.gs_passthrough;
      desc.InputPrimitive = record.input_primitive;
      desc.PassThrough = record.pass_through;
      desc.QuadLines = record.quad_lines;
      desc.PipelineStatistics = record.pipeline_statistics;

      MTLCompiledGraphicsPipeline *pipeline;
      if (GetGraphicsPipelineImpl(&desc, &pipeline, true))
        archive_->OnWarmUpQueued();
    }
  }

  // the stage a container's code is of, which each Create call takes its own of (Wine's test_stream_output has a
  // geometry shader made of vertex bytecode refused on Windows)
  std::optional<microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE>
  Stage(const void *pBytecode, uint32_t BytecodeLength) {
    using namespace microsoft;
    CDXBCParser parser;
    // Direct3D refuses a container that is not what its hash says before it reads anything else of it
    if (FAILED(parser.ReadDXBC(pBytecode, BytecodeLength)) ||
        memcmp(parser.GetHash(), md5::hashDxbcBinary(pBytecode, BytecodeLength).data.data(), sizeof(DXBCHash)))
      return {};
    auto code = parser.FindNextMatchingBlob(DXBC_GenericShaderEx);
    if (code == DXBC_BLOB_NOT_FOUND)
      code = parser.FindNextMatchingBlob(DXBC_GenericShader);
    if (code == DXBC_BLOB_NOT_FOUND)
      return {};
    auto token = *(const uint32_t *)parser.GetBlob(code);
    auto stage = DECODE_D3D10_SB_TOKENIZED_PROGRAM_TYPE(token);
    auto major = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(token);
    auto minor = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(token);
    // Microsoft Learn, Specifying Compiler Targets: SM4.0/4.1/5.0 require FL10.0/10.1/11.0; FL9 uses level_9 code
    if (major < 4 || major > 5 || minor > (major == 4 ? 1u : 0u) ||
        (major < 5 && (stage == D3D11_SB_HULL_SHADER || stage == D3D11_SB_DOMAIN_SHADER)))
      return {};
    auto minimum = major == 5 ? D3D_FEATURE_LEVEL_11_0
                   : minor    ? D3D_FEATURE_LEVEL_10_1
                   : (stage == D3D10_SB_VERTEX_SHADER || stage == D3D10_SB_PIXEL_SHADER) &&
                           parser.FindNextMatchingBlob(DXBC_FeatureLevel9Shader) != DXBC_BLOB_NOT_FOUND
                       ? D3D_FEATURE_LEVEL_9_1
                       : D3D_FEATURE_LEVEL_10_0;
    if (device->GetFeatureLevel() < minimum)
      return {};
    return stage;
  }

  virtual HRESULT AddVertexShader(const void *pBytecode,
                                  uint32_t BytecodeLength,
                                  ID3D11VertexShader **ppShader) override {
    if (Stage(pBytecode, BytecodeLength) != microsoft::D3D10_SB_VERTEX_SHADER)
      return E_INVALIDARG;
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_INVALIDARG;
    }
    if (!ppShader)
      return S_FALSE;
    *ppShader =
        ref(new TShaderBase<ID3D11VertexShader, MTLD3D10VertexShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddPixelShader(const void *pBytecode, uint32_t BytecodeLength,
                                 ID3D11PixelShader **ppShader) override {
    if (Stage(pBytecode, BytecodeLength) != microsoft::D3D10_SB_PIXEL_SHADER)
      return E_INVALIDARG;
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    // SV_InnerCoverage needs conservative rasterization tier 3, which D3D11 here does not report
    // (ConservativeRasterizationTier): a shader the device cannot run is refused
    if (!managed_shader || managed_shader->reflection().PixelShader.InnerCoverage) {
      return E_INVALIDARG;
    }
    if (!ppShader)
      return S_FALSE;
    *ppShader = ref(new TShaderBase<ID3D11PixelShader, MTLD3D10PixelShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddHullShader(const void *pBytecode, uint32_t BytecodeLength,
                                ID3D11HullShader **ppShader) override {
    if (Stage(pBytecode, BytecodeLength) != microsoft::D3D11_SB_HULL_SHADER)
      return E_INVALIDARG;
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_INVALIDARG;
    }
    if (!ppShader)
      return S_FALSE;
    *ppShader = ref(new TShaderBase<ID3D11HullShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddDomainShader(const void *pBytecode,
                                  uint32_t BytecodeLength,
                                  ID3D11DomainShader **ppShader) override {
    if (Stage(pBytecode, BytecodeLength) != microsoft::D3D11_SB_DOMAIN_SHADER)
      return E_INVALIDARG;
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_INVALIDARG;
    }
    if (!ppShader)
      return S_FALSE;
    *ppShader =
        ref(new TShaderBase<ID3D11DomainShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddGeometryShader(const void *pBytecode,
                                    uint32_t BytecodeLength,
                                    ID3D11GeometryShader **ppShader) override {
    if (Stage(pBytecode, BytecodeLength) != microsoft::D3D10_SB_GEOMETRY_SHADER)
      return E_INVALIDARG;
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_INVALIDARG;
    }
    if (!ppShader)
      return S_FALSE;
    *ppShader =
        ref(new TShaderBase<ID3D11GeometryShader, MTLD3D10GeometryShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddComputeShader(const void *pBytecode,
                                   uint32_t BytecodeLength,
                                   ID3D11ComputeShader **ppShader) override {
    if (Stage(pBytecode, BytecodeLength) != microsoft::D3D11_SB_COMPUTE_SHADER)
      return E_INVALIDARG;
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_INVALIDARG;
    }
    if (!ppShader)
      return S_FALSE;
    *ppShader =
        ref(new TShaderBase<ID3D11ComputeShader>(device, managed_shader));
    return S_OK;
  }

  HRESULT AddInputLayout(const void *pShaderBytecodeWithInputSignature,
                         const D3D11_INPUT_ELEMENT_DESC *pInputElementDesc,
                         UINT NumElements,
                         IMTLD3D11InputLayout **ppInputLayout) override {
    std::vector<MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC> buffer(NumElements);
    uint32_t num_metal_ia_elements;
    HRESULT hr;
    if (FAILED(hr = ExtractMTLInputLayoutElements(
            device, pShaderBytecodeWithInputSignature, pInputElementDesc,
            NumElements, buffer.data(), &num_metal_ia_elements))) {
      return hr;
    }
    if (!ppInputLayout)
      return S_FALSE;
    buffer.resize(num_metal_ia_elements);
    *ppInputLayout = ref(new MTLD3D11InputLayout(device, GetOrCreateInputLayout(std::move(buffer))));
    return hr;
  }

  // the cached input layout for a set of elements (also used by the pipeline archive's warm-up)
  CachedInputLayout *
  GetOrCreateInputLayout(MTL_INPUT_LAYOUT_DESC &&buffer) {
    std::lock_guard<mullion::mutex> lock(mutex_ia_);
    if (auto iter = input_layouts.find(buffer); iter != input_layouts.end())
      return iter->second.get();
    uint32_t input_slot_mask = 0;
    for (auto &element : buffer) {
      input_slot_mask |= (1 << element.Slot);
    }
    // the map key is a copy, because the layout object takes the buffer
    MTL_INPUT_LAYOUT_DESC key = buffer;
    return input_layouts.emplace(std::move(key), std::make_unique<CachedInputLayout>(std::move(buffer), input_slot_mask))
        .first->second.get();
  }

  HRESULT
  AddStreamOutputLayout(const void *pShaderBytecode, UINT BytecodeLength, UINT NumEntries,
                        const D3D11_SO_DECLARATION_ENTRY *pEntries,
                        UINT NumStrides, const UINT *pStrides,
                        UINT RasterizedStream,
                        IMTLD3D11StreamOutputLayout **ppSOLayout) override {
    using namespace microsoft;
    MTL_STREAM_OUTPUT_DESC desc{};
    // the bytecode is the geometry shader, or the stage whose output goes out without one
    auto stage = Stage(pShaderBytecode, BytecodeLength);
    if (!stage || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_10_0)
      return E_INVALIDARG;
    if (*stage == D3D10_SB_GEOMETRY_SHADER && !(desc.GeometryShader = CreateShader(pShaderBytecode, BytecodeLength)))
      return E_INVALIDARG;
    if (HRESULT hr = ExtractStreamOutputElements(
            pShaderBytecode, std::span(pEntries, NumEntries), std::span(pStrides, NumStrides), desc.Elements
        );
        FAILED(hr))
      return hr;
    for (unsigned i = 0; i < std::min<UINT>(NumStrides, std::size(desc.Strides)); i++)
      desc.Strides[i] = pStrides[i];
    // a slot without a stride takes what its elements, each a component, take
    for (auto &element : desc.Elements)
      if (element.output_slot >= NumStrides)
        desc.Strides[element.output_slot] =
            std::max<uint32_t>(desc.Strides[element.output_slot], element.offset + sizeof(uint32_t));
    desc.RasterizedStream = RasterizedStream;
    std::lock_guard<mullion::mutex> lock(mutex_so_);
    return so_layouts.CreateStateObject(&desc, ppSOLayout);
  };

  HRESULT AddBlendState(const D3D11_BLEND_DESC1 *pBlendDesc,
                        IMTLD3D11BlendState **ppBlendState) override {
    return blend_states.CreateStateObject(pBlendDesc, ppBlendState);
  }

  void GetGraphicsPipeline(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                           MTLCompiledGraphicsPipeline **ppPipeline) override {
    if (GetGraphicsPipelineImpl(pDesc, ppPipeline, false) && archive_) {
      PipelineRecord record;
      if (PipelineRecord::FromGraphicsDesc(*pDesc, record))
        archive_->Record(record);
    }
  }

  // true if a new pipeline was created and queued
  bool GetGraphicsPipelineImpl(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                               MTLCompiledGraphicsPipeline **ppPipeline, bool WarmUp) {
    std::lock_guard<mullion::mutex> lock(mutex_);

    if (auto iter = pipelines_.find(*pDesc); iter != pipelines_.end()) {
      *ppPipeline = iter->second.get();
      // SC6 fork: the game needs a warm-up pipeline now; if it still waits in the low-priority queue, move it to the
      // normal queue
      if (!WarmUp && !warm_pending_.empty() && warm_pending_.erase(iter->second.get()) &&
          !iter->second->GetIsDone())
        scheduler_.promote(iter->second.get());
      return false;
    }
    auto [iter, inserted] =
        pipelines_.insert({*pDesc, CreateGraphicsPipeline(device, pDesc, archive_.get(), WarmUp)});
    if (!inserted) {
      D3D11_ASSERT(0 && "duplicated graphics pipeline");
    } else {
      iter->second->description = &iter->first;
      if (WarmUp) {
        warm_pending_.insert(iter->second.get());
        scheduler_.submit_low(iter->second.get());
      } else {
        scheduler_.submit(iter->second.get());
      }
    }
    *ppPipeline = iter->second.get();
    return inserted;
  }

  void GetGeometryPipeline(
      MTL_GRAPHICS_PIPELINE_DESC *pDesc,
      MTLCompiledGeometryPipeline **ppPipeline) override {
    std::lock_guard<mullion::mutex> lock(mutex_gs_);

    if (auto iter = pipelines_gs_.find(*pDesc); iter != pipelines_gs_.end()) {
      *ppPipeline = iter->second.get();
      return;
    }
    auto [iter, inserted] = pipelines_gs_.insert({*pDesc, CreateGeometryPipeline(device, pDesc)});
    if (!inserted) {
      D3D11_ASSERT(0 && "duplicated geometry pipeline");
    } else {
      iter->second->description = &iter->first;
      scheduler_.submit(iter->second.get());
    }
    *ppPipeline = iter->second.get();
  }

  void GetTessellationPipeline(MTL_GRAPHICS_PIPELINE_DESC * pDesc,
                                   MTLCompiledTessellationMeshPipeline *
                                       *ppPipeline) override {
    std::lock_guard<mullion::mutex> lock(mutex_ts_);

    if (auto iter = pipelines_ts_.find(*pDesc); iter != pipelines_ts_.end()) {
      *ppPipeline = iter->second.get();
      return;
    }
    auto [iter, inserted] = pipelines_ts_.insert({*pDesc, CreateTessellationMeshPipeline(device, pDesc)});
    if (!inserted) {
      D3D11_ASSERT(0 && "duplicated tessellation pipeline");
    } else {
      iter->second->description = &iter->first;
      scheduler_.submit(iter->second.get());
    }
    *ppPipeline = iter->second.get();
  }

  void GetComputePipeline(MTL_COMPUTE_PIPELINE_DESC *pDesc,
                                  MTLCompiledComputePipeline **ppPipeline) override {
    if (GetComputePipelineImpl(pDesc, ppPipeline, false) && archive_) {
      PipelineRecord record;
      record.kind = PipelineRecord::Compute;
      record.shader_mask = 1 << PipelineRecord::SlotCompute;
      record.shaders[PipelineRecord::SlotCompute] = pDesc->ComputeShader->sha1();
      archive_->Record(record);
    }
  }

  bool GetComputePipelineImpl(MTL_COMPUTE_PIPELINE_DESC *pDesc,
                              MTLCompiledComputePipeline **ppPipeline, bool WarmUp) {
    std::lock_guard<mullion::mutex> lock(mutex_cs_);

    if (auto iter = pipelines_cs_.find(pDesc->ComputeShader); iter != pipelines_cs_.end()) {
      *ppPipeline = iter->second.get();
      if (!WarmUp && !warm_pending_cs_.empty() && warm_pending_cs_.erase(iter->second.get()) &&
          !iter->second->GetIsDone())
        scheduler_.promote(iter->second.get());
      return false;
    }
    auto [iter, inserted] = pipelines_cs_.insert(
        {pDesc->ComputeShader, CreateComputePipeline(device, pDesc->ComputeShader, archive_.get(), WarmUp)});
    if (!inserted) {
      D3D11_ASSERT(0 && "duplicated compute pipeline");
    } else if (WarmUp) {
      warm_pending_cs_.insert(iter->second.get());
      scheduler_.submit_low(iter->second.get());
    } else {
      scheduler_.submit(iter->second.get());
    }
    *ppPipeline = iter->second.get();
    return inserted;
  }

public:
  PipelineCache(MTLD3D11Device *pDevice) :
      scache_(ShaderCache::getInstance(pDevice->GetMullionDevice().metalVersion())),
      device(pDevice),
      archive_(PipelineArchive::Create(pDevice->GetMTLDevice(), pDevice->GetMullionDevice().metalVersion())),
      blend_states(pDevice),
      so_layouts(pDevice) {};
};

std::unique_ptr<MTLD3D11PipelineCacheBase>
InitializePipelineCache(MTLD3D11Device *device) {
  return std::make_unique<PipelineCache>(device);
}

}; // namespace mullion