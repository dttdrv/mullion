#pragma once

#include "llvm/Support/Error.h"
#include <string>
#include <string_view>
namespace mullion {

class UnsupportedFeature : public llvm::ErrorInfo<UnsupportedFeature> {
public:
  static char ID;
  std::string msg;

  UnsupportedFeature(std::string_view msg) : msg(msg) {}

  void log(llvm::raw_ostream &OS) const override { OS << msg; }

  std::error_code convertToErrorCode() const override {
    return std::make_error_code(std::errc::not_supported);
  }
};
}; // namespace mullion
