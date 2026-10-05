// the reference defines fmin and fmax, which <math.h> declares
#include <math.h>
#define fmin reference_fmin
#define fmax reference_fmax
#include "tessellator.cpp"
#include "reference.hpp"

namespace reference {

static void
tessellate(CHWTessellator &tessellator, Domain domain, Partitioning partitioning, const float *f, D3D11_TESSELLATOR_OUTPUT_PRIMITIVE output) {
  tessellator.Init((D3D11_TESSELLATOR_PARTITIONING)partitioning, output);
  if (domain == isoline)
    tessellator.TessellateIsoLineDomain(f[0], f[1]);
  else if (domain == triangle)
    tessellator.TessellateTriDomain(f[0], f[1], f[2], f[3]);
  else
    tessellator.TessellateQuadDomain(f[0], f[1], f[2], f[3], f[4], f[5]);
}

std::vector<Point>
points(Domain domain, Partitioning partitioning, const float *f) {
  CHWTessellator tessellator;
  tessellate(tessellator, domain, partitioning, f, D3D11_TESSELLATOR_OUTPUT_POINT);
  std::vector<Point> out;
  for (int i = 0; i < tessellator.GetPointCount(); i++)
    out.push_back({tessellator.GetPoints()[i].u, tessellator.GetPoints()[i].v});
  return out;
}

std::vector<Point>
corners(Domain domain, Partitioning partitioning, const float *f, int vertices) {
  CHWTessellator tessellator;
  tessellate(tessellator, domain, partitioning, f, vertices == 2 ? D3D11_TESSELLATOR_OUTPUT_LINE : D3D11_TESSELLATOR_OUTPUT_TRIANGLE_CW);
  std::vector<Point> out;
  for (int i = 0; i < tessellator.GetIndexCount(); i++) {
    auto &p = tessellator.GetPoints()[tessellator.GetIndices()[i]];
    out.push_back({p.u, p.v});
  }
  return out;
}

} // namespace reference
