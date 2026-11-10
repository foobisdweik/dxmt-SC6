#pragma once
#include "d3d11_2.h"
#include "mullion_diag.hpp"

namespace mullion {

// a context's annotations: its events, as BeginEventInt and EndEvent take them
class D3D11UserDefinedAnnotation : public ID3DUserDefinedAnnotation {
public:
  D3D11UserDefinedAnnotation(ID3D11DeviceContext2 *container) : container_(container) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override {
    return container_->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return container_->AddRef(); }

  ULONG STDMETHODCALLTYPE Release() override { return container_->Release(); }

  // the events begun before and not ended, or -1 with no tool to read them
  INT STDMETHODCALLTYPE BeginEvent(LPCWSTR Name) override {
    if (!diag::events)
      return -1;
    container_->BeginEventInt(Name, 0);
    return open_++;
  }

  INT STDMETHODCALLTYPE EndEvent() override {
    if (!diag::events)
      return -1;
    container_->EndEvent();
    return open_ ? --open_ : 0;
  }

  // a marker is an instant, which no pass is named by
  void STDMETHODCALLTYPE SetMarker(LPCWSTR Name) override {}

  WINBOOL STDMETHODCALLTYPE GetStatus() override { return diag::events; }

private:
  ID3D11DeviceContext2 *container_;
  INT open_ = 0;
};

} // namespace mullion
