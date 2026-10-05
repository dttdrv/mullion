// Direct3D 11's reference fixed function tessellator, which the Direct3D 11.3 functional specification (11.7.1) names
// as the concrete description of the tessellator, behind an interface without its names, which the Windows headers
// also define. tessellator.hpp and tessellator.cpp are Microsoft's files, unchanged, from
// https://microsoft.github.io/DirectX-Specs/d3d/archive/images/d3d11/ under the MIT license in LICENSE
#pragma once
#include <vector>

namespace reference {

enum Domain { isoline, triangle, quad };
// in the order of the reference's D3D11_TESSELLATOR_PARTITIONING
enum Partitioning { integer, pow2, fractional_odd, fractional_even };

struct Point {
  float u, v;
};

// the domain points of a patch, each once. `factors`: SV_TessFactor's values, then SV_InsideTessFactor's
std::vector<Point> points(Domain domain, Partitioning partitioning, const float *factors);

// a patch's primitives of `vertices` each, one after another: its lines (2), or its triangles, clockwise (3)
std::vector<Point> corners(Domain domain, Partitioning partitioning, const float *factors, int vertices);

} // namespace reference
