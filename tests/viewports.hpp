// shared by d3d11_viewports and d3d12_viewports: the cases and what they must draw.
// contract: viewports and scissors are each set all at once, and what a call does not set is not there: a viewport of
// no size, and an empty scissor (D3D11.3 15.6, 15.7: "the default Scissor Rectangle is an empty Scissor Rectangle").
// a primitive is drawn through the viewport and the scissor its SV_ViewportArrayIndex names, the first ones without
// a geometry shader (15.8.1), so through neither when one of the two is not set. this holds from draw to draw
// in one pass: a call that sets one kind leaves the other kind as it was set.
// each case is draws of a triangle over the whole clip space, each with a value of its own: a pixel holds the value
// of the last draw whose viewport and scissor both have it, or nothing.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace viewports {

static const char hlsl[] = R"hlsl(
cbuffer Draw : register(b0) { uint index; float value; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
struct G { float4 position : SV_Position; uint viewport : SV_ViewportArrayIndex; };
[maxvertexcount(3)]
void gs(triangle float4 corners[3] : SV_Position, inout TriangleStream<G> stream) {
  for (int i = 0; i < 3; i++) {
    G g;
    g.position = corners[i];
    g.viewport = index;
    stream.Append(g);
  }
}
float ps() : SV_Target { return value; }
)hlsl";

const uint32_t width = 8, height = 4;

// parts of the target, as left, top, right, bottom
enum Part { Whole, Left, Right, Lower, Nothing };
struct Rectangle {
  int left, top, right, bottom;
};
inline Rectangle
rectangle(Part part) {
  const int w = width, h = height;
  const Rectangle parts[] = {{0, 0, w, h}, {0, 0, w / 2, h}, {w / 2, 0, w, h}, {0, h / 2, w, h}, {0, 0, 0, 0}};
  return parts[part];
}

// a draw: the viewports and scissors set before it, if any are, and the viewport its primitive names. `plain`
// draws without a geometry shader
const uint32_t plain = ~0u;
struct Draw {
  std::optional<std::vector<Part>> viewports, scissors;
  uint32_t index;
};
using Case = std::vector<Draw>;

inline std::vector<Case>
cases() {
  using Parts = std::vector<Part>;
  return {
      // one of each, without and with the geometry shader
      {{Parts{Whole}, Parts{Whole}, plain}},
      {{Parts{Whole}, Parts{Whole}, 0}},
      {{Parts{Right}, Parts{Lower}, plain}},
      // none of one kind
      {{Parts{}, Parts{Whole}, plain}},
      {{Parts{Whole}, Parts{}, plain}},
      {{Parts{}, Parts{}, 0}},
      // the second of each
      {{Parts{Whole, Right}, Parts{Whole, Whole}, 1}},
      {{Parts{Whole, Whole}, Parts{Whole, Left}, 1}},
      {{Parts{Left, Right}, Parts{Nothing, Lower}, 1}},
      {{Parts{Left, Right}, Parts{Nothing, Lower}, 0}},
      // a second that only one kind has
      {{Parts{Whole, Whole}, Parts{Whole}, 1}},
      {{Parts{Whole}, Parts{Whole, Whole}, 1}},
      // more viewports than before, with the scissors that were set for them earlier: an empty one, a part
      {{Parts{Whole}, Parts{Whole, Nothing}, 0}, {Parts{Whole, Whole}, std::nullopt, 1}},
      {{Parts{Whole}, Parts{Whole, Lower}, 0}, {Parts{Whole, Whole}, std::nullopt, 1}},
      // fewer of one kind than before
      {{Parts{Whole, Whole}, Parts{Whole, Whole}, 1}, {Parts{Whole}, std::nullopt, 1}},
      {{Parts{Whole, Whole}, Parts{Whole, Whole}, 1}, {std::nullopt, Parts{Whole}, 1}},
      {{Parts{Whole}, Parts{Whole}, plain}, {Parts{}, std::nullopt, plain}},
      {{Parts{Whole}, Parts{Whole}, plain}, {std::nullopt, Parts{}, plain}},
      // and more again
      {{Parts{}, Parts{Whole}, plain}, {Parts{Left}, std::nullopt, plain}, {Parts{Left, Right}, Parts{Whole, Lower}, 1}},
  };
}

// what the target holds after a case, pixel by pixel in rows: draw n's value is n + 1. `scissoring`: whether the
// scissor test is on, which Direct3D 12's always is
inline std::vector<float>
expected(const Case &draws, bool scissoring = true) {
  std::vector<float> pixels(width * height);
  std::vector<Part> viewports, scissors;
  for (size_t n = 0; n < draws.size(); n++) {
    auto &draw = draws[n];
    if (draw.viewports)
      viewports = *draw.viewports;
    if (draw.scissors)
      scissors = *draw.scissors;
    uint32_t index = draw.index == plain ? 0 : draw.index;
    Rectangle through = rectangle(index < viewports.size() ? viewports[index] : Nothing),
              scissor = rectangle(!scissoring ? Whole : index < scissors.size() ? scissors[index] : Nothing);
    for (int y = std::max(through.top, scissor.top); y < std::min(through.bottom, scissor.bottom); y++)
      for (int x = std::max(through.left, scissor.left); x < std::min(through.right, scissor.right); x++)
        pixels[y * width + x] = n + 1;
  }
  return pixels;
}

// compares a case's pixels, `pitch` bytes a row; returns the failures
inline unsigned
check(const char *what, size_t number, const std::vector<float> &want, const char *rows, size_t pitch) {
  unsigned wrong = 0;
  for (uint32_t y = 0; y < height; y++)
    for (uint32_t x = 0; x < width; x++) {
      float got = *(const float *)(rows + y * pitch + x * sizeof(float));
      if (got != want[y * width + x] && wrong++ < 2)
        printf("%scase %zu: pixel %u,%u holds %g, want %g\n", what, number, x, y, got, want[y * width + x]);
    }
  return wrong;
}

} // namespace viewports
