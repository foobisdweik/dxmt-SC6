#pragma once
#include "d3dcommon.h"

namespace mullion {

HRESULT CreateBlobFromMalloc(SIZE_T Size, ID3D10Blob **ppBlob);

}; // namespace mullion