#pragma once
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "mullion_presenter.hpp"

namespace mullion {

struct MTLDXGIOutput : public MTLDXGIObject<IDXGIOutput6> {
  virtual const MullionGammaRamp *STDMETHODCALLTYPE GetGammaRamp() = 0;
  virtual void STDMETHODCALLTYPE SetFullscreen(bool fullscreen) = 0;
};

} // namespace mullion
