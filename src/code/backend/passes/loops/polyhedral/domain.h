/* Affine extrema on the integer domain rather than its enclosing box.
 * PET's value-bound refinement motivates sharing this query between
 * extraction, dependence screening and emitted pointer guards. */
#ifndef NYR_POLY_DOMAIN_H
#define NYR_POLY_DOMAIN_H
#include "code/backend/passes/loops/polyhedral/poly.h"
#include "base/compat.h"

static inline bool nyr_poly_extrema(const nyr_poly_region_t *r,
    const ny_i128 coeff[NYR_POLY_MAX_DIMS], ny_i128 constant,
    ny_i128 *lo, ny_i128 *hi) {
  unsigned dims=nyr_poly_dims(r);
  if (dims<2 || dims>NYR_POLY_MAX_DIMS) return false;
  for (unsigned d=0;d<dims;++d)
    if (!r->extent[d]||r->extent[d]>NYR_POLY_MAX_EXTENT) return false;
  *lo=*hi=constant;
  if (r->triangle) {
    /* For j <= i-cut the vertices are (cut,0), (N-1,0),
     * (N-1,N-1-cut). Every integer point lies in their convex hull. */
    if (dims!=2||r->triangle>4||r->extent[0]!=r->extent[1]) return false;
    unsigned cut=r->triangle&1u, last=r->extent[0]-1;
    unsigned x=r->triangle>2?1:0,y=x^1u;
    if (last<cut) return false;
    ny_i128 v[]={constant+coeff[x]*cut,constant+coeff[x]*last,
      constant+coeff[x]*last+coeff[y]*(last-cut)};
    *lo=*hi=v[0];
    for (unsigned k=1;k<3;++k) {
      if (v[k]<*lo) *lo=v[k];
      if (v[k]>*hi) *hi=v[k];
    }
    return true;
  }
  for (unsigned d=0;d<dims;++d) {
    ny_i128 delta=coeff[d]*(r->extent[d]-1);
    if (delta<0) *lo+=delta;else *hi+=delta;
  }
  return true;
}

static inline bool nyr_poly_access_extrema(const nyr_poly_region_t *r,
    const nyr_poly_access_t *a,ny_i128 *lo,ny_i128 *hi) {
  ny_i128 coeff[NYR_POLY_MAX_DIMS];
  for (unsigned d=0;d<NYR_POLY_MAX_DIMS;++d) coeff[d]=a->offset.coeff[d];
  if (!a->width||!nyr_poly_extrema(r,coeff,a->offset.constant,lo,hi)) return false;
  *hi+=a->width-1;
  return true;
}
#endif
