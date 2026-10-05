#pragma once
#include "d3dcommon.h"

namespace dxmt {

HRESULT CreateBlobFromMalloc(SIZE_T Size, ID3D10Blob **ppBlob);

}; // namespace dxmt