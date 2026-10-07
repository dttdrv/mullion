// contract: an operation of the shading language gives what Direct3D defines for it, whatever the type it is done in
// and whatever the stage it is done in. every operation below runs on every value triple of every type the device
// has (16, 32 and 64-bit integers of both signs, half, float, double) in the vertex, hull, domain, geometry, pixel and
// compute shader, and each result is set against the same expression in C++, where each type is a number that
// follows Direct3D's rules:
// - integers wrap, shifts take the low bits of the count that the type's width needs (D3D11.3 22.11.7 ishl, 22.11.8
//   ishr, 22.11.12 ushr), and results are exact;
// - float add, subtract and multiply are the nearest representable value, divide and sqrt within 1 ULP
//   (D3D11.3 3.1.3.1, 3.1.3.2); half the same with nearest for all (3.1.5); double as IEEE (3.1.4);
// - any NaN stands for NaN, min and max return the operand that is not NaN and may return either zero of two
//   (3.1.3.1, 3.1.3.2); a float result that is a denorm may come as a zero of its sign (3.1.3.2), a half or double
//   denorm stays (3.1.4, 3.1.5);
// - mad is no less accurate than multiply then add (3.1.3.2 on fused operations): no further from the exactly
//   rounded a * b + c than the two-step result is;
// - exp2, rcp and rsqrt are within 2^-21 relative (22.10.6, 22.10.18, 22.10.19), log2 within 2^-21 absolute for
//   arguments in [0.5, 2] and relative elsewhere (22.10.8), sin and cos within 0.0008 inside 100 pi (22.10.20);
// - a dot product is no less accurate than its products summed one by one (3.1.3.2), which is held here as an error
//   of at most 2^-21 of the largest product (2^-9 for half). it is a fused operation, and where a step of it leaves
//   the type's range an implementation "may either saturate to the appropriate extreme representable value ... or
//   ... maintain extra intermediate precision (possibly arriving at an output result that falls back into correct,
//   representable form)" (3.1.3): both results are taken, the one of the steps in the type and the one of the
//   steps at more precision rounded once;
// - asin, acos and atan are within the bound of sin and cos. Direct3D's own bytecode has none of the three: FXC
//   writes them out in arithmetic with min and max, of which "If one source operand is NaN, then the other source
//   operand is returned" (22.10.10), so with that front end what they make of a NaN is not held;
// - tan is within that bound on its angle; float to half rounds toward zero and keeps what is past the range at
//   the largest half (3.2.2), half to float is exact;
// - a float converted to an integer is rounded toward zero, clamped to the integer's range, and 0 for NaN (22.13.3
//   ftoi, 22.13.4 ftou). DXIL leaves a value outside the range undefined, so for DXIL, and for double, only values
//   inside the range are held to it.
// a type the device reports as unsupported (double, 64-bit and 16-bit operations) is named and not run.
#include "d3d12_test.hpp"
#undef min
#undef max
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <type_traits>
#include <vector>

#pragma STDC FP_CONTRACT OFF

enum Tolerance { same, zero, ulp, fused, relative, logarithm, sine, converted, products, arc, tangent };

// (the result's type, the expression, how close the result has to be): text for HLSL and code for C++. T is the
// type under test, U the unsigned type of its width, and a, b, c are T
#define INTEGER(X)                                                                                                     \
  X(T, a + b, same) X(T, a - b, same) X(T, a * b, same) X(T, (a | 1) / (b | 1), same) X(T, (a | 1) % (b | 1), same)    \
  X(T, a & b, same) X(T, a | b, same) X(T, a ^ b, same) X(T, ~a, same) X(T, -a, same) X(T, a << c, same)               \
  X(T, a >> c, same) X(bool, a < b, same) X(bool, a <= b, same) X(bool, a > b, same) X(bool, a >= b, same)             \
  X(bool, a == b, same) X(bool, a != b, same) X(bool, !a, same) X(bool, a && b, same) X(bool, a || b, same)            \
  X(T, a ? b : c, same) X(T, min(a, b), same) X(T, max(a, b), same) X(T, clamp(a, min(b, c), max(b, c)), same)         \
  X(T, abs(a), same) X(T, mad(a, b, c), same) X(uint, countbits((U)a), same) X(U, reversebits((U)a), same)             \
  X(uint, firstbitlow((U)a), same) X(float, (float)a, same)
// firstbithigh counts from the low end of a 32-bit value; compilers disagree about the other widths
#define INTEGER32(X) X(uint, firstbithigh(a), same)
#define FLOATING(X)                                                                                                    \
  X(T, a + b, same) X(T, a - b, same) X(T, a * b, same) X(T, a / b, ulp) X(T, -a, same) X(T, abs(a), same)             \
  X(bool, a < b, same) X(bool, a <= b, same) X(bool, a > b, same) X(bool, a >= b, same) X(bool, a == b, same)          \
  X(bool, a != b, same) X(T, a < b ? b : c, same) X(T, min(a, b), zero) X(T, max(a, b), zero) X(T, saturate(a), zero)  \
  X(T, mad(a, b, c), fused) X(float, (float)a, same) X(int, (int)a, converted) X(uint, (uint)a, converted)
// what HLSL has for half and float only
#define REAL(X)                                                                                                        \
  X(T, floor(a), same) X(T, ceil(a), same) X(T, round(a), same) X(T, trunc(a), same) X(T, frac(a), ulp)                \
  X(T, sqrt(a), ulp) X(T, rsqrt(a), relative) X(T, rcp(a), relative) X(T, exp2(a), relative) X(T, log2(a), logarithm)  \
  X(T, sin(a), sine) X(T, cos(a), sine) X(bool, isnan(a), same) X(bool, isinf(a), same)                            \
  X(bool, isfinite(a), same) X(int, sign(a), same)                                                                    \
  X(T, dot(T2(a, b), T2(b, c)), products) X(T, dot(T3(a, b, c), T3(c, a, b)), products)                                \
  X(T, dot(T4(a, b, c, a), T4(b, c, a, c)), products) X(T, tan(a), tangent) X(T, asin(a), arc) X(T, acos(a), arc)      \
  X(T, atan(a), arc)
// float alone: its conversions to and from the bits of a half
#define FLOAT32(X) X(uint, f32tof16(a == a ? a : T(1)), same) X(float, f16tof32(asuint(a)), same)

struct Case {
  const char *type, *expression;
  Tolerance tolerance;
};
#define TEXT(R, e, t) {#R, #e, t},
#define CODE(R, e, t) [](T a, T b, T c) -> uint64_t { return raw(R(e)); },

// Direct3D's numbers in C++
namespace hlsl {
using uint = uint32_t;
static bool fuses; // mad as one exactly rounded operation, or as two

template <class V> constexpr bool real = std::is_floating_point_v<V> || std::is_same_v<V, _Float16>;
template <class X> concept number = std::is_arithmetic_v<X> || std::is_same_v<X, _Float16>;

// the type an operation is exact in: 64 bits that wrap, the float itself, or double for a half
template <class V>
auto
wide(V v) {
  if constexpr (std::is_same_v<V, _Float16>)
    return double(v);
  else if constexpr (real<V>)
    return v;
  else
    return uint64_t(v);
}

template <class X, class V>
X
convert(V v) {
  if constexpr (std::is_same_v<X, bool>) {
    return v != 0;
  } else if constexpr (real<V> && std::is_integral_v<X>) {
    double d = v, low = std::numeric_limits<X>::min(), high = std::numeric_limits<X>::max();
    return d != d ? 0 : d <= low ? std::numeric_limits<X>::min() : d >= high ? std::numeric_limits<X>::max() : X(d);
  } else {
    return X(v);
  }
}

template <class V> struct N {
  V v;
  N() = default;
  template <number X> N(X x) : v(V(x)) {}
  template <class X> explicit N(N<X> x) : v(convert<V>(x.v)) {}
  template <number X> explicit operator X() const { return convert<X>(v); }
#define WRAPS(op)                                                                                                      \
  friend N operator op(N a, N b) { return V(wide(a.v) op wide(b.v)); }
  WRAPS(+) WRAPS(-) WRAPS(*) WRAPS(&) WRAPS(|) WRAPS(^)
#define COMPARES(op)                                                                                                   \
  friend bool operator op(N a, N b) { return a.v op b.v; }
  COMPARES(<) COMPARES(<=) COMPARES(>) COMPARES(>=) COMPARES(==) COMPARES(!=)
  friend N operator/(N a, N b) {
    if constexpr (real<V>)
      return V(wide(a.v) / wide(b.v));
    else
      return V(a.v / b.v);
  }
  friend N operator%(N a, N b) { return V(a.v % b.v); }
  friend N operator<<(N a, N b) { return V(wide(a.v) << (wide(b.v) & (8 * sizeof(V) - 1))); }
  friend N operator>>(N a, N b) { return V(a.v >> (wide(b.v) & (8 * sizeof(V) - 1))); }
  friend N operator~(N a) { return V(~wide(a.v)); }
  friend N operator-(N a) {
    if constexpr (real<V>)
      return V(-a.v);
    else
      return V(0 - wide(a.v));
  }
};

template <class X>
uint64_t
raw(X x) {
  uint64_t bits = 0;
  memcpy(&bits, &x, sizeof(x));
  return bits;
}

// the operand that is not NaN; of two zeros, the second
template <class V> N<V> min(N<V> a, N<V> b) { return b.v != b.v ? a : a.v != a.v ? b : a.v < b.v ? a : b; }
template <class V> N<V> max(N<V> a, N<V> b) { return b.v != b.v ? a : a.v != a.v ? b : a.v > b.v ? a : b; }
template <class V> N<V> clamp(N<V> x, N<V> low, N<V> high) { return min(max(x, low), high); }
template <class V> N<V> abs(N<V> a) { return a.v < 0 ? -a : a.v == 0 ? N<V>(V(0)) : a; }
template <class V> N<V> saturate(N<V> a) { return min(max(a, N<V>(0)), N<V>(1)); }
template <class V>
N<V>
mad(N<V> a, N<V> b, N<V> c) {
  if constexpr (real<V>)
    if (fuses)
      return V(std::fma(wide(a.v), wide(b.v), wide(c.v)));
  return a * b + c;
}
#define THROUGH(name, expression)                                                                                      \
  template <class V> N<V> name(N<V> a) {                                                                               \
    auto x = wide(a.v);                                                                                                \
    return V(expression);                                                                                              \
  }
THROUGH(floor, std::floor(x)) THROUGH(ceil, std::ceil(x)) THROUGH(trunc, std::trunc(x)) THROUGH(round, std::nearbyint(x))
THROUGH(sqrt, std::sqrt(x)) THROUGH(rsqrt, 1 / std::sqrt(double(x))) THROUGH(rcp, 1 / double(x))
THROUGH(exp2, std::exp2(double(x))) THROUGH(log2, std::log2(double(x))) THROUGH(sin, std::sin(double(x)))
THROUGH(cos, std::cos(double(x))) THROUGH(tan, std::tan(double(x))) THROUGH(asin, std::asin(double(x)))
THROUGH(acos, std::acos(double(x))) THROUGH(atan, std::atan(double(x)))
// a vector of n, for the operations that take one
template <class V, int n> struct Vec {
  N<V> e[n];
  template <class... A> Vec(A... a) : e{N<V>(a)...} {}
};
static bool keeps; // a dot product's steps at more precision than its type, rounded once at the end
template <class V, int n>
N<V>
dot(Vec<V, n> a, Vec<V, n> b) {
  if (keeps) {
    double wide = 0;
    for (int i = 0; i < n; i++)
      wide += double(a.e[i].v) * double(b.e[i].v);
    return N<V>(V(wide));
  }
  N<V> sum = a.e[0] * b.e[0];
  for (int i = 1; i < n; i++)
    sum = sum + a.e[i] * b.e[i];
  return sum;
}
inline uint asuint(N<float> a) { return std::bit_cast<uint>(a.v); }
inline float f16tof32(uint bits) { return float(std::bit_cast<_Float16>(uint16_t(bits))); }
// D3D11.3 3.2.2: float to half rounds toward zero; past the half range, the largest finite half; infinity stays
inline uint
f32tof16(N<float> a) {
  uint bits = std::bit_cast<uint>(a.v), sign = bits >> 16 & 0x8000, magnitude = bits & 0x7fffffff;
  int exponent = int(magnitude >> 23) - 127 + 15;
  if (magnitude == 0x7f800000)
    return sign | 0x7c00;
  if (exponent >= 31)
    return sign | 0x7bff;
  if (exponent > 0)
    return sign | exponent << 10 | (magnitude >> 13 & 0x3ff);
  // a half denormal keeps the mantissa bits that fit
  return exponent < -10 ? sign : sign | ((magnitude & 0x7fffff) | 0x800000) >> (14 - exponent);
}
template <class V> N<V> frac(N<V> a) { return a - floor(a); }
template <class V> bool isnan(N<V> a) { return a.v != a.v; }
template <class V> bool isinf(N<V> a) { return a.v == a.v && (a - a).v != 0; }
template <class V> bool isfinite(N<V> a) { return (a - a).v == 0; }
template <class V> int sign(N<V> a) { return (0 < a.v) - (a.v < 0); }
template <class V> uint countbits(N<V> a) { return std::popcount(a.v); }
template <class V> uint firstbitlow(N<V> a) { return a.v ? std::countr_zero(a.v) : ~0u; }
template <class V>
N<V>
reversebits(N<V> a) {
  V reversed = 0;
  for (unsigned bit = 0; bit < 8 * sizeof(V); bit++)
    reversed |= V(V(a.v >> bit & 1) << (8 * sizeof(V) - 1 - bit));
  return reversed;
}
// the first bit from the top that differs from the sign, counted from the low end (HLSL firstbithigh)
template <class V>
uint
firstbithigh(N<V> a) {
  auto bits = uint32_t(a.v < 0 ? ~a.v : a.v);
  return bits ? 31 - std::countl_zero(bits) : ~0u;
}

template <class V>
std::vector<uint64_t (*)(N<V>, N<V>, N<V>)>
references() {
  using T [[maybe_unused]] = N<V>;
  using T2 [[maybe_unused]] = Vec<V, 2>;
  using T3 [[maybe_unused]] = Vec<V, 3>;
  using T4 [[maybe_unused]] = Vec<V, 4>;
  using U [[maybe_unused]] = N<typename std::conditional_t<real<V>, std::type_identity<V>, std::make_unsigned<V>>::type>;
  if constexpr (real<V> && sizeof(V) == 8)
    return {FLOATING(CODE)};
  else if constexpr (real<V> && sizeof(V) == 4)
    return {FLOATING(CODE) REAL(CODE) FLOAT32(CODE)};
  else if constexpr (real<V>)
    return {FLOATING(CODE) REAL(CODE)};
  else if constexpr (sizeof(V) == 4)
    return {INTEGER(CODE) INTEGER32(CODE)};
  else
    return {INTEGER(CODE)};
}
} // namespace hlsl

// the values a type's operations are given: the ends of its range, single bits, zeros, infinities, NaN, and values
// between; no float denorms, which the rules let an operation read as zero
template <class V>
std::vector<uint64_t>
values() {
  std::vector<V> list;
  if constexpr (hlsl::real<V>) {
    for (double d : {0.0, 1.0, 0.5, 1.5, 2.5, 3.25, 0.1, 1.0 / 3, 7.0, 100.5, 1e-3, 1024.0, 65504.0, 3.0e4, 2147483520.0,
                     4294967296.0, 1e-4, 0.75, 0.999, 1.001, 2.0, 6.2831853, 0.6931472, 40.0, 0.0625, 12345.678})
      list.insert(list.end(), {V(d), V(-d)});
    double infinity = std::numeric_limits<double>::infinity();
    list.insert(list.end(), {V(infinity), V(-infinity), V(infinity - infinity)});
  } else {
    for (unsigned bit : {0u, 1u, 2u, unsigned(4 * sizeof(V) - 1), unsigned(4 * sizeof(V)), unsigned(8 * sizeof(V) - 2),
                         unsigned(8 * sizeof(V) - 1)})
      list.insert(list.end(), {V(uint64_t(1) << bit), V(~(uint64_t(1) << bit)), V((uint64_t(1) << bit) - 1)});
    for (uint64_t random = 1, i = 0; i < 24; i++)
      list.push_back(V((random = random * 6364136223846793005u + 1442695040888963407u) >> (i % 3 * 16)));
    list.insert(list.end(), {V(0), V(3), V(5), V(10), V(100), V(-100), V(0x5555555555555555u), V(0xaaaaaaaaaaaaaaaau)});
  }
  std::vector<uint64_t> bits;
  for (V value : list)
    bits.push_back(hlsl::raw(value));
  return bits;
}

struct Type {
  const char *name, *unsigned_name;
  bool real;
  unsigned width;
  // the front ends that have it, the flags it needs, and whether the device has it
  bool dxbc;
  const wchar_t *flag;
  BOOL supported;
  std::vector<Case> cases;
  std::vector<uint64_t> values;
  std::function<uint64_t(unsigned, const uint64_t *)> reference;
};

template <class V>
Type
type(const char *name, const char *unsigned_name, bool dxbc, const wchar_t *flag, BOOL supported) {
  bool real = hlsl::real<V>;
  std::vector<Case> cases = real && sizeof(V) == 8 ? std::vector<Case>{FLOATING(TEXT)}
                            : real && sizeof(V) == 4 ? std::vector<Case>{FLOATING(TEXT) REAL(TEXT) FLOAT32(TEXT)}
                            : real                 ? std::vector<Case>{FLOATING(TEXT) REAL(TEXT)}
                            : sizeof(V) == 4       ? std::vector<Case>{INTEGER(TEXT) INTEGER32(TEXT)}
                                                   : std::vector<Case>{INTEGER(TEXT)};
  return {name, unsigned_name, real, unsigned(8 * sizeof(V)), dxbc, flag, supported, cases, values<V>(),
          [references = hlsl::references<V>()](unsigned which, const uint64_t *in) {
            hlsl::N<V> operands[3];
            for (unsigned i = 0; i < 3; i++)
              memcpy(&operands[i], &in[i], sizeof(V));
            return references[which](operands[0], operands[1], operands[2]);
          }};
}

static double
decoded(uint64_t bits, unsigned width) {
  return width == 16   ? (double)std::bit_cast<_Float16>(uint16_t(bits))
         : width == 32 ? (double)std::bit_cast<float>(uint32_t(bits))
                       : std::bit_cast<double>(bits);
}

// whether a result that is not the reference's bits is one the rules allow
static bool
allowed(const Type &type, const Case &c, uint64_t got, uint64_t want, uint64_t unfused, uint64_t kept, const uint64_t *in, bool dxbc) {
  double a = decoded(in[0], type.width);
  if (c.tolerance == arc && dxbc && a != a)
    return true;
  if (c.tolerance == converted) {
    bool is_signed = !strcmp(c.type, "int");
    bool outside = a != a || a >= (is_signed ? 0x1p31 : 0x1p32) || a <= (is_signed ? -0x1p31 - 1 : -1.0);
    return outside && !(dxbc && type.width == 32);
  }
  // a result is a float when the case says so, or when it is of the type under test and that is one
  const unsigned width = strcmp(c.type, "float") ? type.width : 32;
  if (strcmp(c.type, "float") && (!type.real || strcmp(c.type, "T")))
    return false;
  double g = decoded(got, width), w = decoded(want, width), k = decoded(kept, width);
  // a dot product that kept precision where its steps left the range: as near to that result as any dot product
  // has to be to its own
  double largest = 0;
  for (unsigned i = 0; i < 3; i++)
    largest = std::max(largest, std::abs(decoded(in[i], type.width)));
  const double product_error = std::ldexp(largest * largest, type.width == 16 ? -9 : -21);
  if (c.tolerance == products && k == k && g == g && (got == kept || std::abs(g - k) <= product_error))
    return true;
  // where the steps in the type give infinities of both signs from operands that are numbers, each step may have
  // saturated or kept its precision (3.1.3): an infinity of either sign is among what that gives
  if (c.tolerance == products && w != w && std::isfinite(largest) && std::isinf(g))
    return true;
  if (w != w || g != g)
    return w != w && g != g;
  uint64_t sign = uint64_t(1) << (width - 1);
  auto ordered = [&](uint64_t bits) { return bits & sign ? -int64_t(bits & (sign - 1)) : int64_t(bits); };
  int64_t apart = std::abs(ordered(got) - ordered(want));
  if (width == 32 && std::fpclassify(float(w)) == FP_SUBNORMAL && g == 0 && std::signbit(g) == std::signbit(w))
    return true;
  switch (c.tolerance) {
  case zero:
    return g == 0 && w == 0;
  case ulp:
    return apart <= 1;
  case fused:
    return std::abs(g - w) <= std::abs(decoded(unfused, width) - w);
  case relative:
    return apart <= 1 || std::abs(g - w) <= std::ldexp(std::abs(w), -21);
  case logarithm:
    return apart <= 1 || std::abs(g - w) <= std::ldexp(a >= 0.5 && a <= 2 ? 1 : std::abs(w), -21);
  case sine:
    return std::abs(a) > 100 * 3.14159265358979 || std::abs(g - w) <= 0.0008;
  case arc:
    return apart <= 1 || std::abs(g - w) <= 0.0008;
  case tangent:
    return std::abs(a) > 100 * 3.14159265358979 || apart <= 1 || std::abs(g - w) <= 0.0008 * (1 + w * w);
  case products:
    return !std::isfinite(largest * largest) || std::abs(g - w) <= product_error;
  default:
    return false;
  }
}

static const char hlsl_source[] = R"hlsl(
StructuredBuffer<uint2> input : register(t0);
RWStructuredBuffer<uint2> output : register(u0);
#define T2 vector<T, 2>
#define T3 vector<T, 3>
#define T4 vector<T, 4>
// T from 64 bits, and a result's bits
#if REAL && WIDTH == 64
T value(uint2 v) { return asdouble(v.x, v.y); }
uint2 bits(double r) { uint2 b; asuint(r, b.x, b.y); return b; }
#elif REAL && WIDTH == 16
T value(uint2 v) { return asfloat16(uint16_t(v.x)); }
uint2 bits(float16_t r) { return uint2(asuint16(r), 0); }
#elif REAL
T value(uint2 v) { return asfloat(v.x); }
#elif WIDTH == 64
T value(uint2 v) { return (T)(uint64_t(v.x) | uint64_t(v.y) << 32); }
uint2 bits(uint64_t r) { return uint2(uint(r), uint(r >> 32)); }
uint2 bits(int64_t r) { return bits(uint64_t(r)); }
#elif WIDTH == 16
T value(uint2 v) { return (T)v.x; }
uint2 bits(uint16_t r) { return uint2(r, 0); }
uint2 bits(int16_t r) { return bits(uint16_t(r)); }
#else
T value(uint2 v) { return (T)v.x; }
#endif
uint2 bits(bool r) { return uint2(r ? 1 : 0, 0); }
uint2 bits(uint r) { return uint2(r, 0); }
uint2 bits(int r) { return uint2(uint(r), 0); }
uint2 bits(float r) { return uint2(asuint(r), 0); }

// result `id`: case id % CASES of the values id / CASES
uint2 run(uint id) {
  uint at = id / CASES * 3;
  T a = value(input[at]), b = value(input[at + 1]), c = value(input[at + 2]);
  switch (id % CASES) { SWITCH }
  return 0;
}

[numthreads(64, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {
  if (id.x < CASES * VALUES)
    output[id.x] = run(id.x);
}

// a result is a pixel: its case across, its values down
struct V { float4 pos : SV_Position; nointerpolation uint2 r : R; nointerpolation uint id : ID; };
V at(uint id, bool here) {
  V v;
  v.pos = float4((id % CASES + 0.5) * 2 / CASES - 1, 1 - (id / CASES + 0.5) * 2 / VALUES, 0, 1);
  v.id = id;
  v.r = 0;
  if (here)
    v.r = run(id);
  return v;
}
V vs_runs(uint id : SV_VertexID) { return at(id, true); }
V vs_passes(uint id : SV_VertexID) { return at(id, false); }
float4 vs_fills(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1); }
uint2 ps_runs(float4 pos : SV_Position) : SV_Target { return run(uint(pos.y) * CASES + uint(pos.x)); }
uint2 ps_passes(V v) : SV_Target { return v.r; }
[maxvertexcount(1)] void gs_runs(point V v[1], inout PointStream<V> stream) { stream.Append(at(v[0].id, true)); }
// a patch of one control point is a line of one segment, drawn as its two ends, which are one pixel
struct PC { float edges[2] : SV_TessFactor; uint2 r : R; };
PC pc_runs(InputPatch<V, 1> points) { PC pc; pc.edges[0] = pc.edges[1] = 1; pc.r = run(points[0].id); return pc; }
PC pc_passes(InputPatch<V, 1> points) { PC pc; pc.edges[0] = pc.edges[1] = 1; pc.r = 0; return pc; }
[domain("isoline")] [partitioning("integer")] [outputtopology("point")] [outputcontrolpoints(1)]
[patchconstantfunc("pc_runs")]
V hs_runs(InputPatch<V, 1> points) { return points[0]; }
[domain("isoline")] [partitioning("integer")] [outputtopology("point")] [outputcontrolpoints(1)]
[patchconstantfunc("pc_passes")]
V hs_passes(InputPatch<V, 1> points) { return points[0]; }
[domain("isoline")] V ds_runs(PC pc, float2 where : SV_DomainLocation, const OutputPatch<V, 1> points) {
  return at(points[0].id, true);
}
[domain("isoline")] V ds_passes(PC pc, float2 where : SV_DomainLocation, const OutputPatch<V, 1> points) {
  V v = points[0];
  v.r = pc.r;
  return v;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const bool dxbc = !compiler.dxc;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4)));
  const wchar_t *narrow = L"-enable-16bit-types";
  const Type types[] = {
      type<int32_t>("int", "uint", true, nullptr, TRUE),
      type<uint32_t>("uint", "uint", true, nullptr, TRUE),
      type<float>("float", "float", true, nullptr, TRUE),
      type<double>("double", "double", true, nullptr, options.DoublePrecisionFloatShaderOps),
      type<int64_t>("int64_t", "uint64_t", false, nullptr, options1.Int64ShaderOps),
      type<uint64_t>("uint64_t", "uint64_t", false, nullptr, options1.Int64ShaderOps),
      type<int16_t>("int16_t", "uint16_t", false, narrow, options4.Native16BitShaderOpsSupported),
      type<uint16_t>("uint16_t", "uint16_t", false, narrow, options4.Native16BitShaderOpsSupported),
      type<_Float16>("float16_t", "float16_t", false, narrow, options4.Native16BitShaderOpsSupported),
  };

  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  auto rs = root_signature(device.Get(), {(UINT)std::size(params), params});
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();

  std::string not_run;
  unsigned results = 0;
  for (auto &type : types) {
    if (dxbc && !type.dxbc)
      continue;
    if (!type.supported) {
      not_run += std::string(not_run.empty() ? "" : ", ") + type.name;
      continue;
    }
    const UINT cases = type.cases.size(), count = type.values.size(), total = cases * count;
    // value triple i is a value, one seven places on and one eleven places on; every eighth has the first twice
    std::vector<uint64_t> triples;
    for (UINT i = 0; i < count; i++)
      triples.insert(triples.end(), {type.values[i], type.values[i % 8 ? (i * 7 + 3) % count : i], type.values[(i * 11 + 5) % count]});
    std::string body;
    for (UINT i = 0; i < cases; i++)
      body += "case " + std::to_string(i) + ": { precise " + type.cases[i].type + " r = (" + type.cases[i].type + ")(" +
              type.cases[i].expression + "); return bits(r); } ";
    const std::vector<std::string> defines = {
        std::string("T=") + type.name, std::string("U=") + type.unsigned_name, "REAL=" + std::to_string(type.real),
        "WIDTH=" + std::to_string(type.width), "CASES=" + std::to_string(cases), "VALUES=" + std::to_string(count),
        "SWITCH=" + body};
    auto compiled = [&](const char *entry) {
      std::string stage(entry, 2);
      return compiler.compile(hlsl_source, entry, type.flag ? (stage + "_6_2").c_str() : stage.c_str(), defines,
                              type.flag ? std::vector<std::wstring>{type.flag} : std::vector<std::wstring>{});
    };

    auto input = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, triples.size() * 8, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto computed = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, total * 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    void *mapped;
    CHECK(input->Map(0, nullptr, &mapped));
    memcpy(mapped, triples.data(), triples.size() * 8);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const DXGI_FORMAT format = DXGI_FORMAT_R32G32_UINT;
    D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, cases, count, 1, 1, format, {1, 0},
                                    D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> target;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr, IID_PPV_ARGS(&target)));
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 bytes;
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, std::max<UINT64>(bytes, total * 8), D3D12_RESOURCE_STATE_COPY_DEST);

    // a stage's pipeline: the shader that runs the operations, and the ones that carry its results to the target
    struct Stage {
      const char *name, *vs, *hs, *ds, *gs, *ps;
      D3D12_PRIMITIVE_TOPOLOGY_TYPE kind;
      D3D_PRIMITIVE_TOPOLOGY topology;
    };
    const Stage stages[] = {
        {"compute"},
        {"pixel", "vs_fills", nullptr, nullptr, nullptr, "ps_runs", D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST},
        {"vertex", "vs_runs", nullptr, nullptr, nullptr, "ps_passes", D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, D3D_PRIMITIVE_TOPOLOGY_POINTLIST},
        {"geometry", "vs_passes", nullptr, nullptr, "gs_runs", "ps_passes", D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, D3D_PRIMITIVE_TOPOLOGY_POINTLIST},
        {"hull", "vs_passes", "hs_runs", "ds_passes", nullptr, "ps_passes", D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH, D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST},
        {"domain", "vs_passes", "hs_passes", "ds_runs", nullptr, "ps_passes", D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH, D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST},
    };
    for (auto &stage : stages) {
      step("the operations of %s in the %s shader", type.name, stage.name);
      ComPtr<ID3D12PipelineState> pso;
      std::string code[5];
      const char *entries[5] = {stage.vs ? stage.vs : "cs", stage.hs, stage.ds, stage.gs, stage.ps};
      bool compiles = true;
      for (UINT i = 0; i < 5; i++)
        if (entries[i] && (code[i] = compiled(entries[i])).empty())
          compiles = false;
      if (!expect(compiles, "the HLSL did not compile"))
        continue;
      HRESULT made;
      if (stage.vs) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rs.Get();
        // a stage the pipeline does not have has no bytecode at all
        auto stage_code = [&](UINT i) { return entries[i] ? bytecode(code[i]) : D3D12_SHADER_BYTECODE{}; };
        desc.VS = stage_code(0), desc.HS = stage_code(1), desc.DS = stage_code(2), desc.GS = stage_code(3), desc.PS = stage_code(4);
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = ~0u;
        desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
        desc.PrimitiveTopologyType = stage.kind;
        desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
        desc.SampleDesc = {1, 0};
        made = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
      } else {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(code[0])};
        made = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
      }
      if (!expect(made == S_OK, "the pipeline: %08lx", made))
        continue;

      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      if (stage.vs) {
        const float clear[4] = {};
        D3D12_VIEWPORT viewport{0, 0, (float)cases, (float)count, 0, 1};
        D3D12_RECT scissor{0, 0, (LONG)cases, (LONG)count};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->SetGraphicsRootSignature(rs.Get());
        list->SetGraphicsRootShaderResourceView(0, input->GetGPUVirtualAddress());
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(stage.topology);
        list->DrawInstanced(stage.kind == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ? 3 : total, 1, 0, 0);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      } else {
        list->SetComputeRootSignature(rs.Get());
        list->SetComputeRootShaderResourceView(0, input->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1, computed->GetGPUVirtualAddress());
        list->Dispatch((total + 63) / 64, 1, 1);
        transition(list.Get(), computed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, computed.Get(), 0, total * 8);
        transition(list.Get(), computed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
      CHECK(submit(device.Get(), queue.Get(), list.Get()));

      const char *out;
      CHECK(readback->Map(0, nullptr, (void **)&out));
      unsigned wrong = 0;
      for (UINT i = 0; i < count; i++)
        for (UINT which = 0; which < cases; which++, results++) {
          auto &c = type.cases[which];
          uint64_t got;
          memcpy(&got, out + (stage.vs ? i * footprint.Footprint.RowPitch + which * 8 : (i * cases + which) * 8), 8);
          const uint64_t *in = &triples[3 * i];
          hlsl::fuses = false;
          uint64_t unfused = type.reference(which, in);
          hlsl::fuses = true;
          uint64_t want = type.reference(which, in);
          hlsl::keeps = true;
          uint64_t kept = type.reference(which, in);
          hlsl::keeps = false;
          if (got != want && !allowed(type, c, got, want, unfused, kept, in, dxbc) && wrong++ < 4)
            expect(false, "(%s)(%s) of a = %#llx, b = %#llx, c = %#llx is %#llx, want %#llx", c.type, c.expression, in[0],
                   in[1], in[2], got, want);
        }
      readback->Unmap(0, nullptr);
      expect(wrong <= 4, "and %u more results", wrong - 4);
    }
  }
  if (!not_run.empty())
    printf("not run, the device reports no support: %s\n", not_run.c_str());
  printf("%u results set against the rules\n", results);
  return verdict();
}
