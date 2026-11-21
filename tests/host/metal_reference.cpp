// Growing a vector of Metal references transfers each owned handle. Count the calls that would cross into WineMetal.
#include "../../src/winemetal/Metal.hpp"
#include <array>
#include <cstdio>
#include <utility>
#include <vector>

static unsigned retains;
static std::array<unsigned, 32> releases{};

void NSObject_retain(obj_handle_t) { ++retains; }
void NSObject_release(obj_handle_t handle) { ++releases.at(handle - 1); }

int main() {
  {
    std::vector<WMT::Reference<WMT::Buffer>> buffers;
    buffers.reserve(releases.size());
    for (size_t i = 0; i < releases.size(); ++i)
      buffers.emplace_back(obj_handle_t(i + 1));
    buffers.reserve(buffers.capacity() + 1);
    for (size_t i = 0; i < buffers.size(); ++i)
      if (buffers[i].handle != i + 1 || releases[i] || retains)
        return std::printf("failed: after growth buffer %zu holds %llu, with %u retains and %u releases of this buffer\n",
                           i, (unsigned long long)buffers[i].handle, retains, releases[i]), 1;
    buffers.front() = std::move(buffers.back());
    if (buffers.front().handle != releases.size() || buffers.back().handle != 0 ||
        releases.front() != 1 || releases.back() != 0 || retains)
      return std::printf("failed: move assignment did not transfer the last handle and release the first\n"), 1;
  }
  for (size_t i = 0; i < releases.size(); ++i)
    if (releases[i] != 1)
      return std::printf("failed: buffer %zu was released %u times\n", i, releases[i]), 1;
  std::puts("passed");
}
