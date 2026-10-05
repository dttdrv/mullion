// shared by d3d11_tessellator and d3d12_tessellator: the shaders, the patches and what the tessellator makes of them.
// contract: the fixed function tessellator is Direct3D's reference tessellator (D3D11.3 11.7.1), compiled here from
// Microsoft's source (reference/) as the expectation.
// - points: with point output a patch's domain points come out once each (11.7.14), at the reference's coordinates
//   to the bit, in every domain and partitioning. each draw has patches of factors at and beside their ranges' ends,
//   whole and between, equal and mixed, and ones that cull the patch (zero, negative, NaN: 11.7.11.2). the pixel
//   shader logs every point it is run for: its patch and domain location.
// - triangles: a quad or triangle patch's triangles cover the patch once, without a gap or a pixel twice, and are
//   clockwise or counter-clockwise as the hull shader says, where u goes right and v down (11.5): drawn adding 1
//   to a pixel, with back faces culled, clockwise triangles leave 1 over the patch and counter-clockwise ones 0.
//   the two patches whose insides make the most workloads and the most vertices a threadgroup has are also drawn as
//   many at once, which fills every threadgroup, and leave their count. the triangles join the points as the
//   reference's do (11.7.6): a vertex carries the product of its location's coordinates, which a triangle
//   interpolates between its corners, so a pixel shows which triangle is over it.
// - lines: an isoline patch's lines are at v = line / lines, none at 1 (11.6), each cut where the reference puts
//   its points: a vertex carries u squared, and a pixel shows it interpolated along the segment that draws the
//   pixel, which is the one that leaves the pixel's diamond (3.4.3): going right along a row's centers, the one over
//   the pixel's right edge, with its points snapped to 256ths of a pixel (3.4.1).
// - a geometry shader after the tessellator takes each of those primitives once for each of its instances, with the
//   domain shader's vertices in the tessellator's order and the patch's SV_PrimitiveID (11.7.9.1): all of the above
//   holds through one that passes its primitive on, where instance i adds 1 + i to a pixel instead of 1 and only the
//   first instance logs and carries its value. it runs once for each: where it counts its runs (`counted`, in the
//   log's second word), the count is the points' times its instances. where the stage has bindings of its own
//   (`bound`), an instance's 1 + i is a constant buffer's 1 plus a buffer's element i, bound to the geometry stage:
//   they are the stage's with a tessellator and without one, so a draw of each after the other finds them.
// - none of it depends on what else the domain shader outputs: a factor is the hull shader's up to 64 (11.7.2,
//   maxtessfactor) however many registers a vertex has, so all of the above is drawn again by a domain shader that
//   fills `filled_outputs` output registers. Direct3D has 32; a Metal mesh has 124 scalars, the primitive's number
//   among them, which is 30 registers (the audit's N19).
// - a primitive's render target layer is its leading vertex's (13.4, 15.15), the vertex that also gives it its
//   constant attributes (8.14), which after a tessellator may be any of a triangle's (11.7.9): a geometry shader
//   passes each triangle on with every vertex's own layer, a function of its location that differs between a
//   triangle's corners, and the pixel shader says the layer of the location it gets as a constant attribute. so a
//   pixel of a layer says that layer, and the layers together cover the patch once.
#pragma once
#include "reference/reference.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

namespace tessellator {

static const char hlsl[] = R"hlsl(
// a patch's factors: SV_TessFactor's, then SV_InsideTessFactor's
cbuffer Factors : register(b0) { float4 factors[2 * PATCHES]; };
struct CP { float unused : UNUSED; };
CP vs() {
  CP o;
  o.unused = 0;
  return o;
}
struct PC {
  float edges[EDGES] : SV_TessFactor;
#if INSIDES
  float inside[INSIDES] : SV_InsideTessFactor;
#endif
};
PC pc(uint id : SV_PrimitiveID) {
  PC o;
  [unroll] for (int i = 0; i < EDGES; i++)
    o.edges[i] = factors[2 * id][i];
#if INSIDES
  [unroll] for (int j = 0; j < INSIDES; j++)
    o.inside[j] = factors[2 * id + 1][j];
#endif
  return o;
}
[domain(DOMAIN)] [partitioning(PARTITIONING)] [outputtopology(TOPOLOGY)] [outputcontrolpoints(1)]
[patchconstantfunc("pc")] [maxtessfactor(64)]
CP hs(InputPatch<CP, 1> ip) { return ip[0]; }
struct DSO {
  float4 pos : SV_Position;
  nointerpolation float3 location : LOCATION;
  // u squared along lines, u v across triangles
  float value : VALUE;
#if LAYERED
  uint layer : LAYER;
#endif
#if FILL
  float4 fill[FILL] : MORE;
#endif
};
// one of LAYERS (a power of two) from a location's bits, so that neighbors differ
uint layer_of(float3 location) {
  return (asuint(location.x) * LAYER_HASH_U + asuint(location.y) * LAYER_HASH_V) >> (32 - LAYER_BITS);
}
[domain(DOMAIN)]
DSO ds(PC pc, LOCATION location : SV_DomainLocation, const OutputPatch<CP, 1> p) {
  DSO o;
  o.location = float3(location PAD);
  o.value = location.x * (TRIANGLES ? location.y : location.x);
#if LAYERED
  o.layer = layer_of(o.location);
#endif
#if FILL
  for (uint f = 0; f < FILL; f++)
    o.fill[f] = float4(o.location, f);
#endif
  // u goes right and v down; the patch spans SPAN of the target's width, its lines DOWN lower
  o.pos = float4((location.x - 0.5) * 2 * SPAN, (0.5 - location.y - DOWN) * 2 * SPAN, 0, 1);
  return o;
}
// a point without a tessellator, at the center of a pixel
DSO vs_point() {
  DSO o = (DSO)0;
  o.pos = float4(1.0 / WIDTH, 1.0 / HEIGHT, 0, 1);
  return o;
}
RWByteAddressBuffer points : register(u1);
#if LAYERED
struct GSO {
  float4 pos : SV_Position;
  nointerpolation float3 location : LOCATION;
  float value : VALUE;
  uint layer : SV_RenderTargetArrayIndex;
};
[maxvertexcount(CORNERS)]
void gs(PRIMITIVE DSO i[CORNERS], inout STREAM<GSO> o) {
  [unroll] for (int corner = 0; corner < CORNERS; corner++) {
    GSO g;
    g.pos = i[corner].pos;
    g.location = i[corner].location;
    g.value = i[corner].value;
    g.layer = i[corner].layer;
    o.Append(g);
  }
}
float4 ps_layer(GSO i) : SV_Target { return float4(1, 1 + layer_of(i.location), 0, 0); }
#define PSI GSO
#define WEIGHT(i) 1
#define PATCH(i) patch
#define PATCH_INPUT , uint patch : SV_PrimitiveID
#elif INSTANCES
struct GSO {
  float4 pos : SV_Position;
  nointerpolation float3 location : LOCATION;
  float value : VALUE;
  nointerpolation float weight : WEIGHT;
  uint patch : SV_PrimitiveID;
};
#if BOUND
cbuffer First : register(b1) { float first; };
Buffer<float> steps : register(t2);
#define INSTANCE_WEIGHT(instance) (first + steps[instance])
#else
#define INSTANCE_WEIGHT(instance) (1 + instance)
#endif
[instance(INSTANCES)] [maxvertexcount(CORNERS)]
void gs(PRIMITIVE DSO i[CORNERS], uint patch : SV_PrimitiveID, uint instance : SV_GSInstanceID, inout STREAM<GSO> o) {
#if COUNTED
  points.InterlockedAdd(4, 1);
#endif
  for (int corner = 0; corner < CORNERS; corner++) {
    GSO g;
    g.pos = i[corner].pos;
    g.location = i[corner].location;
    g.value = i[corner].value;
    g.weight = INSTANCE_WEIGHT(instance);
    g.patch = patch;
    o.Append(g);
  }
}
#define PSI GSO
#define WEIGHT(i) i.weight
#define PATCH(i) i.patch
#define PATCH_INPUT
#else
#define PSI DSO
#define WEIGHT(i) 1
#define PATCH(i) patch
#define PATCH_INPUT , uint patch : SV_PrimitiveID
#endif
float4 ps_log(PSI i PATCH_INPUT) : SV_Target {
  if (WEIGHT(i) == 1) {
    uint slot;
    points.InterlockedAdd(0, 1, slot);
    points.Store4(8 + 16 * slot, uint4(PATCH(i), asuint(i.location)));
  }
  return WEIGHT(i);
}
float4 ps_cover(PSI i) : SV_Target { return float4(WEIGHT(i), WEIGHT(i) == 1 ? i.value : 0, 0, 0); }
)hlsl";

using reference::Domain;
using reference::Partitioning;

const char *const domain_names[] = {"isoline", "tri", "quad"};
const char *const partitioning_names[] = {"integer", "pow2", "fractional_odd", "fractional_even"};
// a domain's factors: SV_TessFactor's elements, SV_InsideTessFactor's
const uint32_t edges[] = {2, 3, 4}, insides[] = {0, 1, 2};

const uint32_t patches = 40;
// the target of the triangles and lines: a line of any count of lines up to 4, and of 64, is over a row's centers
const uint32_t width = 64, height = 192;

// the layers a location's bits choose from, and the odd numbers that spread the bits
const uint32_t layer_bits = 1, layers = 1u << layer_bits, layer_hash[2] = {2654435761u, 2246822519u};

// the output registers the domain shader has without `fill`: its position, location and value; and with it
const uint32_t outputs = 3, filled_outputs = 30;

// what a pixel gets from a primitive: 1, or with a geometry shader 1 + i from its instance i
inline uint32_t
weight(uint32_t instances) {
  return instances ? instances * (instances + 1) / 2 : 1;
}

// `topology`: point, line, triangle_cw or triangle_ccw. `instances`: of the geometry shader, 0 for none. `counted`:
// the geometry shader counts its runs in the log, where the API lets it write there and the log is bound. `bound`:
// it takes its instances' weights from the geometry stage's bindings, where the API has bindings for a stage.
// `layered`: the geometry shader is the one that passes its primitive on to its vertices' layers. `fill`: output
// registers the domain shader has beside its own
inline std::vector<std::string>
defines(Domain domain, Partitioning partitioning, const char *topology, uint32_t instances, bool counted, bool bound,
        bool layered = false, uint32_t fill = 0) {
  bool points = !strcmp(topology, "point"), lines = !strcmp(topology, "line");
  return {
      "INSTANCES=" + std::to_string(instances),
      counted ? "COUNTED=1" : "COUNTED=0",
      bound ? "BOUND=1" : "BOUND=0",
      layered ? "LAYERED=1" : "LAYERED=0",
      "FILL=" + std::to_string(fill),
      "LAYER_BITS=" + std::to_string(layer_bits),
      "LAYER_HASH_U=" + std::to_string(layer_hash[0]) + "u",
      "LAYER_HASH_V=" + std::to_string(layer_hash[1]) + "u",
      points || lines ? "TRIANGLES=0" : "TRIANGLES=1",
      points ? "CORNERS=1" : lines ? "CORNERS=2" : "CORNERS=3",
      points ? "PRIMITIVE=point" : lines ? "PRIMITIVE=line" : "PRIMITIVE=triangle",
      points ? "STREAM=PointStream" : lines ? "STREAM=LineStream" : "STREAM=TriangleStream",
      "PATCHES=" + std::to_string(patches),
      "WIDTH=" + std::to_string(width),
      "HEIGHT=" + std::to_string(height),
      "EDGES=" + std::to_string(edges[domain]),
      "INSIDES=" + std::to_string(insides[domain]),
      std::string("DOMAIN=\"") + domain_names[domain] + "\"",
      std::string("PARTITIONING=\"") + partitioning_names[partitioning] + "\"",
      std::string("TOPOLOGY=\"") + topology + "\"",
      domain == reference::triangle ? "LOCATION=float3" : "LOCATION=float2",
      domain == reference::triangle ? "PAD=" : "PAD=, 0",
      // points stay off the target's edges
      points ? "SPAN=0.5" : "SPAN=1",
      // half a pixel
      lines ? "DOWN=(0.5 / " + std::to_string(height) + ")" : "DOWN=0",
  };
}

// a patch's factors, as the shader's constants have them
using Factors = std::array<float, 8>;

inline float
from_bits(uint32_t bits) {
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

// the factors that are a range's end, beside one, or cull the patch; those after the first `small` are large, and
// rare below
const uint32_t small = 19;
const float notable[] = {
    1,          from_bits(0x3f800080) /* 1 + 2^-16 */,
    1.00001f,   1.5f,
    2,          2.5f,
    3,          3.0001f,
    4,          5,
    6.5f,       7,
    0.5f,       from_bits(1) /* the least above zero */,
    0,          -0.0f,
    -1,         from_bits(0x7fc00000) /* NaN */,
    from_bits(0xffc00000) /* NaN */,
    16,         17.3f,
    31,         62.9f,
    63,         63.5f,
    64,         100,
    from_bits(0x7f800000) /* infinity */,
};

// the patches of a draw: notable factors and others, a third of the patches with all their factors equal
inline std::vector<Factors>
draw_patches(Domain domain, uint32_t seed) {
  auto next = [&] { return seed = seed * 1664525u + 1013904223u, seed >> 8; };
  auto factor = [&] {
    uint32_t kind = next() % 8, value = next();
    // the large ones less often, for the points' count
    bool large = next() % 4 == 0;
    if (kind < 4)
      return notable[value % (large ? std::size(notable) : small)];
    if (kind < 6)
      return (float)(1 + value % (large ? 64 : 12));
    return 0.5f + (value % 65536) / 65536.0f * (large ? 65 : 12);
  };
  std::vector<Factors> out(patches);
  for (auto &patch : out) {
    bool equal = next() % 3 == 0;
    float all = factor();
    for (uint32_t i = 0; i < edges[domain]; i++)
      patch[i] = equal ? all : factor();
    for (uint32_t i = 0; i < insides[domain]; i++)
      patch[4 + i] = equal ? all : factor();
  }
  return out;
}

// a patch's factors as the reference takes them: the edges', then the inside's
inline std::array<float, 6>
reference_factors(Domain domain, const Factors &patch) {
  std::array<float, 6> factors{};
  for (uint32_t i = 0; i < edges[domain]; i++)
    factors[i] = patch[i];
  for (uint32_t i = 0; i < insides[domain]; i++)
    factors[edges[domain] + i] = patch[4 + i];
  return factors;
}

// the reference's points of a patch, as (u, v, w) bits, in order
using Point = std::array<uint32_t, 3>;
inline std::vector<Point>
expected_points(Domain domain, Partitioning partitioning, const Factors &patch) {
  std::vector<Point> out;
  for (auto &p : reference::points(domain, partitioning, reference_factors(domain, patch).data())) {
    // a triangle's third coordinate is what the other two leave of 1, which their 16 bits make exact
    float location[3] = {p.u, p.v, domain == reference::triangle ? 1 - p.u - p.v : 0};
    Point bits;
    memcpy(bits.data(), location, sizeof(location));
    out.push_back(bits);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// a draw's log starts with the count of points the pixel shader logged and of the geometry shader's runs
const uint32_t log_header = 2;

// compares a draw's log (its header, then patch and location of each point) with the reference; returns the failures
inline unsigned
check_points(
    const char *name, Domain domain, Partitioning partitioning, const std::vector<Factors> &draw,
    const std::vector<uint32_t> &log, size_t &total
) {
  std::vector<std::vector<Point>> got(draw.size());
  unsigned failures = 0;
  size_t capacity = (log.size() - log_header) / 4;
  for (size_t i = 0; i < std::min<size_t>(log[0], capacity); i++) {
    const uint32_t *record = &log[log_header + 4 * i];
    if (record[0] < got.size())
      got[record[0]].push_back({record[1], record[2], record[3]});
    else if (failures++ < 4)
      printf("%s: a point of patch %u, of %zu\n", name, record[0], got.size());
  }
  size_t expected_total = 0;
  for (size_t patch = 0; patch < draw.size(); patch++) {
    auto want = expected_points(domain, partitioning, draw[patch]);
    expected_total += want.size();
    std::sort(got[patch].begin(), got[patch].end());
    if (got[patch] == want)
      continue;
    if (failures++ < 4) {
      std::vector<Point> missing, extra;
      std::set_difference(want.begin(), want.end(), got[patch].begin(), got[patch].end(), std::back_inserter(missing));
      std::set_difference(got[patch].begin(), got[patch].end(), want.begin(), want.end(), std::back_inserter(extra));
      printf(
          "%s: patch %zu (factors %g %g %g %g, inside %g %g): %zu points, want %zu; %zu missing, %zu not the "
          "reference's", name, patch, draw[patch][0], draw[patch][1], draw[patch][2], draw[patch][3], draw[patch][4],
          draw[patch][5], got[patch].size(), want.size(), missing.size(), extra.size()
      );
      if (!missing.empty())
        printf(", first missing %08x %08x", missing[0][0], missing[0][1]);
      if (!extra.empty())
        printf(", first other %08x %08x", extra[0][0], extra[0][1]);
      printf("\n");
    }
  }
  if (log[0] != expected_total && failures++ < 4)
    printf("%s: %u points logged, want %zu\n", name, log[0], expected_total);
  total += expected_total;
  return failures;
}

// the most points a draw's patches make, for the log's size
inline size_t
points_of(Domain domain, Partitioning partitioning, const std::vector<Factors> &draw) {
  size_t count = 0;
  for (auto &patch : draw)
    count += expected_points(domain, partitioning, patch).size();
  return count;
}

// the patches drawn alone as triangles or lines: factors of one kind and mixed, small and the largest, whole and
// between, one that culls, and first the `fullest` whose insides make the most workloads and the most vertices in one
const uint32_t fullest = 2;
const Factors alone[] = {
    {64, 64, 64, 64, 64, 33}, {64, 64, 64, 64, 64, 63},
    {1, 1, 1, 1, 1, 1},       {2, 2, 2, 2, 2, 2},         {3, 3, 3, 3, 3, 3},       {4, 4, 4, 4, 4, 4},
    {64, 64, 64, 64, 64, 64}, {63, 63, 63, 63, 63, 63},   {1, 1, 1, 1, 5, 9},       {7, 2, 13, 4, 1, 1},
    {5.5f, 2.25f, 9.75f, 3, 6.5f, 4.125f},                {1, 64, 3, 17, 64, 2},    {3, 3, 3, 3, 64, 5},
    {2, 16.5f, 2, 2, 12, 33}, {4, 4, 0, 4, 4, 4},
};

// what a pixel of a patch drawn alone as triangles must hold of the 1 each triangle adds: a quad covers the target, a
// triangle the pixels with u + v below 1, and a culled patch nothing. -1 for a pixel whose center is on a triangle
// patch's slanted edge, where the rasterizer's snapping of the edge's points decides
inline float
expected_cover(Domain domain, Partitioning partitioning, const Factors &patch, bool clockwise, uint32_t x, uint32_t y) {
  // twice the sum of the center's coordinates, in pixels of a target as wide as high
  uint32_t sum = (2 * x + 1) * height + (2 * y + 1) * width, whole = 2 * width * height;
  if (domain == reference::triangle && sum == whole)
    return -1;
  if (!clockwise || expected_points(domain, partitioning, patch).empty())
    return 0;
  return domain == reference::quad || sum < whole;
}

// how far a pixel's interpolated product may be from the reference's triangle's: the rasterizer snaps a vertex to a
// 256th of a pixel (3.4.1), half of that either way, along each axis, and the product goes from 0 to 1 across the
// target; twice that
const float product_tolerance = (1.0f / width + 1.0f / height) / 256;

// whether the patch's triangles are large enough to tell by their products: cutting a cell of the patch the other
// way moves the product at its middle by half the cell's area, here well above the tolerance
inline bool
products_tell(const Factors &patch) {
  float most = 1;
  for (float f : patch)
    most = std::max(most, std::min(f, 64.0f));
  return 0.5f / (most * most) > 8 * product_tolerance;
}

// what each pixel of a patch drawn alone as clockwise triangles holds of the product of its location's coordinates,
// as the reference's triangle over the pixel's center interpolates it; NaN where there is none. triangles that share
// an edge agree along it
inline std::vector<float>
expected_products(Domain domain, Partitioning partitioning, const Factors &patch) {
  std::vector<float> out(width * height, std::nanf(""));
  auto corners = reference::corners(domain, partitioning, reference_factors(domain, patch).data(), 3);
  for (size_t t = 0; t + 3 <= corners.size(); t += 3) {
    // in pixels: u goes right and v down
    double x[3], y[3], value[3];
    for (int c = 0; c < 3; c++) {
      x[c] = corners[t + c].u * width;
      y[c] = corners[t + c].v * height;
      value[c] = (double)corners[t + c].u * corners[t + c].v;
    }
    double area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    if (area == 0)
      continue;
    auto first = [](double a, double b, double c) { return (uint32_t)std::max(0.0, std::floor(std::min({a, b, c}) - 0.5)); };
    auto last = [](double a, double b, double c, uint32_t limit) {
      return std::min<double>(limit - 1, std::ceil(std::max({a, b, c}) - 0.5));
    };
    for (uint32_t py = first(y[0], y[1], y[2]); py <= last(y[0], y[1], y[2], height); py++)
      for (uint32_t px = first(x[0], x[1], x[2]); px <= last(x[0], x[1], x[2], width); px++) {
        // each corner's weight: the part of the triangle opposite it
        double cx = px + 0.5, cy = py + 0.5, weights[3], sum = 0;
        bool inside = true;
        for (int c = 0; c < 3; c++) {
          int a = (c + 1) % 3, b = (c + 2) % 3;
          weights[c] = ((x[a] - cx) * (y[b] - cy) - (x[b] - cx) * (y[a] - cy)) / area;
          inside &= weights[c] >= 0;
          sum += weights[c] * value[c];
        }
        if (inside)
          out[py * width + px] = (float)sum;
      }
  }
  return out;
}

// the shader's layer_of
inline uint32_t
layer_of(float u, float v) {
  uint32_t bits[2];
  memcpy(&bits[0], &u, 4);
  memcpy(&bits[1], &v, 4);
  return (bits[0] * layer_hash[0] + bits[1] * layer_hash[1]) >> (32 - layer_bits);
}

// how many of a patch's triangles have corners of more than one layer: where the leading vertex decides
inline unsigned
mixed_layers(Domain domain, Partitioning partitioning, const Factors &patch) {
  unsigned mixed = 0;
  auto corners = reference::corners(domain, partitioning, reference_factors(domain, patch).data(), 3);
  for (size_t t = 0; t + 3 <= corners.size(); t += 3)
    mixed += layer_of(corners[t].u, corners[t].v) != layer_of(corners[t + 1].u, corners[t + 1].v) ||
             layer_of(corners[t].u, corners[t].v) != layer_of(corners[t + 2].u, corners[t + 2].v);
  return mixed;
}

// a patch drawn alone as clockwise triangles through the layering geometry shader, each layer's pixels as the
// target's two channels: a pixel a layer has says that layer, and the layers together cover as one target would
inline unsigned
check_layers(const char *name, Domain domain, Partitioning partitioning, const Factors &patch,
             const std::vector<float> drawn[layers]) {
  unsigned wrong = 0;
  for (uint32_t y = 0; y < height; y++)
    for (uint32_t x = 0; x < width; x++) {
      float cover = 0, want = expected_cover(domain, partitioning, patch, true, x, y);
      for (uint32_t layer = 0; layer < layers; layer++) {
        const float *pixel = &drawn[layer][2 * (y * width + x)];
        cover += pixel[0];
        if (pixel[0] != 0 && pixel[1] != pixel[0] * (1 + layer) && wrong++ < 2)
          printf(
              "%s, factors %g %g %g %g, inside %g %g: pixel %u,%u of layer %u is of a primitive whose leading vertex has layer %g\n",
              name, patch[0], patch[1], patch[2], patch[3], patch[4], patch[5], x, y, layer, pixel[1] / pixel[0] - 1
          );
      }
      if (want >= 0 && cover != want && wrong++ < 2)
        printf(
            "%s, factors %g %g %g %g, inside %g %g: pixel %u,%u is covered %g times over the layers, want %g\n", name,
            patch[0], patch[1], patch[2], patch[3], patch[4], patch[5], x, y, cover, want
        );
    }
  return wrong;
}

// an isoline patch's factors drawn alone: the lines' density, then their detail. the densities put lines over rows
const Factors lines_alone[] = {{1, 1}, {2, 8}, {3, 5.5f}, {4, 64}, {2.5f, 3}, {64, 4}, {1, 6.25f}, {0, 4}};

// what pixel (x, y) of an isoline patch drawn alone as lines must hold: whether a line is over it, and u squared
// at the pixel's center on the segment that starts left of the pixel's right edge and ends at it or beyond.
// `decided` is false where a point is halfway between two snapped places, one of them the edge
struct Line {
  bool on, decided;
  float square;
};
inline Line
expected_line(Partitioning partitioning, const Factors &patch, uint32_t x, uint32_t y) {
  auto points = expected_points(reference::isoline, partitioning, patch);
  // in 256ths of a pixel
  float u = (x + 0.5f) / width, edge = (x + 1) * 256, before = 0, after = 1;
  Line line{false, true};
  for (auto &p : points) {
    float pu, pv;
    memcpy(&pu, &p[0], 4);
    memcpy(&pv, &p[1], 4);
    line.on |= std::lround(pv * height) == y;
    float at = pu * width * 256;
    line.decided &= at != edge - 0.5f;
    if (std::nearbyint(at) < edge)
      before = std::max(before, pu);
    else
      after = std::min(after, pu);
  }
  line.square = u * (before + after) - before * after;
  return line;
}

} // namespace tessellator
