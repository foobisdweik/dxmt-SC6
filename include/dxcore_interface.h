// DXCore Interface
// Copyright (C) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#include <unknwn.h>
#include <cstdint>

// declarations used here from Microsoft DirectX-Headers, include/directx/dxcore_interface.h
enum class DXCoreAdapterProperty : uint32_t { InstanceLuid = 0 };
enum class DXCoreAdapterState : uint32_t;

struct IDXCoreAdapter : public IUnknown {
  virtual bool STDMETHODCALLTYPE IsValid() = 0;
  virtual bool STDMETHODCALLTYPE IsAttributeSupported(REFGUID attributeGUID) = 0;
  virtual bool STDMETHODCALLTYPE IsPropertySupported(DXCoreAdapterProperty property) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProperty(DXCoreAdapterProperty property, size_t bufferSize,
                                                void *propertyData) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertySize(DXCoreAdapterProperty property, size_t *bufferSize) = 0;
  virtual bool STDMETHODCALLTYPE IsQueryStateSupported(DXCoreAdapterState property) = 0;
  virtual HRESULT STDMETHODCALLTYPE QueryState(DXCoreAdapterState state, size_t inputStateDetailsSize,
                                               const void *inputStateDetails, size_t outputBufferSize,
                                               void *outputBuffer) = 0;
  virtual bool STDMETHODCALLTYPE IsSetStateSupported(DXCoreAdapterState property) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetState(DXCoreAdapterState state, size_t inputStateDetailsSize,
                                             const void *inputStateDetails, size_t inputDataSize,
                                             const void *inputData) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetFactory(REFIID riid, void **ppvFactory) = 0;
};
__CRT_UUID_DECL(IDXCoreAdapter, 0xf0db4c7f, 0xfe5a, 0x42a2, 0xbd, 0x62, 0xf2, 0xa6, 0xcf, 0x6f, 0xc8, 0x3e)
