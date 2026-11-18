#include "../../src/airconv/monad.hpp"

using IO = ReaderIO<int, int>;

static IO task(std::shared_ptr<int> value) {
  co_return *value;
}

using Owned = ReaderIO<int, std::unique_ptr<int>>;

static Owned forward_owned(Owned input) {
  auto value = co_yield std::move(input);
  co_return std::move(value);
}

int main() {
  Owned input([](int value) -> llvm::Expected<std::unique_ptr<int>> {
    return std::make_unique<int>(value);
  });
  auto result = forward_owned(std::move(input)).build(42);
  if (!result || !*result || **result != 42)
    return 4;

  Owned failing([](int) -> llvm::Expected<std::unique_ptr<int>> {
    return llvm::createStringError(std::errc::invalid_argument, "input failed");
  });
  auto error = forward_owned(std::move(failing)).build(0);
  if (error || llvm::toString(error.takeError()) != "input failed")
    return 5;

  for (int path = 0; path < 3; ++path) {
    auto value = std::make_shared<int>(7);
    std::weak_ptr<int> observer = value;
    {
      auto op = task(std::move(value));
      if (path == 0) {
        auto result = op.build(0);
        if (!result || *result != 7)
          return 1;
      } else if (path == 1) {
        IO fail([](int) -> llvm::Expected<int> { return llvm::createStringError(std::errc::invalid_argument, "failure"); });
        fail.concat(std::move(op));
        auto result = fail.build(0);
        if (result)
          return 2;
        llvm::consumeError(result.takeError());
      }
    }
    if (!observer.expired())
      return 3;
  }
}
