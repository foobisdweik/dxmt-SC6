#pragma once

#include "d3d11_4.h"
#include "log/log.hpp"

namespace mullion {

// the immediate context's ID3D11VideoContext (Wine's test_immediate_context: an immediate context has one). it is a
// device child of the context's: its device, private data and COM identity are the context's. Mullion decodes and
// processes no video, so the video methods do nothing, and those that answer say E_NOTIMPL.
class D3D11VideoContext : public ID3D11VideoContext {
public:
  explicit D3D11VideoContext(ID3D11DeviceContext *context) : context_(context) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    return context_->QueryInterface(riid, ppvObject);
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return context_->AddRef(); }
  ULONG STDMETHODCALLTYPE Release() final { return context_->Release(); }

  void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) final { context_->GetDevice(ppDevice); }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) final {
    return context_->GetPrivateData(guid, pDataSize, pData);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) final {
    return context_->SetPrivateData(guid, DataSize, pData);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) final {
    return context_->SetPrivateDataInterface(guid, pData);
  }

  HRESULT STDMETHODCALLTYPE
  GetDecoderBuffer(ID3D11VideoDecoder *decoder, D3D11_VIDEO_DECODER_BUFFER_TYPE type, UINT *buffer_size, void **buffer) final {
    WARN("D3D11VideoContext::GetDecoderBuffer: video is not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  ReleaseDecoderBuffer(ID3D11VideoDecoder *decoder, D3D11_VIDEO_DECODER_BUFFER_TYPE type) final {
    WARN("D3D11VideoContext::ReleaseDecoderBuffer: video is not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  DecoderBeginFrame(ID3D11VideoDecoder *decoder, ID3D11VideoDecoderOutputView *view, UINT key_size, const void *key) final {
    WARN("D3D11VideoContext::DecoderBeginFrame: video is not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  DecoderEndFrame(ID3D11VideoDecoder *decoder) final {
    WARN("D3D11VideoContext::DecoderEndFrame: video is not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  SubmitDecoderBuffers(ID3D11VideoDecoder *decoder, UINT buffers_count, const D3D11_VIDEO_DECODER_BUFFER_DESC *buffer_desc) final {
    WARN("D3D11VideoContext::SubmitDecoderBuffers: video is not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  DecoderExtension(ID3D11VideoDecoder *decoder, const D3D11_VIDEO_DECODER_EXTENSION *extension) final {
    WARN("D3D11VideoContext::DecoderExtension: video is not implemented");
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetOutputTargetRect(ID3D11VideoProcessor *processor, WINBOOL enable, const RECT *rect) final {
    WARN("D3D11VideoContext::VideoProcessorSetOutputTargetRect: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetOutputBackgroundColor(ID3D11VideoProcessor *processor, WINBOOL y_cb_cr, const D3D11_VIDEO_COLOR *color) final {
    WARN("D3D11VideoContext::VideoProcessorSetOutputBackgroundColor: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetOutputColorSpace(ID3D11VideoProcessor *processor, const D3D11_VIDEO_PROCESSOR_COLOR_SPACE *color_space) final {
    WARN("D3D11VideoContext::VideoProcessorSetOutputColorSpace: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetOutputAlphaFillMode(ID3D11VideoProcessor *processor, D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE alpha_fill_mode, UINT stream_idx) final {
    WARN("D3D11VideoContext::VideoProcessorSetOutputAlphaFillMode: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetOutputConstriction(ID3D11VideoProcessor *processor, WINBOOL enable, SIZE size) final {
    WARN("D3D11VideoContext::VideoProcessorSetOutputConstriction: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetOutputStereoMode(ID3D11VideoProcessor *processor, WINBOOL enable) final {
    WARN("D3D11VideoContext::VideoProcessorSetOutputStereoMode: video is not implemented");
  }

  HRESULT STDMETHODCALLTYPE
  VideoProcessorSetOutputExtension(ID3D11VideoProcessor *processor, const GUID *guid, UINT data_size, void *data) final {
    WARN("D3D11VideoContext::VideoProcessorSetOutputExtension: video is not implemented");
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetOutputTargetRect(ID3D11VideoProcessor *processor, WINBOOL *enabled, RECT *rect) final {
    WARN("D3D11VideoContext::VideoProcessorGetOutputTargetRect: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetOutputBackgroundColor(ID3D11VideoProcessor *processor, WINBOOL *y_cb_cr, D3D11_VIDEO_COLOR *color) final {
    WARN("D3D11VideoContext::VideoProcessorGetOutputBackgroundColor: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetOutputColorSpace(ID3D11VideoProcessor *processor, D3D11_VIDEO_PROCESSOR_COLOR_SPACE *color_space) final {
    WARN("D3D11VideoContext::VideoProcessorGetOutputColorSpace: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetOutputAlphaFillMode(ID3D11VideoProcessor *processor, D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE *alpha_fill_mode, UINT *stream_idx) final {
    WARN("D3D11VideoContext::VideoProcessorGetOutputAlphaFillMode: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetOutputConstriction(ID3D11VideoProcessor *processor, WINBOOL *enabled, SIZE *size) final {
    WARN("D3D11VideoContext::VideoProcessorGetOutputConstriction: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetOutputStereoMode(ID3D11VideoProcessor *processor, WINBOOL *enabled) final {
    WARN("D3D11VideoContext::VideoProcessorGetOutputStereoMode: video is not implemented");
  }

  HRESULT STDMETHODCALLTYPE
  VideoProcessorGetOutputExtension(ID3D11VideoProcessor *processor, const GUID *guid, UINT data_size, void *data) final {
    WARN("D3D11VideoContext::VideoProcessorGetOutputExtension: video is not implemented");
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetStreamFrameFormat(ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_FRAME_FORMAT format) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamFrameFormat: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetStreamColorSpace(ID3D11VideoProcessor *processor, UINT stream_idx, const D3D11_VIDEO_PROCESSOR_COLOR_SPACE *color_space) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamColorSpace: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetStreamOutputRate(ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE rate, WINBOOL repeat, const DXGI_RATIONAL *custom_rate) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamOutputRate: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetStreamSourceRect(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, const RECT *rect) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamSourceRect: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetStreamDestRect(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, const RECT *rect) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamDestRect: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetStreamAlpha(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, float alpha) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamAlpha: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetStreamPalette(ID3D11VideoProcessor *processor, UINT stream_idx, UINT entry_count, const UINT *entries) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamPalette: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetStreamPixelAspectRatio(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, const DXGI_RATIONAL *src_aspect_ratio, const DXGI_RATIONAL *dst_aspect_ratio) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamPixelAspectRatio: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetStreamLumaKey(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, float lower, float upper) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamLumaKey: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetStreamStereoFormat(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, D3D11_VIDEO_PROCESSOR_STEREO_FORMAT format, WINBOOL left_view_frame0, WINBOOL base_view_frame0, D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE flip_mode, int mono_offset) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamStereoFormat: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetStreamAutoProcessingMode(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamAutoProcessingMode: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetStreamFilter(ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_PROCESSOR_FILTER filter, WINBOOL enable, int level) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamFilter: video is not implemented");
  }

  HRESULT STDMETHODCALLTYPE
  VideoProcessorSetStreamExtension(ID3D11VideoProcessor *processor, UINT stream_idx, const GUID *guid, UINT data_size, void *data) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamExtension: video is not implemented");
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetStreamFrameFormat(ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_FRAME_FORMAT *format) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamFrameFormat: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetStreamColorSpace(ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_PROCESSOR_COLOR_SPACE *color_space) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamColorSpace: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetStreamOutputRate(ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE *rate, WINBOOL *repeat, DXGI_RATIONAL *custom_rate) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamOutputRate: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetStreamSourceRect(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled, RECT *rect) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamSourceRect: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetStreamDestRect(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled, RECT *rect) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamDestRect: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetStreamAlpha(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled, float *alpha) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamAlpha: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetStreamPalette(ID3D11VideoProcessor *processor, UINT stream_idx, UINT entry_count, UINT *entries) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamPalette: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetStreamPixelAspectRatio(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled, DXGI_RATIONAL *src_aspect_ratio, DXGI_RATIONAL *dst_aspect_ratio) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamPixelAspectRatio: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetStreamLumaKey(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled, float *lower, float *upper) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamLumaKey: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetStreamStereoFormat(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled, D3D11_VIDEO_PROCESSOR_STEREO_FORMAT *format, WINBOOL *left_view_frame0, WINBOOL *base_view_frame0, D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE *flip_mode, int *mono_offset) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamStereoFormat: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetStreamAutoProcessingMode(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamAutoProcessingMode: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetStreamFilter(ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_PROCESSOR_FILTER filter, WINBOOL *enabled, int *level) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamFilter: video is not implemented");
  }

  HRESULT STDMETHODCALLTYPE
  VideoProcessorGetStreamExtension(ID3D11VideoProcessor *processor, UINT stream_idx, const GUID *guid, UINT data_size, void *data) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamExtension: video is not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  VideoProcessorBlt(ID3D11VideoProcessor *processor, ID3D11VideoProcessorOutputView *view, UINT frame_idx, UINT stream_count, const D3D11_VIDEO_PROCESSOR_STREAM *streams) final {
    WARN("D3D11VideoContext::VideoProcessorBlt: video is not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  NegotiateCryptoSessionKeyExchange(ID3D11CryptoSession *session, UINT data_size, void *data) final {
    WARN("D3D11VideoContext::NegotiateCryptoSessionKeyExchange: video is not implemented");
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE
  EncryptionBlt(ID3D11CryptoSession *session, ID3D11Texture2D *src_surface, ID3D11Texture2D *dst_surface, UINT iv_size, void *iv) final {
    WARN("D3D11VideoContext::EncryptionBlt: video is not implemented");
  }

  void STDMETHODCALLTYPE
  DecryptionBlt(ID3D11CryptoSession *session, ID3D11Texture2D *src_surface, ID3D11Texture2D *dst_surface, D3D11_ENCRYPTED_BLOCK_INFO *block_info, UINT key_size, const void *key, UINT iv_size, void *iv) final {
    WARN("D3D11VideoContext::DecryptionBlt: video is not implemented");
  }

  void STDMETHODCALLTYPE
  StartSessionKeyRefresh(ID3D11CryptoSession *session, UINT random_number_size, void *random_number) final {
    WARN("D3D11VideoContext::StartSessionKeyRefresh: video is not implemented");
  }

  void STDMETHODCALLTYPE
  FinishSessionKeyRefresh(ID3D11CryptoSession *session) final {
    WARN("D3D11VideoContext::FinishSessionKeyRefresh: video is not implemented");
  }

  HRESULT STDMETHODCALLTYPE
  GetEncryptionBltKey(ID3D11CryptoSession *session, UINT key_size, void *key) final {
    WARN("D3D11VideoContext::GetEncryptionBltKey: video is not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  NegotiateAuthenticatedChannelKeyExchange(ID3D11AuthenticatedChannel *channel, UINT data_size, void *data) final {
    WARN("D3D11VideoContext::NegotiateAuthenticatedChannelKeyExchange: video is not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  QueryAuthenticatedChannel(ID3D11AuthenticatedChannel *channel, UINT input_size, const void *input, UINT output_size, void *output) final {
    WARN("D3D11VideoContext::QueryAuthenticatedChannel: video is not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  ConfigureAuthenticatedChannel(ID3D11AuthenticatedChannel *channel, UINT input_size, const void *input, D3D11_AUTHENTICATED_CONFIGURE_OUTPUT *output) final {
    WARN("D3D11VideoContext::ConfigureAuthenticatedChannel: video is not implemented");
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE
  VideoProcessorSetStreamRotation(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, D3D11_VIDEO_PROCESSOR_ROTATION rotation) final {
    WARN("D3D11VideoContext::VideoProcessorSetStreamRotation: video is not implemented");
  }

  void STDMETHODCALLTYPE
  VideoProcessorGetStreamRotation(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enable, D3D11_VIDEO_PROCESSOR_ROTATION *rotation) final {
    WARN("D3D11VideoContext::VideoProcessorGetStreamRotation: video is not implemented");
  }

private:
  ID3D11DeviceContext *context_;
};

} // namespace mullion
