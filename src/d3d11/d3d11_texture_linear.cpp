#include "d3d11_private.h"
#include "com/com_pointer.hpp"
#include "d3d11_device.hpp"
#include "mullion_dynamic.hpp"
#include "mullion_format.hpp"
#include "mullion_staging.hpp"
#include "mullion_texture.hpp"
#include "d3d11_resource.hpp"

namespace mullion {

#pragma region DynamicLinearTexture

template <typename tag_texture>
class TDynamicLinearTexture : public TResourceBase<tag_texture> {
private:
  Rc<DynamicLinearTexture> dynamic_;
  size_t bytes_per_image_;
  size_t bytes_per_row_;

  using SRVBase = TResourceViewBase<tag_shader_resource_view<TDynamicLinearTexture>>;

  class SRV : public SRVBase {
  public:
    SRV(const tag_shader_resource_view<>::DESC1 *pDesc, TDynamicLinearTexture *pResource, MTLD3D11Device *pDevice,
        const TextureViewDescriptor &descriptor) :
        SRVBase(pDesc, pResource, pDevice) {
      this->texture_ = pResource->texture_.ptr();
      this->view_id_ = this->texture_->createView(descriptor);
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength()
      );
    }

    ~SRV() {}
  };

public:
TDynamicLinearTexture(
      const tag_texture::DESC1 *pDesc, const WMTTextureInfo &descriptor,
      const D3D11_SUBRESOURCE_DATA *pInitialData, UINT bytes_per_image, UINT bytes_per_row, MTLD3D11Device *device
  ) :
      TResourceBase<tag_texture>(*pDesc, device),
      bytes_per_image_(bytes_per_image),
      bytes_per_row_(bytes_per_row) {
    this->texture_ = new Texture(bytes_per_image, bytes_per_row, descriptor, device->GetMTLDevice());
    Flags<TextureAllocationFlag> flags;
    flags.set(TextureAllocationFlag::ShaderReadonly);
    if (!this->m_parent->IsTraced() && pDesc->Usage == D3D11_USAGE_DYNAMIC)
      flags.set(TextureAllocationFlag::CpuWriteCombined);
    // if (pDesc->Usage != D3D11_USAGE_DEFAULT)
    if (pDesc->Usage == D3D11_USAGE_IMMUTABLE)
      flags.set(TextureAllocationFlag::GpuReadonly);
    if (pDesc->Usage != D3D11_USAGE_DYNAMIC)
      flags.set(TextureAllocationFlag::GpuManaged);
    auto allocation = this->texture_->allocate(flags);
    auto _ = this->texture_->rename(Rc(allocation));
    D3D11_ASSERT(_.ptr() == nullptr);

    if (pInitialData) {
      const UINT row_bytes = this->texture_->width() * MTLGetTexelSize(descriptor.pixel_format);
      for (unsigned row = 0; row < this->texture_->height(); row++) {
        memcpy(
            ptr_add(allocation->mappedMemory, row * bytes_per_row_),
            ptr_add(pInitialData->pSysMem, row * pInitialData->SysMemPitch), row_bytes);
      }
    }
    dynamic_ = new DynamicLinearTexture(this->texture_.ptr(), flags);
  }
  Rc<StagingResource> staging(UINT) final { return nullptr; }
  Rc<DynamicBuffer> dynamicBuffer(UINT*, UINT*) final { return {}; };
  Rc<DynamicLinearTexture> dynamicLinearTexture(UINT* pBytesPerRow, UINT* pBytesPerImage) final {
    *pBytesPerRow = bytes_per_row_;
    *pBytesPerImage = bytes_per_image_;
    return dynamic_; 
  };
  Rc<DynamicBuffer> dynamicTexture(UINT , UINT *, UINT *) final { return {}; };

  HRESULT
  STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc, ID3D11ShaderResourceView1 **ppView) override;
};

template<>
HRESULT STDMETHODCALLTYPE TDynamicLinearTexture<tag_texture_2d>::CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc, ID3D11ShaderResourceView1 **ppView){
  D3D11_SHADER_RESOURCE_VIEW_DESC1 finalDesc;
  if (FAILED(ExtractEntireResourceViewDescription(this->m_parent, &this->desc, pDesc, &finalDesc, this->dxgi_usage_))) {
    return E_INVALIDARG;
  }
  TextureViewDescriptor descriptor;
  auto hr = InitializeAndNormalizeViewDescriptor(this->m_parent, this->desc.MipLevels, this->desc.ArraySize,
                                                this->texture_.ptr(), finalDesc, descriptor);
  if (FAILED(hr))
    return hr;
  if (!ppView) {
    return S_FALSE;
  }
  auto srv = ref(new SRV(&finalDesc, this, this->m_parent, descriptor));

  *ppView = srv;
  return S_OK;
};

template<>
HRESULT STDMETHODCALLTYPE TDynamicLinearTexture<tag_texture_1d>::CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc, ID3D11ShaderResourceView1 **ppView){
  D3D11_SHADER_RESOURCE_VIEW_DESC1 finalDesc;
  if (FAILED(ExtractEntireResourceViewDescription(this->m_parent, &this->desc, pDesc, &finalDesc, this->dxgi_usage_))) {
    return E_INVALIDARG;
  }
  TextureViewDescriptor descriptor;
  auto hr = InitializeAndNormalizeViewDescriptor(this->m_parent, this->desc.MipLevels, this->desc.ArraySize,
                                                this->texture_.ptr(), finalDesc, descriptor);
  if (FAILED(hr))
    return hr;
  if (!ppView) {
    return S_FALSE;
  }
  auto srv = ref(new SRV(&finalDesc, this, this->m_parent, descriptor));

  *ppView = srv;
  return S_OK;
};

#pragma endregion

HRESULT
CreateDynamicLinearTexture2D(
    MTLD3D11Device *pDevice, const D3D11_TEXTURE2D_DESC1 *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Texture2D1 **ppTexture
) {
  if (pDesc->SampleDesc.Count > 1)
    return E_FAIL;
  if (pDesc->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
    return E_FAIL;
  if (pDesc->ArraySize > 1)
    return E_FAIL;
  MTL_DXGI_FORMAT_DESC format;
  MTLQueryDXGIFormat(pDevice->GetMTLDevice(), pDesc->Format, format);
  if (format.Flag & (MTL_DXGI_FORMAT_BC | MTL_DXGI_FORMAT_VIDEO_PLANAR | MTL_DXGI_FORMAT_EMULATED_RGB32)) {
    return E_FAIL;
  }
  if (format.Flag & MTL_DXGI_FORMAT_EMULATED_LINEAR_DEPTH_STENCIL) {
    ERR("CreateDynamicLinearTexture: depth-stencil resource cannot be dynamic");
    return E_FAIL;
  }
  if (format.PixelFormat == WMTPixelFormatInvalid) {
    return E_FAIL;
  }
  WMTTextureInfo info{};
  D3D11_TEXTURE2D_DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDesc, &finalDesc, &info))) {
    return E_INVALIDARG;
  }
  if (info.mipmap_level_count > 1)
    return E_FAIL;
  uint32_t bytesPerRow, bytesPerImage, bufferLen;
  if (FAILED(GetLinearTextureLayout(pDevice, finalDesc, 0, bytesPerRow, bytesPerImage, bufferLen))) {
    return E_FAIL;
  }

  *ppTexture = reinterpret_cast<ID3D11Texture2D1 *>(
      ref(new TDynamicLinearTexture<tag_texture_2d>(&finalDesc, info, pInitialData, bufferLen, bytesPerRow, pDevice))
  );
  return S_OK;
}

HRESULT
CreateDynamicLinearTexture1D(
    MTLD3D11Device *pDevice, const D3D11_TEXTURE1D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Texture1D **ppTexture
) {
  if (pDesc->ArraySize > 1)
    return E_FAIL;
  MTL_DXGI_FORMAT_DESC format;
  MTLQueryDXGIFormat(pDevice->GetMTLDevice(), pDesc->Format, format);
  if (format.Flag & (MTL_DXGI_FORMAT_BC | MTL_DXGI_FORMAT_EMULATED_RGB32)) {
    return E_FAIL;
  }
  if (format.Flag & MTL_DXGI_FORMAT_EMULATED_LINEAR_DEPTH_STENCIL) {
    ERR("CreateDynamicLinearTexture: depth-stencil resource cannot be dynamic");
    return E_FAIL;
  }
  if (format.PixelFormat == WMTPixelFormatInvalid) {
    return E_FAIL;
  }
  WMTTextureInfo info{};
  D3D11_TEXTURE1D_DESC finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDesc, &finalDesc, &info))) {
    return E_INVALIDARG;
  }
  if (info.mipmap_level_count > 1)
    return E_FAIL;
  uint32_t bytesPerRow, bytesPerImage, bufferLen;
  if (FAILED(GetLinearTextureLayout(pDevice, finalDesc, 0, bytesPerRow, bytesPerImage, bufferLen))) {
    return E_FAIL;
  }

  *ppTexture = reinterpret_cast<ID3D11Texture1D *>(
      ref(new TDynamicLinearTexture<tag_texture_1d>(&finalDesc, info, pInitialData, bufferLen, bytesPerRow, pDevice))
  );
  return S_OK;
}

} // namespace mullion
