#include "com/com_pointer.hpp"
#include "d3d11_device.hpp"
#include "d3d11_texture.hpp"
#include "mullion_buffer.hpp"
#include "mullion_dynamic.hpp"
#include "mullion_staging.hpp"
#include "mullion_texture.hpp"
#include "d3d11_resource.hpp"
#include "d3d11_enumerable.hpp"

namespace mullion {

#pragma region DynamicTexture

struct Subresource {
  Rc<Buffer> buffer;
  Rc<DynamicBuffer> dynamic;
  uint32_t bytes_per_row;
  uint32_t bytes_per_depth;
};

template <typename tag_texture>
class DynamicTexture : public TResourceBase<tag_texture, IMTLMinLODClampable> {
private:
  std::vector<Subresource> subresources_;
  float min_lod = 0.0;

  Texture *
  viewTexture(DXGI_FORMAT format) {
    if (!this->texture_->chroma)
      return this->texture_.ptr();
    if (format == DXGI_FORMAT_R8_UNORM || format == DXGI_FORMAT_R8_UINT)
      return this->texture_.ptr();
    if (format == DXGI_FORMAT_R8G8_UNORM || format == DXGI_FORMAT_R8G8_UINT)
      return this->texture_->chroma.ptr();
    return nullptr;
  }

  using SRVBase =
      TResourceViewBase<tag_shader_resource_view<DynamicTexture<tag_texture>>>;
  class TextureSRV : public SRVBase {
  public:
    TextureSRV(const TextureViewDescriptor &descriptor,
               const tag_shader_resource_view<>::DESC1 *pDesc,
               DynamicTexture *pResource, MTLD3D11Device *pDevice)
        : SRVBase(pDesc, pResource, pDevice) {
      this->texture_ = pResource->viewTexture(pDesc->Format);
      this->view_id_ = this->texture_->createView(descriptor);
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength()
      );
    }

  };

public:
  DynamicTexture(const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, MTLD3D11Device *pDevice, std::vector<Subresource> && subresources) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      subresources_(std::move(subresources)) {
    this->texture_ = std::move(u_texture);
  }
  Rc<StagingResource> staging(UINT) final { return nullptr; }
  Rc<DynamicBuffer> dynamicBuffer(UINT*, UINT*) final { return {}; }
  Rc<DynamicLinearTexture> dynamicLinearTexture(UINT*, UINT*) final { return {}; };
  Rc<DynamicBuffer> dynamicTexture(UINT subresource, UINT *bytes_per_row,
                                   UINT *bytes_per_depth) final {
    if (subresource < subresources_.size()) {
      auto &sub = subresources_[subresource];
      *bytes_per_row = sub.bytes_per_row;
      *bytes_per_depth = sub.bytes_per_depth;
      return sub.dynamic;
    }
    return {};
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc,
                           ID3D11ShaderResourceView1 **ppView) override {
    D3D11_SHADER_RESOURCE_VIEW_DESC1 finalDesc;
    if (FAILED(
            ExtractEntireResourceViewDescription(this->m_parent, &this->desc, pDesc, &finalDesc, this->dxgi_usage_))) {
      ERR("DynamicTexture: Failed to create SRV descriptor");
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    auto texture = viewTexture(finalDesc.Format);
    if (!texture)
      return E_INVALIDARG;
    auto hr = InitializeAndNormalizeViewDescriptor(
        this->m_parent, this->desc.MipLevels, arraySize, texture, finalDesc, descriptor
    );
    if (FAILED(hr)) {
      ERR("DynamicTexture: Failed to create texture SRV");
      return hr;
    }
    if (!ppView) {
      return S_FALSE;
    }
    *ppView = ref(new TextureSRV(descriptor, &finalDesc, this, this->m_parent));
    return S_OK;
  };

  void SetMinLOD(float MinLod) override { min_lod = MinLod; }

  float GetMinLOD() override { return min_lod; }
};

template <typename tag>
HRESULT CreateDynamicTextureInternal(MTLD3D11Device *pDevice,
                                    const typename tag::DESC1 *pDesc,
                                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                                    typename tag::COM_IMPL **ppTexture) {
  WMTTextureInfo info{};
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDesc, &finalDesc, &info))) {
    return E_INVALIDARG;
  }

  std::vector<Subresource> subresources;
  for (auto &sub : EnumerateSubresources(finalDesc)) {
    uint32_t bpr, bpi, buf_len;
    if (FAILED(GetLinearTextureLayout(pDevice, finalDesc, sub.MipLevel, bpr, bpi, buf_len))) {
      return E_FAIL;
    }
    Flags<BufferAllocationFlag> buffer_flags;
    buffer_flags.set(BufferAllocationFlag::CpuWriteCombined, BufferAllocationFlag::SuballocateFromOnePage);
#ifdef __i386__
    buffer_flags.set(BufferAllocationFlag::CpuPlaced);
#endif
    Subresource subresource;
    subresource.buffer = new Buffer(buf_len, pDevice->GetMTLDevice());
    subresource.buffer->rename(subresource.buffer->allocate(buffer_flags));
    subresource.dynamic = new DynamicBuffer(subresource.buffer.ptr(), buffer_flags);
    subresource.bytes_per_row = bpr;
    subresource.bytes_per_depth = bpi;

    D3D11_ASSERT(subresources.size() == sub.SubresourceId);
    if (pInitialData) {
      // FIXME: should buffer be initialized? in theory they won't be read anyway
    }
    subresources.push_back(std::move(subresource));
  }

  auto texture = Rc<Texture>(new Texture(info, pDevice->GetMTLDevice()));
  MTL_DXGI_FORMAT_DESC format;
  MTLQueryDXGIFormat(pDevice->GetMTLDevice(), finalDesc.Format, format);
  if (format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR) {
    MTLQueryDXGIFormat(pDevice->GetMTLDevice(), finalDesc.Format, format, 1);
    auto chroma_info = info;
    chroma_info.pixel_format = format.PixelFormat;
    chroma_info.width /= format.SubsampleWidth;
    chroma_info.height /= format.SubsampleHeight;
    texture->chroma = new Texture(chroma_info, pDevice->GetMTLDevice());
  }
  Flags<TextureAllocationFlag> flags;
  flags.set(TextureAllocationFlag::GpuManaged);
  flags.set(TextureAllocationFlag::ShaderReadonly);
  if (texture->chroma) {
    texture->chroma->rename(texture->chroma->allocate(flags));
    if (!texture->chroma->current()->texture())
      return E_OUTOFMEMORY;
    if (pInitialData)
      for (auto sub : EnumerateSubresources(finalDesc)) {
        auto &data = pInitialData[sub.SubresourceId];
        pDevice->GetMullionDevice().queue().initializer.initWithData(
            texture->chroma.ptr(), texture->chroma->current(), sub.ArraySlice, sub.MipLevel,
            ptr_add(data.pSysMem, std::uintptr_t(data.SysMemPitch) * texture->height()), data.SysMemPitch, 0, 0
        );
      }
  }
  if (pInitialData) {
    auto default_allocation = texture->allocate(flags);
    if (format.Flag & MTL_DXGI_FORMAT_EMULATED_RGB32) {
      for (auto sub : EnumerateSubresources(finalDesc)) {
        auto &data = pInitialData[sub.SubresourceId];
        pDevice->GetMullionDevice().queue().initializer.initWithData(
            texture.ptr(), default_allocation.ptr(), sub.ArraySlice, sub.MipLevel, data.pSysMem,
            data.SysMemPitch, data.SysMemSlicePitch, format.Flag
        );
      }
    } else {
      InitializeTextureData(pDevice, default_allocation->texture(), finalDesc, pInitialData);
    }
    texture->rename(std::move(default_allocation));
    *ppTexture =
        reinterpret_cast<typename tag::COM_IMPL *>(
          ref(new DynamicTexture<tag>(&finalDesc, std::move(texture), pDevice, std::move(subresources))));
  } else {
    texture->rename(texture->allocate(flags));
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
      ref(new DynamicTexture<tag>(&finalDesc, std::move(texture), pDevice, std::move(subresources))));
  }
  return S_OK;
}

HRESULT
CreateDynamicTexture1D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE1D_DESC *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture1D **ppTexture) {
  return CreateDynamicTextureInternal<tag_texture_1d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

HRESULT
CreateDynamicTexture2D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE2D_DESC1 *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture2D1 **ppTexture) {
  return CreateDynamicTextureInternal<tag_texture_2d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

HRESULT
CreateDynamicTexture3D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE3D_DESC1 *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture3D1 **ppTexture) {
  return CreateDynamicTextureInternal<tag_texture_3d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

#pragma endregion

} // namespace mullion