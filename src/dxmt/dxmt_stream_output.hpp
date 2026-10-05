#pragma once

#include "DXBCParser/DXBCUtils.h"
#include "airconv_public.h"
#include "log/log.hpp"
#include <bit>
#include <span>
#include <strings.h>
#include <vector>

namespace dxmt {

// stream output's elements, from its declaration (D3D11's and D3D12's entries are one layout): each entry's components,
// from the output register of its stream that carries its semantic, or a gap, at the next offset of its slot's vertex.
// `pBytecode` is the stage before stream output: the geometry shader, or the vertex shader
template <typename Entry>
HRESULT
ExtractStreamOutputElements(
    const void *pBytecode, std::span<const Entry> Entries, std::vector<SM50_STREAM_OUTPUT_ELEMENT2> &Elements
) {
  microsoft::CSignatureParser5 outputs;
  if (HRESULT hr = microsoft::DXBCGetOutputSignature(pBytecode, &outputs); FAILED(hr))
    return hr;
  uint32_t offsets[4] = {};
  for (auto &entry : Entries) {
    // a gap, which has no semantic, may skip any number of components
    if (entry.OutputSlot >= std::size(offsets) ||
        (entry.SemanticName && entry.StartComponent + entry.ComponentCount > 4))
      return E_INVALIDARG;
    // the entry's components count from the output's own first one (D3D11_SO_DECLARATION_ENTRY::StartComponent:
    // "which component of the entry"), which is where its register has it: outputs share registers
    uint32_t reg = ~0u, first = 0;
    if (entry.SemanticName) {
      const microsoft::D3D11_SIGNATURE_PARAMETER *params = nullptr;
      auto signature = outputs.Signature(entry.Stream);
      auto count = signature ? signature->GetParameters(&params) : 0;
      for (auto &param : std::span(params, count))
        if (param.SemanticIndex == entry.SemanticIndex && !strcasecmp(param.SemanticName, entry.SemanticName)) {
          reg = param.Register;
          first = std::countr_zero<uint32_t>(param.Mask);
        }
      if (reg == ~0u) {
        ERR("stream output of ", entry.SemanticName, entry.SemanticIndex, " is not an output");
        return E_INVALIDARG;
      }
    }
    for (uint32_t c = 0; c < entry.ComponentCount; c++) {
      Elements.push_back({reg, first + entry.StartComponent + c, entry.Stream, entry.OutputSlot, offsets[entry.OutputSlot]});
      offsets[entry.OutputSlot] += sizeof(uint32_t);
    }
  }
  return S_OK;
}

} // namespace dxmt
