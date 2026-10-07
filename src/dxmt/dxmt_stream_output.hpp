#pragma once

#include "DXBCParser/DXBCUtils.h"
#include "airconv_public.h"
#include "log/log.hpp"
#include <algorithm>
#include <bit>
#include <span>
#include <strings.h>
#include <vector>

namespace dxmt {

// stream output's elements, from its declaration (D3D11's and D3D12's entries are one layout): each entry's components,
// from the output register of its stream that carries its semantic, or a gap, at the next offset of its slot's vertex.
// `pBytecode` is the stage before stream output: the geometry shader, or the vertex shader. `Strides` are the
// application's, for the slots it gave one. what is refused is what Windows refuses in Wine's test_stream_output
// (dlls/d3d11/tests/d3d11.c)
template <typename Entry>
HRESULT
ExtractStreamOutputElements(
    const void *pBytecode, std::span<const Entry> Entries, std::span<const UINT> Strides,
    std::vector<SM50_STREAM_OUTPUT_ELEMENT2> &Elements
) {
  microsoft::CSignatureParser5 outputs;
  if (HRESULT hr = microsoft::DXBCGetOutputSignature(pBytecode, &outputs); FAILED(hr))
    return hr;
  uint32_t offsets[4] = {};
  // the slots that take an output: one of gaps alone is refused
  bool takes[std::size(offsets)] = {};
  for (auto &entry : Entries) {
    // a gap, which has no semantic, may skip any number of components, and is nothing else
    if (entry.OutputSlot >= std::size(offsets) || entry.Stream >= D3D11_SO_STREAM_COUNT || !entry.ComponentCount ||
        (!entry.SemanticName && (entry.SemanticIndex || entry.StartComponent)))
      return E_INVALIDARG;
    // the entry's components count from the output's own first one (D3D11_SO_DECLARATION_ENTRY::StartComponent:
    // "which component of the entry"), which is where its register has it: outputs share registers
    uint32_t reg = ~0u, first = 0;
    if (entry.SemanticName) {
      const microsoft::D3D11_SIGNATURE_PARAMETER *params = nullptr;
      auto signature = outputs.Signature(entry.Stream);
      auto count = signature ? signature->GetParameters(&params) : 0;
      for (auto &param : std::span(params, count))
        if (param.SemanticIndex == entry.SemanticIndex && !strcasecmp(param.SemanticName, entry.SemanticName) &&
            entry.StartComponent + entry.ComponentCount <= uint32_t(std::popcount<uint32_t>(param.Mask))) {
          reg = param.Register;
          first = std::countr_zero<uint32_t>(param.Mask);
        }
      if (reg == ~0u) {
        ERR("stream output of ", entry.SemanticName, entry.SemanticIndex, " is not an output, or not so wide");
        return E_INVALIDARG;
      }
      takes[entry.OutputSlot] = true;
    }
    for (uint32_t c = 0; c < entry.ComponentCount; c++) {
      SM50_STREAM_OUTPUT_ELEMENT2 element{
          reg, first + entry.StartComponent + c, entry.Stream, entry.OutputSlot, offsets[entry.OutputSlot]
      };
      // a component goes out once
      if (entry.SemanticName && std::ranges::any_of(Elements, [&](auto &other) {
            return other.reg_id == reg && other.component == element.component && other.stream == element.stream;
          }))
        return E_INVALIDARG;
      Elements.push_back(element);
      offsets[entry.OutputSlot] += sizeof(uint32_t);
    }
  }
  for (uint32_t slot = 0; slot < std::size(offsets); slot++)
    if (offsets[slot] && (!takes[slot] || (slot < Strides.size() &&
                                           (Strides[slot] % sizeof(uint32_t) || Strides[slot] < offsets[slot]))))
      return E_INVALIDARG;
  return S_OK;
}

} // namespace dxmt
