#include <metal_stdlib>

using namespace metal;

// the fixed function tessellator. its points are those of Direct3D's reference tessellator, which D3D11.3 11.7.1
// names as the tessellator's definition: every coordinate is 16.16 fixed point, placed by one factor's partition of
// [0, 1], so a point is the same bits whichever edge reaches it.
//
// the hull stage cuts a patch into rings and writes workloads, each a mesh threadgroup's work: one or two frames, a
// frame being a side of a ring (its outer edge) and the same side of the next ring in (its inner edge), which the
// mesh stage fills with triangles, joined as the reference joins them (11.7.6). an isoline patch's workloads hold two
// lines instead.

enum class partitioning {
  integer,
  pow2,
  fractional_odd,
  fractional_even,
};

using fxp = uint;

constexpr constant fxp fxp_one = 0x10000;
constexpr constant fxp fxp_half = 0x8000;
constexpr constant fxp fxp_third = 0x5555;
constexpr constant fxp fxp_two_thirds = 0xaaaa;

// a factor the tessellator takes, and whether it cuts into an odd number of segments
struct tess_factor {
  fxp value;
  bool odd;
};

// whether a factor is above a positive bound: compared as bits, false for a NaN whatever the compiler assumes of
// floats
bool
above(float factor, float bound) {
  return as_type<uint>(factor) - as_type<uint>(bound) - 1 < 0x7f800000u - as_type<uint>(bound);
}

// a hull shader's factor culls its patch unless it is above zero
bool
culls(float factor) {
  return !above(factor, 0);
}

// in its partitioning's range, and whole when the partitioning is (D3D11.3 11.7.11). pow2 is integer here: rounding
// to a power of two is the hull shader's work (11.7.7.1)
tess_factor
take(float factor, partitioning p, float lower) {
  bool whole = p == partitioning::integer || p == partitioning::pow2;
  float upper = p == partitioning::fractional_odd ? 63 : 64;
  // a NaN becomes the lower bound
  factor = !above(factor, lower) ? lower : above(factor, upper) ? upper : factor;
  if (whole)
    factor = ceil(factor);
  fxp value = rint(factor * 65536.0f);
  return {value, whole ? bool(value >> 16 & 1) : p == partitioning::fractional_odd};
}

float
lowest(partitioning p) {
  return p == partitioning::fractional_even ? 2 : 1;
}

// one factor's partition of [0, 1]
struct tess_partition {
  fxp inv_floor, inv_ceil; // a segment's length at the whole factors around this one
  fxp fraction;            // how far the factor is from the lower to the upper
  int half_points;         // the points before the middle
  int split;               // the lower factor's point that is splitting in two
  int points;
};

fxp
ceil(fxp x) {
  return (x + 0xffff) & ~0xffffu;
}

int
without_top_bit(int x) {
  return x > 0 ? x & ~(0x40000000 >> (clz(x) - 1)) : 0;
}

tess_partition
partition_of(fxp factor, bool odd) {
  tess_partition p;
  fxp halved = (factor + 1) / 2;
  // a factor of 1 without odd parity is cut as 2
  if (odd || halved == fxp_half)
    halved += fxp_half;
  fxp lower = halved & ~0xffffu, upper = ceil(halved);
  p.fraction = halved - lower;
  p.half_points = upper >> 16;
  p.split = lower == upper ? p.half_points + 1
            : odd ? (lower == fxp_one ? 0 : (without_top_bit((lower >> 16) - 1) << 1) + 1)
                  : (without_top_bit(lower >> 16) << 1) + 1;
  int floor_segments = (lower >> 15) - odd, ceil_segments = (upper >> 15) - odd;
  p.inv_floor = (fxp_one + floor_segments / 2) / floor_segments;
  p.inv_ceil = (fxp_one + ceil_segments / 2) / ceil_segments;
  p.points = odd ? ceil(fxp_half + (factor + 1) / 2) >> 15 : (ceil((factor + 1) / 2) >> 15) + 1;
  return p;
}

// where a partition's point is
fxp
place(tess_partition p, bool odd, int point) {
  bool flip = point >= p.half_points;
  if (flip)
    point = (p.half_points << 1) - point - odd;
  if (point == p.half_points)
    return fxp_half;
  int on_floor = point - (point > p.split);
  fxp location = (on_floor * p.inv_floor * (fxp_one - p.fraction) + point * p.inv_ceil * p.fraction + fxp_half) >> 16;
  return flip ? fxp_one - location : location;
}

fxp
place(tess_factor factor, int point) {
  return place(partition_of(factor.value, factor.odd), factor.odd, point);
}

int
points(tess_factor factor) {
  return partition_of(factor.value, factor.odd).points;
}

// how an edge's points lie in the domain, from their place along the edge and the coordinate the edge keeps
enum edge_kind : uchar {
  quad_u,     // (place, kept)
  quad_v,     // (kept, place)
  tri_0,      // a triangle's sides, by the edge they are parallel to: u = 0,
  tri_1,      // v = 0,
  tri_2,      // w = 0
  tri_center, // the one point in the middle of a triangle
  edge_kind_mask = 7,
  edge_odd = 8,      // its factor's parity
  edge_reverse = 16, // its points go down its factor's partition
  // of a frame's inner edge, how the frame's triangles join its edges (tess_zip). without these, as a ring's side
  // between two rings of the inside factor
  stitch_transition = 32, // the patch's outermost ring, whose edges have two factors
  stitch_strip = 64,      // the two middle rows of a quad patch whose shorter axis has an even count of points
  stitch_turned = 128,    // of a strip: its middle quad is cut the other way
};

// a run of points along one side of a ring
struct tess_edge {
  fxp factor;    // the factor whose partition places the points
  fxp kept;      // the coordinate that is the same for all of them
  uchar first;   // the first point's index on the partition
  char segments; // its points less one, or -1 for no edge
  char owned;    // how many of them, from the first, the workload outputs when the tessellator outputs points
  uchar kind;
};

constexpr constant tess_edge no_edge = {0, 0, 0, -1, 0, 0};

struct tess_workload {
  // a frame's inner edge, then its outer edge; then the second frame's
  tess_edge edge[4];
  int patch_index;
};

tess_edge
run(uchar kind, tess_factor factor, int start, int end, bool reverse, fxp kept, int owned = 0) {
  return {factor.value,
          kept,
          uchar(reverse ? end : start),
          char(end - start),
          char(owned),
          uchar(kind | (factor.odd ? edge_odd : 0) | (reverse ? edge_reverse : 0))};
}

struct tess_frame {
  tess_edge inner, outer;
};

constexpr constant tess_frame no_frame = {no_edge, no_edge};

// where a patch writes its workloads: the payload's room for its threadgroup's, and the place of the patch's next
// one, which counts them. the patches' workloads follow each other in the patches' order, so a patch starts where
// those before it end: it is counted first, with no room, and written once every patch of the threadgroup is counted
struct tess_output {
  object_data tess_workload *workloads;
  thread int *next;
  int capacity;
  int patch_index;
};

// the payload has room for the most workloads its patches can have (get_max_potential_workload_count): one past it
// is not written
void
emit(tess_output out, tess_frame a, tess_frame b) {
  int index = (*out.next)++;
  if (index < out.capacity)
    out.workloads[index] = {{a.inner, a.outer, b.inner, b.outer}, out.patch_index};
}

// a workload is one frame, or two that are small together: ring i's and ring (rings - 1 - i)'s, counting the rings
// of the patch's longer axis, which keeps a workload's points near twice the longest edge's. the frame inside the last
// ring, if the patch has one, takes a free place among the middle ring's workloads, or its own
template <typename Patch>
void
emit_workloads(Patch patch, tess_output out) {
  bool last = patch.has_last();
  for (int i = 0, j = patch.long_rings() - 1; i < patch.rings() && i <= j; i++, j--) {
    if (i < j) {
      for (int t = 0; t < Patch::sides; t++)
        emit(out, patch.frame(i, t), j < patch.rings() ? patch.frame(j, t) : no_frame);
      continue;
    }
    for (int t = 0; t < Patch::sides; t += 2) {
      bool free = t + 1 == Patch::sides;
      emit(out, patch.frame(i, t), !free ? patch.frame(i, t + 1) : last ? patch.last_frame() : no_frame);
      last = last && !free;
    }
  }
  if (last)
    emit(out, patch.last_frame(), no_frame);
}

struct quad_patch {
  enum { sides = 4 };
  // the factors of the edges u = 0, v = 0, u = 1, v = 1, and of the inside along u and v
  tess_factor out[4], in[2];
  // the inside factors' points
  int n[2];

  // ring k's side s, around the patch from its edge s
  tess_edge side(int k, int s) const {
    bool reverse = s == 0 || s == 3;
    if (k == 0)
      return run(s & 1 ? quad_u : quad_v, out[s], 0, points(out[s]) - 1, reverse, s >= 2 ? fxp_one : 0);
    int axis = s & 1 ? 0 : 1, other = 1 - axis;
    return run(axis ? quad_v : quad_u, in[axis], k, n[axis] - 1 - k, reverse, place(in[other], s < 2 ? k : n[other] - 1 - k));
  }
  int shorter() const { return min(n[0], n[1]); }
  int rings() const { return (shorter() - 1) >> 1; }
  int long_rings() const { return (max(n[0], n[1]) - 1) >> 1; }
  tess_frame frame(int ring, int t) const {
    int s = 3 - t;
    tess_frame f = {side(ring + 1, s), side(ring, s)};
    f.outer.owned = f.outer.segments;
    // with an odd count of points the middle is a row of them: the inner edge of the sides along it
    if ((shorter() & 1) && ring + 1 == rings() && s == (n[0] >= n[1] ? 1 : 2))
      f.inner.owned = f.inner.segments + 1;
    if (ring == 0)
      f.inner.kind |= stitch_transition;
    return f;
  }
  // with an even count the last ring is two rows, which are a frame
  bool has_last() const { return !(shorter() & 1); }
  tess_frame last_frame() const {
    int k = shorter() / 2 - 1;
    // the reference's rows: along u when that axis has more points, cut alike; along v otherwise, the middle quad
    // the other way when that axis has an even count too
    bool along_u = n[0] > n[1];
    tess_frame f = along_u ? tess_frame{side(k, 3), side(k, 1)} : tess_frame{side(k, 2), side(k, 0)};
    // the far row, turned to run with the near one
    f.inner.first += f.inner.kind & edge_reverse ? -f.inner.segments : f.inner.segments;
    f.inner.kind ^= edge_reverse;
    f.inner.kind |= stitch_strip | (!along_u && !(n[1] & 1) ? stitch_turned : 0);
    f.inner.owned = f.inner.segments + 1;
    f.outer.owned = f.outer.segments + 1;
    return f;
  }
};

struct tri_patch {
  enum { sides = 3 };
  // the factors of the edges u = 0, v = 0, w = 0, and of the inside
  tess_factor out[3], in;
  int n;

  fxp kept(int k) const { return (place(in, k) * fxp_two_thirds + fxp_half) >> 16; }
  tess_edge side(int k, int s) const {
    if (k == 0)
      return run(tri_0 + s, out[s], 0, points(out[s]) - 1, s != 1, 0);
    if (2 * k == n - 1)
      return run(tri_center, in, 0, 0, false, 0);
    return run(tri_0 + s, in, k, n - 1 - k, s != 1, kept(k));
  }
  int rings() const { return (n - 1) >> 1; }
  int long_rings() const { return rings(); }
  tess_frame frame(int ring, int t) const {
    int s = (t + 1) % 3;
    tess_frame f = {side(ring + 1, s), side(ring, s)};
    f.outer.owned = f.outer.segments;
    if ((n & 1) && ring + 1 == rings() && t == 0)
      f.inner.owned = 1;
    if (ring == 0)
      f.inner.kind |= stitch_transition;
    return f;
  }
  // with an even count the last ring is one triangle: a side, and the last point of the next
  bool has_last() const { return !(n & 1); }
  tess_frame last_frame() const {
    int k = n / 2 - 1;
    return {run(tri_2, in, k, k, true, kept(k), 1), run(tri_1, in, k, k + 1, false, kept(k), 2)};
  }
};

void generate_workload_quad(
    int patch_index, thread int *out_next, object_data int *out_buffer, int capacity, int partition,
    float in0, float in1, float out0, float out1, float out2, float out3
) asm("dxmt.generate_workload.quad");

void
generate_workload_quad(
    int patch_index, thread int *out_next, object_data int *out_buffer, int capacity, int partition,
    float in0, float in1, float out0, float out1, float out2, float out3
) {
  if (culls(out0) || culls(out1) || culls(out2) || culls(out3))
    return;
  tess_output out = {(object_data tess_workload *)out_buffer, out_next, capacity, patch_index};
  partitioning p = partitioning(partition);
  float lower = lowest(p);
  quad_patch patch;
  patch.out[0] = take(out0, p, lower);
  patch.out[1] = take(out1, p, lower);
  patch.out[2] = take(out2, p, lower);
  patch.out[3] = take(out3, p, lower);
  // fractional odd: one factor above 1 puts a frame inside the patch (D3D11.3 11.7.11.3). a factor is above 1 if
  // its fixed point value is
  float epsilon = 1.0f / 65536.0f, one = 1 + epsilon / 2;
  if (p == partitioning::fractional_odd && (above(out0, one) || above(out1, one) || above(out2, one) ||
                                            above(out3, one) || above(in0, one) || above(in1, one)))
    lower = 1 + epsilon;
  patch.in[0] = take(in0, p, lower);
  patch.in[1] = take(in1, p, lower);
  // a patch whose factors are all 1 is its four corners
  bool ones = p != partitioning::fractional_even;
  for (int i = 0; i < 4; i++)
    ones = ones && patch.out[i].value == fxp_one;
  for (int i = 0; i < 2; i++)
    ones = ones && patch.in[i].value == fxp_one;
  for (int i = 0; i < 2; i++) {
    // a whole inside factor of 1 beside larger ones is cut as 2
    if (!ones && patch.in[i].value == fxp_one)
      patch.in[i].odd = false;
    patch.n[i] = points(patch.in[i]);
  }
  emit_workloads(patch, out);
}

void generate_workload_triangle(
    int patch_index, thread int *out_next, object_data int *out_buffer, int capacity, int partition,
    float in, float out0, float out1, float out2
) asm("dxmt.generate_workload.triangle");

void
generate_workload_triangle(
    int patch_index, thread int *out_next, object_data int *out_buffer, int capacity, int partition,
    float in, float out0, float out1, float out2
) {
  if (culls(out0) || culls(out1) || culls(out2))
    return;
  tess_output out = {(object_data tess_workload *)out_buffer, out_next, capacity, patch_index};
  partitioning p = partitioning(partition);
  float lower = lowest(p);
  tri_patch patch;
  patch.out[0] = take(out0, p, lower);
  patch.out[1] = take(out1, p, lower);
  patch.out[2] = take(out2, p, lower);
  float epsilon = 1.0f / 65536.0f, one = 1 + epsilon / 2;
  if (p == partitioning::fractional_odd && (above(out0, one) || above(out1, one) || above(out2, one)))
    lower = 1 + epsilon;
  patch.in = take(in, p, lower);
  bool ones = p != partitioning::fractional_even && patch.in.value == fxp_one;
  for (int i = 0; i < 3; i++)
    ones = ones && patch.out[i].value == fxp_one;
  if (ones) {
    // the patch's three corners
    emit(out, {run(tri_2, patch.out[2], 0, 0, true, 0, 1), run(tri_1, patch.out[1], 0, 1, false, 0, 2)}, no_frame);
    return;
  }
  if (patch.in.value == fxp_one)
    patch.in.odd = false;
  patch.n = points(patch.in);
  emit_workloads(patch, out);
}

void generate_workload_isoline(
    int patch_index, thread int *out_next, object_data int *out_buffer, int capacity, int partition,
    float density, float detail
) asm("dxmt.generate_workload.isoline");

void
generate_workload_isoline(
    int patch_index, thread int *out_next, object_data int *out_buffer, int capacity, int partition,
    float density, float detail
) {
  if (culls(density) || culls(detail))
    return;
  tess_output out = {(object_data tess_workload *)out_buffer, out_next, capacity, patch_index};
  partitioning p = partitioning(partition);
  // the lines' count is whole whatever the partitioning, and no line is at v = 1 (D3D11.3 11.6)
  tess_factor across = take(density, partitioning::integer, 1);
  tess_factor along = take(detail, p, lowest(p));
  int lines = points(across) - 1, last = points(along) - 1;
  for (int line = 0; line < lines; line += 2) {
    tess_frame pair = {run(quad_u, along, 0, last, false, place(across, line), last + 1), no_edge};
    if (line + 1 < lines)
      pair.outer = run(quad_u, along, 0, last, false, place(across, line + 1), last + 1);
    emit(out, pair, no_frame);
  }
}

float2
point(tess_edge edge, int i) {
  uchar kind = edge.kind & edge_kind_mask;
  if (kind == tri_center)
    return float2(fxp_third) / 65536.0f;
  bool odd = edge.kind & edge_odd;
  tess_partition p = partition_of(edge.factor, odd);
  int q = edge.kind & edge_reverse ? edge.first - i : edge.first + i;
  // a triangle's corner is the first point of the side that starts there: the three sides' formulas round apart
  if (kind >= tri_0 && i == edge.segments) {
    if (kind == tri_2)
      q = p.points - 1 - q;
    kind = tri_0 + (kind - tri_0 + 1) % 3;
  }
  fxp at = place(p, odd, q), along = at - (edge.kept + 1) / 2;
  uint2 uv = kind == quad_u   ? uint2(at, edge.kept)
             : kind == quad_v ? uint2(edge.kept, at)
             : kind == tri_0  ? uint2(edge.kept, along)
             : kind == tri_1  ? uint2(along, edge.kept)
                              : uint2(along, fxp_one - along - edge.kept);
  return float2(uv) / 65536.0f;
}

int get_domain_patch_index(int workload_index, object_data int *data) asm("dxmt.get_domain_patch_index");

int
get_domain_patch_index(int workload_index, object_data int *data) {
  return ((object_data tess_workload *)data)[workload_index].patch_index;
}

struct domain_location {
  float2 uv;
  bool active;
  bool iterate;
};

// the workload's vertices are its edges' points, in order
domain_location
workload_vertex(object_data tess_workload &workload, int index) {
  domain_location ret{float2(0), false, false};
  for (int e = 0; e < 4; e++) {
    tess_edge edge = workload.edge[e];
    if (index >= 0 && index <= edge.segments) {
      ret.active = true;
      ret.uv = point(edge, index);
    }
    index -= edge.segments + 1;
  }
  return ret;
}

domain_location get_domain_location(int workload_index, int thread_index, object_data int *data) asm(
    "dxmt.get_domain_location"
);

domain_location
get_domain_location(int workload_index, int thread_index, object_data int *data) {
  simdgroup_barrier(mem_flags::mem_none);

  domain_location ret = workload_vertex(((object_data tess_workload *)data)[workload_index], thread_index);

  simdgroup_barrier(mem_flags::mem_none);
  ret.iterate = simd_all(ret.active);

  return ret;
};

struct dummy_vertex {
  float4 pos [[position]];
};

struct culled_primitive {
  bool culled [[primitive_culled]];
};

// the mesh's real limits and topology are the mesh function's
using Mesh = metal::mesh<dummy_vertex, culled_primitive, 130, 128, topology::triangle>;

// `behind` has, for each vertex, a bit for every cull distance it is behind, or is null for a shader without any: a
// primitive is discarded when one distance has all its vertices behind (D3D11.3 15.4.2)
void
cull(Mesh mesh, threadgroup uint *behind, short primitive, short a, short b, short c) {
  if (behind)
    mesh.set_primitive(primitive, culled_primitive{(behind[a] & behind[b] & behind[c]) != 0});
}

// half an edge has at most 1 << ruler_levels points: the largest factor's
constexpr constant int ruler_levels = 5;

// when a place on half an edge, counted from the edge's end, gets its point as the factor grows: the ruler
// function's order (D3D11.3 11.7.5), in which the place beside the end is the last
int
ruler(int place) {
  if (place < 2)
    return place << ruler_levels;
  int zeros = ctz(place - 1);
  return (1 << (ruler_levels - 1 - zeros)) + ((place - 1) >> (zeros + 1));
}

// a frame's triangles zip its edges together, as the reference's StitchTransition and StitchRegular do (11.7.6):
// each steps along one edge, an outer step taking the outer edge's next segment and the inner edge's point, an inner
// step the inner edge's next segment and the outer edge's point. all are clockwise
struct tess_zip {
  tess_edge inner, outer;
  short slot, i, o;

  // the points half the edge has apart from its middle
  static int half_points(tess_edge edge) {
    bool odd = edge.kind & edge_odd;
    return partition_of(edge.factor, odd).half_points - odd;
  }
  int slots() const {
    return inner.kind & stitch_transition ? 4 * places + 2 : 2 * inner.segments + (inner.kind & stitch_strip ? 0 : 2);
  }
  // what slot `at` is: 1 an inner step, 2 an outer step, 0 none
  int step(int at) const {
    if (inner.kind & stitch_transition) {
      // from each end to the middle, a step along an edge at every place where it has a point, the outer edge first
      // seen from the middle; in the middle what the edges' parities leave
      int middle = at - 2 * places;
      if (middle == 0 || middle == 1) {
        bool in_odd = inner.kind & edge_odd, out_odd = outer.kind & edge_odd;
        return in_odd && out_odd ? 1 + middle : middle ? 0 : in_odd ? 1 : out_odd ? 2 : 0;
      }
      int mirrored = middle < 0 ? at : 4 * places + 1 - at, place = mirrored / 2;
      return mirrored & 1 ? (ruler(place) < half_points(outer) ? 2 : 0)
                          : (place && ruler(place) < half_points(inner) ? 1 : 0);
    }
    int segments = inner.segments;
    // a row of quads, each cut from the inner edge's point to the outer edge's next
    if (inner.kind & stitch_strip)
      return (at & 1) != ((inner.kind & stitch_turned) && at / 2 == (segments + 1) / 2 - 1) ? 1 : 2;
    // a triangle at each end, and between them quads cut towards the nearer end
    if (at == 0 || at == 2 * segments + 1)
      return 2;
    return ((at - 1) & 1) == ((at - 1) / 2 < (segments + 1) / 2) ? 2 : 1;
  }
  // the next triangle, as vertices of the workload, where the edges' first points are `base_inner` and `base_outer`
  bool next(thread short3 &triangle, short base_inner, short base_outer) {
    while (slot < slots()) {
      int kind = step(slot++);
      if (kind == 1 && i < inner.segments) {
        triangle = short3(base_inner + i, base_outer + o, base_inner + i + 1);
        i++;
        return true;
      }
      if (kind == 2 && o < outer.segments) {
        triangle = short3(base_outer + o, base_outer + o + 1, base_inner + i);
        o++;
        return true;
      }
    }
    return false;
  }

  // the places half an edge has at most, its end among them
  enum { places = (1 << ruler_levels) + 1 };
};

// the workload's triangle `n`, numbered as its frames' zips make them; false past the last
bool
workload_triangle(object_data tess_workload &workload, int n, thread short3 &triangle) {
  short base = 0;
  for (int frame = 0; frame < 4 && workload.edge[frame].segments >= 0; frame += 2) {
    tess_zip zip = {workload.edge[frame], workload.edge[frame + 1], 0, 0, 0};
    short inner = zip.inner.segments, outer = zip.outer.segments;
    while (zip.next(triangle, base, base + inner + 1))
      if (n-- == 0)
        return true;
    base += inner + outer + 2;
  }
  return false;
}

void
generate_triangles(int workload_index, object_data int *data, Mesh mesh, threadgroup uint *behind, short ccw_xor) {
  object_data tess_workload &workload = ((object_data tess_workload *)data)[workload_index];
  short primitives = 0, base = 0;
  for (int frame = 0; frame < 4 && workload.edge[frame].segments >= 0; frame += 2) {
    tess_zip zip = {workload.edge[frame], workload.edge[frame + 1], 0, 0, 0};
    short inner = zip.inner.segments, outer = zip.outer.segments;
    for (short3 triangle(0, 0, 0); zip.next(triangle, base, base + inner + 1); primitives++) {
      mesh.set_index(primitives * 3, triangle[0]);
      mesh.set_index(primitives * 3 + (1 ^ ccw_xor), triangle[1]);
      mesh.set_index(primitives * 3 + (2 ^ ccw_xor), triangle[2]);
      cull(mesh, behind, primitives, triangle[0], triangle[1], triangle[2]);
    }
    base += inner + outer + 2;
  }
  mesh.set_primitive_count(primitives);
}

void generatePrimitiveTriangle(int workload_index, object_data int *data, Mesh mesh, threadgroup uint *behind) asm(
    "dxmt.domain_generate_primitives.triangle"
);

void
generatePrimitiveTriangle(int workload_index, object_data int *data, Mesh mesh, threadgroup uint *behind) {
  generate_triangles(workload_index, data, mesh, behind, 0);
}

void generatePrimitiveTriangleCCW(int workload_index, object_data int *data, Mesh mesh, threadgroup uint *behind) asm(
    "dxmt.domain_generate_primitives.triangle_ccw"
);

void
generatePrimitiveTriangleCCW(int workload_index, object_data int *data, Mesh mesh, threadgroup uint *behind) {
  generate_triangles(workload_index, data, mesh, behind, 3);
}

void generatePrimitiveLine(int workload_index, object_data int *data, Mesh mesh, threadgroup uint *behind) asm(
    "dxmt.domain_generate_primitives.line"
);

void
generatePrimitiveLine(int workload_index, object_data int *data, Mesh mesh, threadgroup uint *behind) {
  object_data tess_workload &workload = ((object_data tess_workload *)data)[workload_index];
  short primitives = 0, base = 0;
  for (int e = 0; e < 4; e++) {
    short segments = workload.edge[e].segments;
    for (short i = 0; i < segments; i++, primitives++) {
      mesh.set_index(primitives * 2, base + i);
      mesh.set_index(primitives * 2 + 1, base + i + 1);
      cull(mesh, behind, primitives, base + i, base + i + 1, base + i);
    }
    base += segments + 1;
  }
  mesh.set_primitive_count(primitives);
}

void generatePrimitivePoint(int workload_index, object_data int *data, Mesh mesh, threadgroup uint *behind) asm(
    "dxmt.domain_generate_primitives.point"
);

void
generatePrimitivePoint(int workload_index, object_data int *data, Mesh mesh, threadgroup uint *behind) {
  object_data tess_workload &workload = ((object_data tess_workload *)data)[workload_index];
  short primitives = 0, base = 0;
  // a patch's points are output once (D3D11.3 11.7.14): a workload outputs the ones that are its own
  for (int e = 0; e < 4; e++) {
    for (short i = 0; i < workload.edge[e].owned; i++, primitives++) {
      mesh.set_index(primitives, base + i);
      cull(mesh, behind, primitives, base + i, base + i, base + i);
    }
    base += workload.edge[e].segments + 1;
  }
  mesh.set_primitive_count(primitives);
}

// a geometry shader takes the workload's primitives one at a time, each numbered as above

// which of the workload's vertices is `corner` of its primitive `n`, when the tessellator outputs `vertices` a
// primitive; -1 past the last primitive
int
primitive_vertex(object_data tess_workload &workload, int n, int corner, int vertices) {
  if (vertices == 3) {
    short3 triangle(0, 0, 0);
    return workload_triangle(workload, n, triangle) ? triangle[corner] : -1;
  }
  int base = 0;
  for (int e = 0; e < 4; e++) {
    // a line's segments, or the points that are the workload's own
    int count = vertices == 2 ? workload.edge[e].segments : workload.edge[e].owned;
    if (n < count)
      return base + n + corner;
    n -= max(count, 0);
    base += workload.edge[e].segments + 1;
  }
  return -1;
}

domain_location get_domain_primitive_location(
    int workload_index, int primitive, int corner, int vertices, int ccw, object_data int *data
) asm("dxmt.get_domain_primitive_location");

// counter-clockwise triangles have their second and third vertices exchanged, as generate_triangles does
domain_location
get_domain_primitive_location(
    int workload_index, int primitive, int corner, int vertices, int ccw, object_data int *data
) {
  object_data tess_workload &workload = ((object_data tess_workload *)data)[workload_index];
  return workload_vertex(workload, primitive_vertex(workload, primitive, ccw && corner ? 3 - corner : corner, vertices));
}

// a mesh threadgroup that has a piece of a workload makes `count` of its primitives, from `first`, each of vertices
// of its own: a thread's is a corner of one
domain_location get_domain_piece_location(
    int workload_index, int first, int thread_index, int count, int vertices, int ccw, object_data int *data
) asm("dxmt.get_domain_piece_location");

domain_location
get_domain_piece_location(
    int workload_index, int first, int thread_index, int count, int vertices, int ccw, object_data int *data
) {
  simdgroup_barrier(mem_flags::mem_none);

  int primitive = thread_index / vertices;
  domain_location ret =
      get_domain_primitive_location(workload_index, first + primitive, thread_index % vertices, vertices, ccw, data);
  ret.active = ret.active && primitive < count;

  simdgroup_barrier(mem_flags::mem_none);
  ret.iterate = simd_all(ret.active);

  return ret;
}

void generate_piece(
    int workload_index, int first, int count, int vertices, object_data int *data, Mesh mesh, threadgroup uint *behind
) asm("dxmt.domain_generate_piece");

void
generate_piece(
    int workload_index, int first, int count, int vertices, object_data int *data, Mesh mesh, threadgroup uint *behind
) {
  object_data tess_workload &workload = ((object_data tess_workload *)data)[workload_index];
  short primitives = 0;
  for (; primitives < count && primitive_vertex(workload, first + primitives, 0, vertices) >= 0; primitives++) {
    short base = primitives * vertices;
    for (short corner = 0; corner < vertices; corner++)
      mesh.set_index(base + corner, base + corner);
    cull(mesh, behind, primitives, base, base + vertices - 1, base + (vertices - 1) / 2);
  }
  mesh.set_primitive_count(primitives);
}
