// d3d12.dll: Direct3D 12's loader, whose functions are D3D12Core.dll's (d3d12.def). the two are apart because they
// are apart in Windows, where applications built with the Agility SDK look for the core by its name
#include <windows.h>
#include "d3d12.h"

extern "C" HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID iid, void **object);

// each export is code of its own (loader_ here, named in d3d12.def) that calls the core's: Windows' loader exports
// functions, not forwarders, and an application that reads an export's address from the table itself (God of War
// Ragnarok) runs what it finds there
extern "C" HRESULT WINAPI
loader_D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID iid, void **device) {
  return D3D12CreateDevice(adapter, level, iid, device);
}

extern "C" HRESULT WINAPI
loader_D3D12GetDebugInterface(REFIID iid, void **debug) {
  return D3D12GetDebugInterface(iid, debug);
}

extern "C" HRESULT WINAPI
loader_D3D12CreateRootSignatureDeserializer(const void *data, SIZE_T size, REFIID iid, void **deserializer) {
  return D3D12CreateRootSignatureDeserializer(data, size, iid, deserializer);
}

extern "C" HRESULT WINAPI
loader_D3D12CreateVersionedRootSignatureDeserializer(const void *data, SIZE_T size, REFIID iid, void **deserializer) {
  return D3D12CreateVersionedRootSignatureDeserializer(data, size, iid, deserializer);
}

extern "C" HRESULT WINAPI
loader_D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC *desc, D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob **blob, ID3DBlob **error
) {
  return D3D12SerializeRootSignature(desc, version, blob, error);
}

extern "C" HRESULT WINAPI
loader_D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob, ID3DBlob **error) {
  return D3D12SerializeVersionedRootSignature(desc, blob, error);
}

extern "C" HRESULT WINAPI
loader_D3D12GetInterface(REFCLSID clsid, REFIID iid, void **object) {
  return D3D12GetInterface(clsid, iid, object);
}

extern "C" HRESULT WINAPI
loader_D3D12EnableExperimentalFeatures(UINT count, const IID *iids, void *configurations, UINT *sizes) {
  return D3D12EnableExperimentalFeatures(count, iids, configurations, sizes);
}
