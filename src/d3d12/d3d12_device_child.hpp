/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once
#include "d3d12_device.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include <algorithm>
#include <limits>
#include <mutex>
#include <vector>

#if defined(__MINGW32__)

const GUID kWKPDID_D3DDebugObjectNameW = {0x4cca5fd8, 0x921f, 0x42c8, 0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41};

#endif

namespace mullion {
template <typename... Base> class MTLD3D12Object : public Base... {
public:
  MTLD3D12Object() {}

  virtual ~MTLD3D12Object() {
    for (auto &callback : callbacks_)
      callback.function(callback.data);
  }

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return FAIL(E_POINTER);
    *object = nullptr;
    if (riid != __uuidof(ID3DDestructionNotifier))
      return E_NOINTERFACE;
    *object = static_cast<ID3DDestructionNotifier *>(this);
    this->AddRef();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  RegisterDestructionCallback(PFN_DESTRUCTION_CALLBACK function, void *data, UINT *id) override {
    if (!function)
      return FAIL(DXGI_ERROR_INVALID_CALL);
    std::lock_guard<mullion::mutex> lock(callback_mutex_);
    if (id && next_callback_ == std::numeric_limits<UINT>::max())
      return FAIL(E_OUTOFMEMORY);
    UINT assigned = id ? next_callback_ + 1 : 0;
    try {
      callbacks_.push_back({function, data, assigned});
    } catch (const std::bad_alloc &) {
      return FAIL(E_OUTOFMEMORY);
    }
    if (id)
      *id = next_callback_ = assigned;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  UnregisterDestructionCallback(UINT id) override {
    if (!id)
      return DXGI_ERROR_NOT_FOUND;
    std::lock_guard<mullion::mutex> lock(callback_mutex_);
    return std::erase_if(callbacks_, [id](auto &callback) { return callback.id == id; }) ? S_OK : DXGI_ERROR_NOT_FOUND;
  }

  virtual HRESULT STDMETHODCALLTYPE
  GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) final {
    return private_data_.getData(guid, pDataSize, pData);
  }

  virtual HRESULT STDMETHODCALLTYPE
  SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) final {
    return private_data_.setData(guid, DataSize, pData);
  }

  virtual HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *pUnknown) override {
    return private_data_.setInterface(guid, pUnknown);
  }

  virtual HRESULT STDMETHODCALLTYPE
  SetName(const WCHAR *name) override {
    size_t size = 0;
    if (name)
      size = sizeof(WCHAR) * (wcslen(name) + 1);
    return SetPrivateData(kWKPDID_D3DDebugObjectNameW, size, name);
  }

private:
  ConcurrentComPrivateData private_data_;
  struct Callback { PFN_DESTRUCTION_CALLBACK function; void *data; UINT id; };
  std::vector<Callback> callbacks_;
  mullion::mutex callback_mutex_;
  UINT next_callback_ = 0;
};

template <typename... Base> class MTLD3D12DeviceChild : public MTLD3D12Object<ComObject<Base..., ID3DDestructionNotifier>> {
public:
  MTLD3D12DeviceChild(MTLD3D12Device *pDevice) : MTLD3D12Object<ComObject<Base..., ID3DDestructionNotifier>>(), device_(pDevice) {
    device_->AddRef();
  }

  virtual ~MTLD3D12DeviceChild() {
    device_->Release();
    device_ = nullptr;
  };

  virtual HRESULT STDMETHODCALLTYPE
  GetDevice(REFIID riid, void **ppDevice) final {
    return device_->QueryInterface(riid, ppDevice);
  };

protected:
  MTLD3D12Device *device_;
};

} // namespace mullion