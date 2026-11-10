#include "d3d11_on12.hpp"
#include "d3d11_resource.hpp"
#include "mullion_command_queue.hpp"
#include "log/log.hpp"

namespace mullion {

D3D11On12Device::D3D11On12Device(
    IUnknown *container, MTLD3D11Device *device, ID3D12Device *d3d12, IMTLD3D11On12Source *source,
    ID3D12CommandQueue *queue
) :
    container_(container),
    device_(device),
    d3d12_(d3d12),
    source_(source),
    queue_(queue) {}

HRESULT STDMETHODCALLTYPE
D3D11On12Device::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;
  if (riid == __uuidof(ID3D11On12Device) || riid == __uuidof(ID3D11On12Device1) ||
      riid == __uuidof(ID3D11On12Device2)) {
    *ppvObject = ref(this);
    return S_OK;
  }
  return container_->QueryInterface(riid, ppvObject);
}

ULONG STDMETHODCALLTYPE
D3D11On12Device::AddRef() {
  return container_->AddRef();
}

ULONG STDMETHODCALLTYPE
D3D11On12Device::Release() {
  return container_->Release();
}

HRESULT
D3D11On12Device::ContextAfterQueue(ID3D12CommandQueue *queue) {
  auto event = device_->GetMTLDevice().newSharedEvent();
  if (!event)
    return E_OUTOFMEMORY;
  Com<ID3D12Fence> fence;
  HRESULT hr = d3d12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  if (FAILED(hr) || FAILED(hr = queue->Signal(fence.ptr(), 1)) ||
      FAILED(hr = source_->SignalEventAtFence(fence.ptr(), 1, event, 1)))
    return hr;
  return device_->GetImmediateContextPrivate()->WaitEvent(event, 1, fence.ptr());
}

HRESULT
D3D11On12Device::QueueAfterContext(ID3D12CommandQueue *queue) {
  Com<ID3D12Device> owner;
  if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
      FAILED(queue->GetDevice(IID_PPV_ARGS(&owner))) || owner != d3d12_)
    return E_INVALIDARG;
  Com<ID3D12Fence> fence;
  HRESULT hr = d3d12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  if (FAILED(hr))
    return hr;
  hr = device_->GetImmediateContextPrivate()->SignalFence(fence.ptr(), 1);
  return FAILED(hr) ? hr : queue->Wait(fence.ptr(), 1);
}

// the D3D11 bindings a D3D12 resource's flags allow, which a wrapped resource without D3D11_RESOURCE_FLAGS takes
static UINT
DefaultBindFlags(const D3D12_RESOURCE_DESC &desc) {
  UINT bind = 0;
  if (!(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
    bind |= D3D11_BIND_SHADER_RESOURCE;
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
    bind |= D3D11_BIND_RENDER_TARGET;
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
    bind |= D3D11_BIND_DEPTH_STENCIL;
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    bind |= D3D11_BIND_UNORDERED_ACCESS;
  return bind;
}

HRESULT
D3D11On12Device::ShareResourceState(D3D11ResourceCommon *resource, const D3D12_RESOURCE_DESC &desc, bool cpu_visible) {
  std::lock_guard lock(device_->mutex);
  try {
    auto found = resources_.find(resource->wrapped_resource.ptr());
    auto state = found == resources_.end() ? nullptr : found->second.lock();
    if (!state) {
      std::erase_if(resources_, [](const auto &entry) { return entry.second.expired(); });
      state = std::make_shared<WrappedResourceState>();
      state->buffer = resource->buffer();
      state->texture = resource->texture();
      if (cpu_visible) {
        const UINT mips = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? 1 : desc.MipLevels;
        const UINT arrays = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
                                    desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? 1 : desc.DepthOrArraySize;
        state->uses.resize(mips * arrays);
        if (state->texture)
          for (UINT array = 0; array < arrays; array++)
            for (UINT mip = 0; mip < mips; mip++) {
              TextureViewDescriptor view{};
              view.format = state->texture->pixelFormat();
              view.firstMiplevel = mip;
              view.firstArraySlice = array;
              state->uses[array * mips + mip].subset = ResourceSubsetState(&view, mips, arrays);
            }
      }
      resources_.insert_or_assign(resource->wrapped_resource.ptr(), state);
    }
    resource->buffer_ = state->buffer;
    resource->texture_ = state->texture;
    resource->wrapped_state = std::move(state);
    return S_OK;
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
}

HRESULT STDMETHODCALLTYPE
D3D11On12Device::CreateWrappedResource(
    IUnknown *pResource12, const D3D11_RESOURCE_FLAGS *pFlags11, D3D12_RESOURCE_STATES InState,
    D3D12_RESOURCE_STATES OutState, REFIID riid, void **ppResource11
) {
  InitReturnPtr(ppResource11);
  if (!pResource12)
    return E_INVALIDARG;
  Com<ID3D12Resource> resource12;
  if (FAILED(pResource12->QueryInterface(IID_PPV_ARGS(&resource12))))
    return E_INVALIDARG;

  MullionMetalResource info{};
  if (FAILED(source_->GetResourceObjects(resource12.ptr(), &info))) {
    ERR("CreateWrappedResource: not a resource of this device, or one without a Metal object of its own");
    return E_INVALIDARG;
  }
  WMT::Reference<WMT::Texture> metal_texture(info.texture);
  WMT::Reference<WMT::Buffer> metal_buffer(info.buffer);

  auto desc12 = resource12->GetDesc();
  D3D11_RESOURCE_FLAGS flags11 = {};
  if (pFlags11)
    flags11 = *pFlags11;
  if (!flags11.BindFlags)
    flags11.BindFlags = DefaultBindFlags(desc12);

  D3D12_HEAP_PROPERTIES heap{};
  D3D12_HEAP_FLAGS heap_flags{};
  UINT cpu_access = 0, shared = 0;
  if (SUCCEEDED(resource12->GetHeapProperties(&heap, &heap_flags))) {
    if (heap.Type != D3D12_HEAP_TYPE_CUSTOM)
      heap = d3d12_->GetCustomHeapProperties(heap.CreationNodeMask, heap.Type);
    if (heap.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK)
      cpu_access = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    else if (heap.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE)
      cpu_access = D3D11_CPU_ACCESS_WRITE;
    if (heap_flags & D3D12_HEAP_FLAG_SHARED)
      shared = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  }
  constexpr UINT sharing = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                           D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_RESTRICT_SHARED_RESOURCE;
  if ((flags11.CPUAccessFlags & ~cpu_access) || (flags11.CPUAccessFlags && (flags11.MiscFlags | shared)) ||
      (flags11.MiscFlags & sharing & ~shared) ||
      (flags11.MiscFlags & (D3D11_RESOURCE_MISC_RESTRICTED_CONTENT | D3D11_RESOURCE_MISC_RESTRICT_SHARED_RESOURCE_DRIVER |
                            D3D11_RESOURCE_MISC_GUARDED)) ||
      (flags11.StructureByteStride && desc12.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER))
    return E_INVALIDARG;
  flags11.MiscFlags |= shared;

  // D3D11's bindings must be ones the D3D12 resource allows
  UINT allowed = DefaultBindFlags(desc12);
  UINT bind_needing_flag =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS;
  if (flags11.BindFlags & bind_needing_flag & ~allowed) {
    ERR("CreateWrappedResource: bind flags ", flags11.BindFlags, " the D3D12 resource's flags ", desc12.Flags,
        " don't allow");
    return E_INVALIDARG;
  }

  HRESULT hr = E_INVALIDARG;
  Com<ID3D11Resource> resource11;
  switch (desc12.Dimension) {
  case D3D12_RESOURCE_DIMENSION_BUFFER: {
    if (!metal_buffer || desc12.Width > UINT_MAX)
      return E_INVALIDARG;
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = UINT(desc12.Width);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = flags11.BindFlags;
    desc.CPUAccessFlags = flags11.CPUAccessFlags;
    desc.MiscFlags = flags11.MiscFlags;
    desc.StructureByteStride = flags11.StructureByteStride;
    hr = WrapBuffer(device_, &desc, std::move(metal_buffer), info, IID_PPV_ARGS(&resource11));
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D: {
    if (!metal_texture)
      return E_INVALIDARG;
    D3D11_TEXTURE1D_DESC desc = {};
    desc.Width = UINT(desc12.Width);
    desc.MipLevels = desc12.MipLevels;
    desc.ArraySize = desc12.DepthOrArraySize;
    desc.Format = desc12.Format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = flags11.BindFlags;
    desc.CPUAccessFlags = flags11.CPUAccessFlags;
    desc.MiscFlags = flags11.MiscFlags;
    hr = WrapTexture(
        device_, D3D11_RESOURCE_DIMENSION_TEXTURE1D, &desc, std::move(metal_texture), info, IID_PPV_ARGS(&resource11)
    );
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE2D: {
    if (!metal_texture)
      return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC1 desc = {};
    desc.Width = UINT(desc12.Width);
    desc.Height = desc12.Height;
    desc.MipLevels = desc12.MipLevels;
    desc.ArraySize = desc12.DepthOrArraySize;
    desc.Format = desc12.Format;
    desc.SampleDesc = desc12.SampleDesc;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = flags11.BindFlags;
    desc.CPUAccessFlags = flags11.CPUAccessFlags;
    desc.MiscFlags = flags11.MiscFlags;
    desc.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;
    hr = WrapTexture(
        device_, D3D11_RESOURCE_DIMENSION_TEXTURE2D, &desc, std::move(metal_texture), info, IID_PPV_ARGS(&resource11)
    );
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D: {
    if (!metal_texture)
      return E_INVALIDARG;
    D3D11_TEXTURE3D_DESC1 desc = {};
    desc.Width = UINT(desc12.Width);
    desc.Height = desc12.Height;
    desc.Depth = desc12.DepthOrArraySize;
    desc.MipLevels = desc12.MipLevels;
    desc.Format = desc12.Format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = flags11.BindFlags;
    desc.CPUAccessFlags = flags11.CPUAccessFlags;
    desc.MiscFlags = flags11.MiscFlags;
    desc.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;
    hr = WrapTexture(
        device_, D3D11_RESOURCE_DIMENSION_TEXTURE3D, &desc, std::move(metal_texture), info, IID_PPV_ARGS(&resource11)
    );
    break;
  }
  default:
    break;
  }
  if (FAILED(hr))
    return hr;

  if (!ppResource11) {
    Com<IUnknown> validated;
    hr = resource11->QueryInterface(riid, reinterpret_cast<void **>(&validated));
    return FAILED(hr) ? hr : S_FALSE;
  }

  auto common = GetResourceCommon(resource11.ptr());
  common->wrapped_resource = resource12;
  if (FAILED(hr = ShareResourceState(common, desc12, cpu_access != 0)))
    return hr;
  // a wrapped resource is created acquired: D3D11 sees what the queue's work so far left in it
  if (FAILED(hr = ContextAfterQueue(queue_.ptr())))
    return hr;
  return resource11->QueryInterface(riid, ppResource11);
}

void STDMETHODCALLTYPE
D3D11On12Device::ReleaseWrappedResources(ID3D11Resource *const *ppResources, UINT NumResources) {
  // D3D11 has the queue's place: the queue's later work starts after D3D11's work so far
  if (FAILED(QueueAfterContext(queue_.ptr())))
    ERR("ReleaseWrappedResources: failed to order the queue after D3D11");
}

void STDMETHODCALLTYPE
D3D11On12Device::AcquireWrappedResources(ID3D11Resource *const *ppResources, UINT NumResources) {
  if (FAILED(ContextAfterQueue(queue_.ptr())))
    ERR("AcquireWrappedResources: failed to order D3D11 after the queue");
}

HRESULT STDMETHODCALLTYPE
D3D11On12Device::GetD3D12Device(REFIID riid, ID3D12Device **ppvDevice) {
  if (!ppvDevice)
    return E_POINTER;
  return d3d12_->QueryInterface(riid, reinterpret_cast<void **>(ppvDevice));
}

HRESULT STDMETHODCALLTYPE
D3D11On12Device::UnwrapUnderlyingResource(
    ID3D11Resource *pResource11, ID3D12CommandQueue *pCommandQueue, REFIID riid, void **ppvResource12
) {
  InitReturnPtr(ppvResource12);
  if (!pResource11 || !pCommandQueue || !ppvResource12)
    return E_INVALIDARG;
  Com<ID3D11Device> owner;
  pResource11->GetDevice(&owner);
  if (owner.ptr() != device_)
    return E_INVALIDARG;
  D3D11_RESOURCE_DIMENSION dimension;
  pResource11->GetType(&dimension);
  D3D12_RESOURCE_DESC desc12{};
  D3D11_USAGE usage{};
  UINT misc{};
  auto describe = [&](const auto &desc) {
    usage = desc.Usage;
    misc = desc.MiscFlags;
    desc12.Width = desc.Width;
    desc12.Height = 1;
    desc12.DepthOrArraySize = 1;
    desc12.MipLevels = desc.MipLevels;
    desc12.Format = desc.Format;
    desc12.SampleDesc = {1, 0};
    if constexpr (requires { desc.Height; })
      desc12.Height = desc.Height;
    if constexpr (requires { desc.ArraySize; })
      desc12.DepthOrArraySize = desc.ArraySize;
    if constexpr (requires { desc.Depth; })
      desc12.DepthOrArraySize = desc.Depth;
    if constexpr (requires { desc.SampleDesc; })
      desc12.SampleDesc = desc.SampleDesc;
    desc12.Flags =
        (desc.BindFlags & D3D11_BIND_RENDER_TARGET ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE) |
        (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE) |
        (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE) |
        (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL && !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
             ? D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE : D3D12_RESOURCE_FLAG_NONE);
  };
  switch (dimension) {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
    D3D11_TEXTURE1D_DESC desc;
    static_cast<ID3D11Texture1D *>(pResource11)->GetDesc(&desc);
    describe(desc);
    desc12.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    break;
  }
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
    D3D11_TEXTURE2D_DESC desc;
    static_cast<ID3D11Texture2D *>(pResource11)->GetDesc(&desc);
    describe(desc);
    desc12.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    break;
  }
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
    D3D11_TEXTURE3D_DESC desc;
    static_cast<ID3D11Texture3D *>(pResource11)->GetDesc(&desc);
    describe(desc);
    desc12.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    break;
  }
  default:
    return E_INVALIDARG;
  }
  if ((usage != D3D11_USAGE_DEFAULT && usage != D3D11_USAGE_IMMUTABLE) ||
      (misc & (D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_GDI_COMPATIBLE)))
    return E_INVALIDARG;
  auto resource12 = GetResourceCommon(pResource11)->wrapped_resource;
  if (!resource12) {
    auto texture = GetTexture(pResource11);
    if (!texture || !texture->current())
      return E_INVALIDARG;
    MullionMetalResource info{};
    info.texture = texture->current()->texture();
    info.texture_info = texture->info();
    info.texture_info.gpu_resource_id = texture->current()->gpuResourceID;
    info.flags = texture->current()->flags().raw();
    HRESULT hr = source_->CreateResourceOfMetal(&desc12, &info, IID_PPV_ARGS(&resource12));
    if (FAILED(hr))
      return hr;
    GetResourceCommon(pResource11)->wrapped_resource = resource12;
  }
  if (!GetResourceCommon(pResource11)->wrapped_state) {
    HRESULT hr = ShareResourceState(GetResourceCommon(pResource11), desc12, false);
    if (FAILED(hr))
      return hr;
  }
  HRESULT hr = QueueAfterContext(pCommandQueue);
  if (FAILED(hr))
    return hr;
  return resource12->QueryInterface(riid, ppvResource12);
}

HRESULT STDMETHODCALLTYPE
D3D11On12Device::ReturnUnderlyingResource(
    ID3D11Resource *pResource11, UINT NumSync, UINT64 *pSignalValues, ID3D12Fence **ppFences
) {
  if (!pResource11 || (NumSync && (!pSignalValues || !ppFences)))
    return E_INVALIDARG;
  Com<ID3D11Device> owner;
  pResource11->GetDevice(&owner);
  if (owner.ptr() != device_ || !GetTexture(pResource11) || !GetResourceCommon(pResource11)->wrapped_resource)
    return E_INVALIDARG;
  std::vector<std::pair<Com<ID3D12Fence>, WMT::Reference<WMT::SharedEvent>>> waits;
  for (UINT i = 0; i < NumSync; i++) {
    auto event = device_->GetMTLDevice().newSharedEvent();
    if (!event)
      return E_OUTOFMEMORY;
    HRESULT hr = source_->SignalEventAtFence(ppFences[i], pSignalValues[i], event, 1);
    if (FAILED(hr))
      return hr;
    waits.emplace_back(ppFences[i], std::move(event));
  }
  return device_->GetImmediateContextPrivate()->ReturnResource(
      pResource11, [waits = std::move(waits)](ArgumentEncodingContext &enc) mutable {
        for (auto &[fence, event] : waits)
          enc.queue().WaitSharedEvent(event, 1);
        enc.queue().OnCompletion(enc.currentSeqId(), [waits = std::move(waits)]() mutable { waits.clear(); });
      });
}

} // namespace mullion
