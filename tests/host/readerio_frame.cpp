#include "../../src/airconv/monad.hpp"

using IO = ReaderIO<int, int>;

static IO task(std::shared_ptr<int> value) {
  co_return *value;
}

int main() {
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
