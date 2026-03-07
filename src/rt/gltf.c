#include "rt/shared.h"
#include <math.h>
#include <string.h>

static inline float rt_gltf_f32_clean(float v) {
  return isfinite(v) ? v : 0.0f;
}

static inline float rt_gltf_f32_arg(int64_t v) {
  if (v & 1)
    return rt_gltf_f32_clean((float)(v >> 1));
  if (v == 0)
    return 0.0f;
  if (is_ptr(v) && (v & 15) == 8 && *(int64_t *)((char *)(uintptr_t)v - 8) == TAG_FLOAT) {
    double d;
    memcpy(&d, (const void *)(uintptr_t)v, 8);
    return rt_gltf_f32_clean((float)d);
  }
  if (!is_ptr(v)) {
    double d;
    memcpy(&d, &v, 8);
    return rt_gltf_f32_clean((float)d);
  }
  return 0.0f;
}

static inline int64_t rt_gltf_i64_arg(int64_t v) { return is_int(v) ? (v >> 1) : v; }

static inline float rt_gltf_f32_from_value(int64_t v, float fallback) {
  if (is_int(v))
    return (float)(v >> 1);
  if (is_v_flt(v)) {
    double d;
    memcpy(&d, (const void *)(uintptr_t)v, 8);
    return rt_gltf_f32_clean((float)d);
  }
  return fallback;
}

static inline int64_t rt_gltf_box_f32(float f) {
  double d = (double)rt_gltf_f32_clean(f);
  int64_t bits;
  memcpy(&bits, &d, 8);
  return rt_flt_box_val(bits);
}

static inline int64_t rt_gltf_list_get_raw(int64_t lst, int64_t idx) {
  return *(int64_t *)((char *)(uintptr_t)lst + 16 + idx * 8);
}

static inline void rt_gltf_list_set_raw(int64_t lst, int64_t idx, int64_t val) {
  *(int64_t *)((char *)(uintptr_t)lst + 16 + idx * 8) = val;
}

static inline int64_t rt_gltf_list_len_raw(int64_t lst) {
  if (!is_ptr(lst))
    return 0;
  return rt_gltf_i64_arg(*(int64_t *)(uintptr_t)lst);
}

static inline void rt_gltf_mat4_load_list(int64_t m_v, float *out) {
  if (!is_ptr(m_v)) {
    memset(out, 0, 16u * sizeof(float));
    out[0] = 1.0f;
    out[5] = 1.0f;
    out[10] = 1.0f;
    out[15] = 1.0f;
    return;
  }
  for (int i = 0; i < 16; i++) {
    const float fallback = (i == 0 || i == 5 || i == 10 || i == 15) ? 1.0f : 0.0f;
    out[i] = rt_gltf_f32_from_value(rt_gltf_list_get_raw(m_v, i), fallback);
  }
}

static int64_t rt_gltf_mat4_tag_str(void) {
  static int64_t tag = 0;
  if (!tag)
    tag = rt_alloc_string_len("mat4", 4);
  return tag;
}

static int64_t rt_gltf_mat4_make_list(const float *m) {
  int64_t out = rt_list_new(rt_tag_v(18));
  if (!out)
    return 0;
  *(int64_t *)((char *)(uintptr_t)out + 0) = rt_tag_v(18);
  for (int i = 0; i < 16; i++)
    rt_gltf_list_set_raw(out, i, rt_gltf_box_f32(m[i]));
  rt_gltf_list_set_raw(out, 16, rt_gltf_mat4_tag_str());
  rt_gltf_list_set_raw(out, 17, rt_tag_v(400));
  return out;
}

int64_t rt_gltf_mat4_mul_list(int64_t a_v, int64_t b_v) {
  if (!is_ptr(a_v) || !is_ptr(b_v))
    return 0;
  float A[16], B[16], O[16];
  rt_gltf_mat4_load_list(a_v, A);
  rt_gltf_mat4_load_list(b_v, B);
  _mat4_mul_simd(A, B, O);
  return rt_gltf_mat4_make_list(O);
}

int64_t rt_gltf_skin_mat_store_raw(int64_t slab_v, int64_t idx_v, int64_t joint_world_v,
                                   int64_t inv_bind_v, int64_t mesh_inv_v) {
  float *slab = (float *)(uintptr_t)slab_v;
  const int64_t idx = rt_gltf_i64_arg(idx_v);
  if (!slab || idx < 0)
    return 0;
  float joint_world[16], inv_bind[16], mesh_inv[16], tmp[16], out[16];
  rt_gltf_mat4_load_list(joint_world_v, joint_world);
  rt_gltf_mat4_load_list(inv_bind_v, inv_bind);
  rt_gltf_mat4_load_list(mesh_inv_v, mesh_inv);
  _mat4_mul_simd(joint_world, inv_bind, tmp);
  _mat4_mul_simd(mesh_inv, tmp, out);
  memcpy(slab + (size_t)idx * 16u, out, 16u * sizeof(float));
  return slab_v;
}

int64_t rt_gltf_skin_mats_store_raw(int64_t slab_v, int64_t joints_v, int64_t world_list_v,
                                    int64_t inv_bind_list_v, int64_t mesh_inv_v,
                                    int64_t count_v) {
  float *slab = (float *)(uintptr_t)slab_v;
  const int64_t count = rt_gltf_i64_arg(count_v);
  if (!slab || !is_ptr(joints_v) || !is_ptr(world_list_v) || !is_ptr(inv_bind_list_v) ||
      count <= 0)
    return 0;

  const int64_t joints_n = rt_gltf_list_len_raw(joints_v);
  const int64_t world_n = rt_gltf_list_len_raw(world_list_v);
  const int64_t inv_n = rt_gltf_list_len_raw(inv_bind_list_v);
  float mesh_inv[16];
  rt_gltf_mat4_load_list(mesh_inv_v, mesh_inv);

  for (int64_t ji = 0; ji < count; ji++) {
    int64_t joint_idx = -1;
    if (ji < joints_n)
      joint_idx = rt_gltf_i64_arg(rt_gltf_list_get_raw(joints_v, ji));
    int64_t joint_world_v = 0;
    if (joint_idx >= 0 && joint_idx < world_n)
      joint_world_v = rt_gltf_list_get_raw(world_list_v, joint_idx);
    int64_t inv_bind_v = ji < inv_n ? rt_gltf_list_get_raw(inv_bind_list_v, ji) : 0;

    float joint_world[16], inv_bind[16], tmp[16], out[16];
    rt_gltf_mat4_load_list(joint_world_v, joint_world);
    rt_gltf_mat4_load_list(inv_bind_v, inv_bind);
    _mat4_mul_simd(joint_world, inv_bind, tmp);
    _mat4_mul_simd(mesh_inv, tmp, out);
    memcpy(slab + (size_t)ji * 16u, out, 16u * sizeof(float));
  }
  return slab_v;
}

static int64_t rt_gltf_anim_make_vec3(float x, float y, float z) {
  int64_t out = rt_list_new(rt_tag_v(3));
  if (!out)
    return 0;
  *(int64_t *)((char *)(uintptr_t)out + 0) = rt_tag_v(3);
  rt_gltf_list_set_raw(out, 0, rt_gltf_box_f32(x));
  rt_gltf_list_set_raw(out, 1, rt_gltf_box_f32(y));
  rt_gltf_list_set_raw(out, 2, rt_gltf_box_f32(z));
  return out;
}

static int64_t rt_gltf_anim_make_vec4(float x, float y, float z, float w) {
  int64_t out = rt_list_new(rt_tag_v(4));
  if (!out)
    return 0;
  *(int64_t *)((char *)(uintptr_t)out + 0) = rt_tag_v(4);
  rt_gltf_list_set_raw(out, 0, rt_gltf_box_f32(x));
  rt_gltf_list_set_raw(out, 1, rt_gltf_box_f32(y));
  rt_gltf_list_set_raw(out, 2, rt_gltf_box_f32(z));
  rt_gltf_list_set_raw(out, 3, rt_gltf_box_f32(w));
  return out;
}

int64_t rt_gltf_anim_fast_value_raw(int64_t rec_v, int64_t time_v) {
  if (!is_ptr(rec_v))
    return 0;
  const float time_sec = rt_gltf_f32_arg(time_v);
  const int64_t in_ptr_v = rt_gltf_list_get_raw(rec_v, 2);
  const int64_t out_ptr_v = rt_gltf_list_get_raw(rec_v, 3);
  const unsigned char *in_ptr = (const unsigned char *)(uintptr_t)in_ptr_v;
  const unsigned char *out_ptr = (const unsigned char *)(uintptr_t)out_ptr_v;
  const int64_t count = rt_gltf_i64_arg(rt_gltf_list_get_raw(rec_v, 4));
  const int64_t in_stride = rt_gltf_i64_arg(rt_gltf_list_get_raw(rec_v, 5));
  const int64_t out_stride = rt_gltf_i64_arg(rt_gltf_list_get_raw(rec_v, 6));
  const int64_t n_comp = rt_gltf_i64_arg(rt_gltf_list_get_raw(rec_v, 7));
  if (!in_ptr || !out_ptr || count <= 0 || in_stride <= 0 || out_stride <= 0)
    return 0;

  int64_t lo = 0;
  int64_t hi = 0;
  float alpha = 0.0f;
  if (count > 1) {
    const float first_t = *(const float *)(const void *)in_ptr;
    const float last_t = *(const float *)(const void *)(in_ptr + (size_t)(count - 1) * in_stride);
    if (time_sec <= first_t) {
      lo = 0;
      hi = 0;
    } else if (time_sec >= last_t) {
      lo = count - 1;
      hi = count - 1;
    } else {
      lo = rt_gltf_i64_arg(rt_gltf_list_get_raw(rec_v, 8));
      if (lo < 0 || lo >= count - 1)
        lo = 0;
      float t_lo = *(const float *)(const void *)(in_ptr + (size_t)lo * in_stride);
      float t_hi = *(const float *)(const void *)(in_ptr + (size_t)(lo + 1) * in_stride);
      while (lo > 0 && time_sec < t_lo) {
        lo--;
        t_lo = *(const float *)(const void *)(in_ptr + (size_t)lo * in_stride);
        t_hi = *(const float *)(const void *)(in_ptr + (size_t)(lo + 1) * in_stride);
      }
      while (lo + 1 < count - 1 && time_sec >= t_hi) {
        lo++;
        t_lo = t_hi;
        t_hi = *(const float *)(const void *)(in_ptr + (size_t)(lo + 1) * in_stride);
      }
      hi = lo + 1;
      const float dt = t_hi - t_lo;
      alpha = dt > 0.00001f ? (time_sec - t_lo) / dt : 0.0f;
    }
  }
  rt_gltf_list_set_raw(rec_v, 8, rt_tag_v(lo));

  const unsigned char *a = out_ptr + (size_t)lo * out_stride;
  const unsigned char *b = out_ptr + (size_t)hi * out_stride;
  if (n_comp == 4) {
    const float ax = *(const float *)(const void *)(a + 0);
    const float ay = *(const float *)(const void *)(a + 4);
    const float az = *(const float *)(const void *)(a + 8);
    const float aw = *(const float *)(const void *)(a + 12);
    float bx = *(const float *)(const void *)(b + 0);
    float by = *(const float *)(const void *)(b + 4);
    float bz = *(const float *)(const void *)(b + 8);
    float bw = *(const float *)(const void *)(b + 12);
    const float dot = ax * bx + ay * by + az * bz + aw * bw;
    if (dot < 0.0f) {
      bx = -bx;
      by = -by;
      bz = -bz;
      bw = -bw;
    }
    float rx = ax + (bx - ax) * alpha;
    float ry = ay + (by - ay) * alpha;
    float rz = az + (bz - az) * alpha;
    float rw = aw + (bw - aw) * alpha;
    const float len2 = rx * rx + ry * ry + rz * rz + rw * rw;
    if (len2 <= 0.000001f || !isfinite(len2))
      return rt_gltf_anim_make_vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const float inv_len = 1.0f / sqrtf(len2);
    return rt_gltf_anim_make_vec4(rx * inv_len, ry * inv_len, rz * inv_len, rw * inv_len);
  }

  const float ax = *(const float *)(const void *)(a + 0);
  const float ay = *(const float *)(const void *)(a + 4);
  const float az = *(const float *)(const void *)(a + 8);
  const float bx = *(const float *)(const void *)(b + 0);
  const float by = *(const float *)(const void *)(b + 4);
  const float bz = *(const float *)(const void *)(b + 8);
  return rt_gltf_anim_make_vec3(ax + (bx - ax) * alpha, ay + (by - ay) * alpha,
                                az + (bz - az) * alpha);
}

static inline void rt_gltf_skin_accum(const float *m, float w, float px, float py, float pz,
                                      float nx, float ny, float nz, float *sx, float *sy,
                                      float *sz, float *nnx, float *nny, float *nnz) {
  *sx += (m[0] * px + m[4] * py + m[8] * pz + m[12]) * w;
  *sy += (m[1] * px + m[5] * py + m[9] * pz + m[13]) * w;
  *sz += (m[2] * px + m[6] * py + m[10] * pz + m[14]) * w;
  *nnx += (m[0] * nx + m[4] * ny + m[8] * nz) * w;
  *nny += (m[1] * nx + m[5] * ny + m[9] * nz) * w;
  *nnz += (m[2] * nx + m[6] * ny + m[10] * nz) * w;
}

static inline void rt_gltf_skin_two(const float *m0, const float *m1, float w0, float w1,
                                    float px, float py, float pz, float nx, float ny,
                                    float nz, unsigned char *dst) {
  float sx = (m0[0] * px + m0[4] * py + m0[8] * pz + m0[12]) * w0 +
             (m1[0] * px + m1[4] * py + m1[8] * pz + m1[12]) * w1;
  float sy = (m0[1] * px + m0[5] * py + m0[9] * pz + m0[13]) * w0 +
             (m1[1] * px + m1[5] * py + m1[9] * pz + m1[13]) * w1;
  float sz = (m0[2] * px + m0[6] * py + m0[10] * pz + m0[14]) * w0 +
             (m1[2] * px + m1[6] * py + m1[10] * pz + m1[14]) * w1;
  const float wsum = w0 + w1;
  if (wsum > 0.000001f && (wsum < 0.9999f || wsum > 1.0001f)) {
    const float inv_w = 1.0f / wsum;
    sx *= inv_w;
    sy *= inv_w;
    sz *= inv_w;
  }
  *(float *)(void *)(dst + 0) = rt_gltf_f32_clean(sx);
  *(float *)(void *)(dst + 4) = rt_gltf_f32_clean(sy);
  *(float *)(void *)(dst + 8) = rt_gltf_f32_clean(sz);

  float nnx = (m0[0] * nx + m0[4] * ny + m0[8] * nz) * w0 +
              (m1[0] * nx + m1[4] * ny + m1[8] * nz) * w1;
  float nny = (m0[1] * nx + m0[5] * ny + m0[9] * nz) * w0 +
              (m1[1] * nx + m1[5] * ny + m1[9] * nz) * w1;
  float nnz = (m0[2] * nx + m0[6] * ny + m0[10] * nz) * w0 +
              (m1[2] * nx + m1[6] * ny + m1[10] * nz) * w1;
  if (wsum > 0.000001f && (wsum < 0.9999f || wsum > 1.0001f)) {
    const float inv_w = 1.0f / wsum;
    nnx *= inv_w;
    nny *= inv_w;
    nnz *= inv_w;
  }
  const float nlen2 = nnx * nnx + nny * nny + nnz * nnz;
  if (nlen2 > 0.999f && nlen2 < 1.001f) {
    *(float *)(void *)(dst + 24) = rt_gltf_f32_clean(nnx);
    *(float *)(void *)(dst + 28) = rt_gltf_f32_clean(nny);
    *(float *)(void *)(dst + 32) = rt_gltf_f32_clean(nnz);
  } else if (nlen2 > 0.000001f && isfinite(nlen2)) {
    const float inv_n = 1.0f / sqrtf(nlen2);
    *(float *)(void *)(dst + 24) = rt_gltf_f32_clean(nnx * inv_n);
    *(float *)(void *)(dst + 28) = rt_gltf_f32_clean(nny * inv_n);
    *(float *)(void *)(dst + 32) = rt_gltf_f32_clean(nnz * inv_n);
  }
}

static inline void rt_gltf_skin_four(const float *m0, const float *m1, const float *m2,
                                     const float *m3, float w0, float w1, float w2,
                                     float w3, float px, float py, float pz, float nx,
                                     float ny, float nz, unsigned char *dst) {
  const float wsum = w0 + w1 + w2 + w3;
  if (wsum <= 0.000001f) {
    *(float *)(void *)(dst + 0) = rt_gltf_f32_clean(px);
    *(float *)(void *)(dst + 4) = rt_gltf_f32_clean(py);
    *(float *)(void *)(dst + 8) = rt_gltf_f32_clean(pz);
    *(float *)(void *)(dst + 24) = rt_gltf_f32_clean(nx);
    *(float *)(void *)(dst + 28) = rt_gltf_f32_clean(ny);
    *(float *)(void *)(dst + 32) = rt_gltf_f32_clean(nz);
    return;
  }

  float sx = (m0[0] * px + m0[4] * py + m0[8] * pz + m0[12]) * w0 +
             (m1[0] * px + m1[4] * py + m1[8] * pz + m1[12]) * w1 +
             (m2[0] * px + m2[4] * py + m2[8] * pz + m2[12]) * w2 +
             (m3[0] * px + m3[4] * py + m3[8] * pz + m3[12]) * w3;
  float sy = (m0[1] * px + m0[5] * py + m0[9] * pz + m0[13]) * w0 +
             (m1[1] * px + m1[5] * py + m1[9] * pz + m1[13]) * w1 +
             (m2[1] * px + m2[5] * py + m2[9] * pz + m2[13]) * w2 +
             (m3[1] * px + m3[5] * py + m3[9] * pz + m3[13]) * w3;
  float sz = (m0[2] * px + m0[6] * py + m0[10] * pz + m0[14]) * w0 +
             (m1[2] * px + m1[6] * py + m1[10] * pz + m1[14]) * w1 +
             (m2[2] * px + m2[6] * py + m2[10] * pz + m2[14]) * w2 +
             (m3[2] * px + m3[6] * py + m3[10] * pz + m3[14]) * w3;
  const bool unit_w = (wsum >= 0.9999f && wsum <= 1.0001f);
  const float inv_w = unit_w ? 1.0f : (1.0f / wsum);
  sx = rt_gltf_f32_clean(sx * inv_w);
  sy = rt_gltf_f32_clean(sy * inv_w);
  sz = rt_gltf_f32_clean(sz * inv_w);
  *(float *)(void *)(dst + 0) = sx;
  *(float *)(void *)(dst + 4) = sy;
  *(float *)(void *)(dst + 8) = sz;

  float nnx = (m0[0] * nx + m0[4] * ny + m0[8] * nz) * w0 +
              (m1[0] * nx + m1[4] * ny + m1[8] * nz) * w1 +
              (m2[0] * nx + m2[4] * ny + m2[8] * nz) * w2 +
              (m3[0] * nx + m3[4] * ny + m3[8] * nz) * w3;
  float nny = (m0[1] * nx + m0[5] * ny + m0[9] * nz) * w0 +
              (m1[1] * nx + m1[5] * ny + m1[9] * nz) * w1 +
              (m2[1] * nx + m2[5] * ny + m2[9] * nz) * w2 +
              (m3[1] * nx + m3[5] * ny + m3[9] * nz) * w3;
  float nnz = (m0[2] * nx + m0[6] * ny + m0[10] * nz) * w0 +
              (m1[2] * nx + m1[6] * ny + m1[10] * nz) * w1 +
              (m2[2] * nx + m2[6] * ny + m2[10] * nz) * w2 +
              (m3[2] * nx + m3[6] * ny + m3[10] * nz) * w3;
  nnx *= inv_w;
  nny *= inv_w;
  nnz *= inv_w;
  const float nlen2 = nnx * nnx + nny * nny + nnz * nnz;
  if (nlen2 > 0.999f && nlen2 < 1.001f) {
    *(float *)(void *)(dst + 24) = rt_gltf_f32_clean(nnx);
    *(float *)(void *)(dst + 28) = rt_gltf_f32_clean(nny);
    *(float *)(void *)(dst + 32) = rt_gltf_f32_clean(nnz);
  } else if (nlen2 > 0.000001f && isfinite(nlen2)) {
    const float inv_n = 1.0f / sqrtf(nlen2);
    *(float *)(void *)(dst + 24) = rt_gltf_f32_clean(nnx * inv_n);
    *(float *)(void *)(dst + 28) = rt_gltf_f32_clean(nny * inv_n);
    *(float *)(void *)(dst + 32) = rt_gltf_f32_clean(nnz * inv_n);
  }
}

static inline void rt_gltf_skin_one(const float *m, float px, float py, float pz, float nx,
                                    float ny, float nz, unsigned char *dst) {
  const float sx = rt_gltf_f32_clean(m[0] * px + m[4] * py + m[8] * pz + m[12]);
  const float sy = rt_gltf_f32_clean(m[1] * px + m[5] * py + m[9] * pz + m[13]);
  const float sz = rt_gltf_f32_clean(m[2] * px + m[6] * py + m[10] * pz + m[14]);
  *(float *)(void *)(dst + 0) = sx;
  *(float *)(void *)(dst + 4) = sy;
  *(float *)(void *)(dst + 8) = sz;

  const float nnx = m[0] * nx + m[4] * ny + m[8] * nz;
  const float nny = m[1] * nx + m[5] * ny + m[9] * nz;
  const float nnz = m[2] * nx + m[6] * ny + m[10] * nz;
  const float nlen2 = nnx * nnx + nny * nny + nnz * nnz;
  if (nlen2 > 0.999f && nlen2 < 1.001f) {
    *(float *)(void *)(dst + 24) = rt_gltf_f32_clean(nnx);
    *(float *)(void *)(dst + 28) = rt_gltf_f32_clean(nny);
    *(float *)(void *)(dst + 32) = rt_gltf_f32_clean(nnz);
  } else if (nlen2 > 0.000001f && isfinite(nlen2)) {
    const float inv_n = 1.0f / sqrtf(nlen2);
    *(float *)(void *)(dst + 24) = rt_gltf_f32_clean(nnx * inv_n);
    *(float *)(void *)(dst + 28) = rt_gltf_f32_clean(nny * inv_n);
    *(float *)(void *)(dst + 32) = rt_gltf_f32_clean(nnz * inv_n);
  }
}

int64_t rt_gltf_skin_apply_raw(int64_t vptr_v, int64_t bind_vptr_v, int64_t joints_ptr_v,
                               int64_t weights_ptr_v, int64_t vcnt_v, int64_t skin_slab_v,
                               int64_t mat_count_v) {
  unsigned char *restrict vptr = (unsigned char *)(uintptr_t)vptr_v;
  const unsigned char *restrict bind = (const unsigned char *)(uintptr_t)bind_vptr_v;
  const uint32_t *restrict joints = (const uint32_t *)(uintptr_t)joints_ptr_v;
  const float *restrict weights = (const float *)(uintptr_t)weights_ptr_v;
  const float *restrict mats = (const float *)(uintptr_t)skin_slab_v;
  int64_t vcnt = is_int(vcnt_v) ? (vcnt_v >> 1) : vcnt_v;
  int64_t mat_count = is_int(mat_count_v) ? (mat_count_v >> 1) : mat_count_v;
  if (!vptr || !bind || !joints || !weights || !mats || vcnt <= 0 || mat_count <= 0)
    return 0;

  for (int64_t vi = 0; vi < vcnt; vi++) {
    const unsigned char *src = bind + (size_t)vi * 64u;
    unsigned char *dst = vptr + (size_t)vi * 64u;
    const float px = *(const float *)(const void *)(src + 0);
    const float py = *(const float *)(const void *)(src + 4);
    const float pz = *(const float *)(const void *)(src + 8);
    const float nx = *(const float *)(const void *)(src + 24);
    const float ny = *(const float *)(const void *)(src + 28);
    const float nz = *(const float *)(const void *)(src + 32);
    const uint32_t *j = joints + vi * 4;
    const float *w = weights + vi * 4;

    if (w[0] >= 0.999999f && w[1] <= 0.000001f && w[2] <= 0.000001f &&
        w[3] <= 0.000001f && j[0] < (uint32_t)mat_count) {
      rt_gltf_skin_one(mats + (size_t)j[0] * 16u, px, py, pz, nx, ny, nz, dst);
      continue;
    }
    if (w[0] > 0.000001f && w[1] > 0.000001f && w[2] <= 0.000001f &&
        w[3] <= 0.000001f && j[0] < (uint32_t)mat_count && j[1] < (uint32_t)mat_count) {
      rt_gltf_skin_two(mats + (size_t)j[0] * 16u, mats + (size_t)j[1] * 16u, w[0], w[1], px,
                       py, pz, nx, ny, nz, dst);
      continue;
    }
    if (j[0] < (uint32_t)mat_count && j[1] < (uint32_t)mat_count &&
        j[2] < (uint32_t)mat_count && j[3] < (uint32_t)mat_count) {
      rt_gltf_skin_four(mats + (size_t)j[0] * 16u, mats + (size_t)j[1] * 16u,
                        mats + (size_t)j[2] * 16u, mats + (size_t)j[3] * 16u, w[0], w[1],
                        w[2], w[3], px, py, pz, nx, ny, nz, dst);
      continue;
    }

    float sx = 0.0f, sy = 0.0f, sz = 0.0f;
    float nnx = 0.0f, nny = 0.0f, nnz = 0.0f;
    float wsum = 0.0f;
    for (int k = 0; k < 4; k++) {
      const uint32_t ji = j[k];
      const float wk = w[k];
      if (wk > 0.000001f && ji < (uint32_t)mat_count) {
        rt_gltf_skin_accum(mats + (size_t)ji * 16u, wk, px, py, pz, nx, ny, nz, &sx, &sy, &sz,
                           &nnx, &nny, &nnz);
        wsum += wk;
      }
    }
    if (wsum <= 0.000001f) {
      *(float *)(void *)(dst + 0) = rt_gltf_f32_clean(px);
      *(float *)(void *)(dst + 4) = rt_gltf_f32_clean(py);
      *(float *)(void *)(dst + 8) = rt_gltf_f32_clean(pz);
      *(float *)(void *)(dst + 24) = rt_gltf_f32_clean(nx);
      *(float *)(void *)(dst + 28) = rt_gltf_f32_clean(ny);
      *(float *)(void *)(dst + 32) = rt_gltf_f32_clean(nz);
      continue;
    }

    const bool unit_w = (wsum >= 0.9999f && wsum <= 1.0001f);
    const float inv_w = unit_w ? 1.0f : (1.0f / wsum);
    sx = rt_gltf_f32_clean(sx * inv_w);
    sy = rt_gltf_f32_clean(sy * inv_w);
    sz = rt_gltf_f32_clean(sz * inv_w);
    *(float *)(void *)(dst + 0) = sx;
    *(float *)(void *)(dst + 4) = sy;
    *(float *)(void *)(dst + 8) = sz;

    nnx *= inv_w;
    nny *= inv_w;
    nnz *= inv_w;
    const float nlen2 = nnx * nnx + nny * nny + nnz * nnz;
    if (nlen2 > 0.999f && nlen2 < 1.001f) {
      *(float *)(void *)(dst + 24) = rt_gltf_f32_clean(nnx);
      *(float *)(void *)(dst + 28) = rt_gltf_f32_clean(nny);
      *(float *)(void *)(dst + 32) = rt_gltf_f32_clean(nnz);
    } else if (nlen2 > 0.000001f && isfinite(nlen2)) {
      const float inv_n = 1.0f / sqrtf(nlen2);
      *(float *)(void *)(dst + 24) = rt_gltf_f32_clean(nnx * inv_n);
      *(float *)(void *)(dst + 28) = rt_gltf_f32_clean(nny * inv_n);
      *(float *)(void *)(dst + 32) = rt_gltf_f32_clean(nnz * inv_n);
    }
  }
  return vptr_v;
}

int64_t rt_gltf_skin_apply_one_raw(int64_t vptr_v, int64_t bind_vptr_v, int64_t joints_ptr_v,
                                   int64_t vcnt_v, int64_t skin_slab_v, int64_t mat_count_v) {
  unsigned char *restrict vptr = (unsigned char *)(uintptr_t)vptr_v;
  const unsigned char *restrict bind = (const unsigned char *)(uintptr_t)bind_vptr_v;
  const uint32_t *restrict joints = (const uint32_t *)(uintptr_t)joints_ptr_v;
  const float *restrict mats = (const float *)(uintptr_t)skin_slab_v;
  int64_t vcnt = is_int(vcnt_v) ? (vcnt_v >> 1) : vcnt_v;
  int64_t mat_count = is_int(mat_count_v) ? (mat_count_v >> 1) : mat_count_v;
  if (!vptr || !bind || !joints || !mats || vcnt <= 0 || mat_count <= 0)
    return 0;

  for (int64_t vi = 0; vi < vcnt; vi++) {
    const uint32_t ji = joints[(size_t)vi * 4u];
    if (ji >= (uint32_t)mat_count)
      continue;
    const unsigned char *src = bind + (size_t)vi * 64u;
    unsigned char *dst = vptr + (size_t)vi * 64u;
    rt_gltf_skin_one(mats + (size_t)ji * 16u, *(const float *)(const void *)(src + 0),
                     *(const float *)(const void *)(src + 4),
                     *(const float *)(const void *)(src + 8),
                     *(const float *)(const void *)(src + 24),
                     *(const float *)(const void *)(src + 28),
                     *(const float *)(const void *)(src + 32), dst);
  }
  return vptr_v;
}

static inline void rt_gltf_store_f32(unsigned char *p, int off, float v) {
  *(float *)(void *)(p + off) = v;
}

static inline void rt_gltf_store_u32(unsigned char *p, int off, uint32_t v) {
  *(uint32_t *)(void *)(p + off) = v;
}

static inline uint16_t rt_gltf_load_u16(const unsigned char *p) {
  uint16_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static inline uint32_t rt_gltf_load_u32(const unsigned char *p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static inline float rt_gltf_load_f32(const unsigned char *p) {
  float v;
  memcpy(&v, p, sizeof(v));
  return rt_gltf_f32_clean(v);
}

static inline float rt_gltf_read_component_f32(const unsigned char *p, int comp, int normalized) {
  switch (comp) {
  case 5126:
    return rt_gltf_load_f32(p);
  case 5121: {
    const uint8_t raw = *p;
    return normalized ? (float)raw * (1.0f / 255.0f) : (float)raw;
  }
  case 5120: {
    const int8_t raw = (int8_t)*p;
    if (normalized) {
      const float v = (float)raw * (1.0f / 127.0f);
      return v < -1.0f ? -1.0f : v;
    }
    return (float)raw;
  }
  case 5123: {
    const uint16_t raw = rt_gltf_load_u16(p);
    return normalized ? (float)raw * (1.0f / 65535.0f) : (float)raw;
  }
  case 5122: {
    const int16_t raw = (int16_t)rt_gltf_load_u16(p);
    if (normalized) {
      const float v = (float)raw * (1.0f / 32767.0f);
      return v < -1.0f ? -1.0f : v;
    }
    return (float)raw;
  }
  case 5125: {
    const uint32_t raw = rt_gltf_load_u32(p);
    return normalized ? (float)((double)raw * (1.0 / 4294967295.0)) : (float)raw;
  }
  default:
    return 0.0f;
  }
}

static inline uint8_t rt_gltf_color_byte(float v) {
  if (!isfinite(v))
    v = 0.0f;
  if (v < 0.0f)
    v = 0.0f;
  if (v > 1.0f)
    v = 1.0f;
  return (uint8_t)(v * 255.0f + 0.5f);
}

int64_t rt_gltf_pack_vertices_pnc_raw(int64_t dst_v, int64_t count_v, int64_t pos_ptr_v,
                                      int64_t pos_stride_v, int64_t norm_ptr_v,
                                      int64_t norm_count_v, int64_t norm_stride_v,
                                      int64_t color_ptr_v, int64_t color_count_v,
                                      int64_t color_stride_v, int64_t color_comp_v,
                                      int64_t color_type_count_v, int64_t color_norm_v,
                                      int64_t tex_id_v) {
  unsigned char *restrict dst = (unsigned char *)(uintptr_t)dst_v;
  const unsigned char *restrict pos = (const unsigned char *)(uintptr_t)pos_ptr_v;
  const unsigned char *restrict norm = (const unsigned char *)(uintptr_t)norm_ptr_v;
  const unsigned char *restrict color = (const unsigned char *)(uintptr_t)color_ptr_v;
  const int64_t count = rt_gltf_i64_arg(count_v);
  const int64_t pos_stride = rt_gltf_i64_arg(pos_stride_v);
  const int64_t norm_count = rt_gltf_i64_arg(norm_count_v);
  const int64_t norm_stride = rt_gltf_i64_arg(norm_stride_v);
  const int64_t color_count = rt_gltf_i64_arg(color_count_v);
  const int64_t color_stride = rt_gltf_i64_arg(color_stride_v);
  const int color_comp = (int)rt_gltf_i64_arg(color_comp_v);
  const int color_type_count = (int)rt_gltf_i64_arg(color_type_count_v);
  const int color_norm = rt_is_truthy(color_norm_v);
  const uint32_t tex_id = (uint32_t)rt_gltf_i64_arg(tex_id_v);

  if (!dst || !pos || count <= 0 || pos_stride < 12)
    return 0;

  for (int64_t vi = 0; vi < count; vi++) {
    const unsigned char *p = pos + (size_t)vi * (size_t)pos_stride;
    unsigned char *out = dst + (size_t)vi * 64u;
    const float px = rt_gltf_load_f32(p + 0);
    const float py = rt_gltf_load_f32(p + 4);
    const float pz = rt_gltf_load_f32(p + 8);

    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    if (norm && vi < norm_count && norm_stride >= 12) {
      const unsigned char *n = norm + (size_t)vi * (size_t)norm_stride;
      nx = rt_gltf_load_f32(n + 0);
      ny = rt_gltf_load_f32(n + 4);
      nz = rt_gltf_load_f32(n + 8);
      const float nl2 = nx * nx + ny * ny + nz * nz;
      if (nl2 > 0.0000000001f) {
        const float inv = 1.0f / sqrtf(nl2);
        nx *= inv;
        ny *= inv;
        nz *= inv;
      }
    }

    uint32_t packed_color = 0xffffffffu;
    if (color && vi < color_count && color_stride > 0 &&
        (color_type_count == 3 || color_type_count == 4)) {
      const unsigned char *c = color + (size_t)vi * (size_t)color_stride;
      if (color_comp == 5121 && color_norm) {
        const uint32_t r = c[0];
        const uint32_t g = c[1];
        const uint32_t b = c[2];
        const uint32_t a = color_type_count == 4 ? c[3] : 255u;
        packed_color = r | (g << 8) | (b << 16) | (a << 24);
      } else {
        int cs = 0;
        switch (color_comp) {
        case 5120:
        case 5121:
          cs = 1;
          break;
        case 5122:
        case 5123:
          cs = 2;
          break;
        case 5125:
        case 5126:
          cs = 4;
          break;
        default:
          cs = 0;
          break;
        }
        if (cs > 0) {
          const float r = rt_gltf_read_component_f32(c + (size_t)cs * 0u, color_comp, color_norm);
          const float g = rt_gltf_read_component_f32(c + (size_t)cs * 1u, color_comp, color_norm);
          const float b = rt_gltf_read_component_f32(c + (size_t)cs * 2u, color_comp, color_norm);
          const float a = color_type_count == 4
                              ? rt_gltf_read_component_f32(c + (size_t)cs * 3u, color_comp, color_norm)
                              : 1.0f;
          packed_color = (uint32_t)rt_gltf_color_byte(r) |
                         ((uint32_t)rt_gltf_color_byte(g) << 8) |
                         ((uint32_t)rt_gltf_color_byte(b) << 16) |
                         ((uint32_t)rt_gltf_color_byte(a) << 24);
        }
      }
    }

    rt_gltf_store_f32(out, 0, px);
    rt_gltf_store_f32(out, 4, py);
    rt_gltf_store_f32(out, 8, pz);
    rt_gltf_store_f32(out, 12, 0.0f);
    rt_gltf_store_f32(out, 16, 0.0f);
    rt_gltf_store_u32(out, 20, packed_color);
    rt_gltf_store_f32(out, 24, nx);
    rt_gltf_store_f32(out, 28, ny);
    rt_gltf_store_f32(out, 32, nz);
    rt_gltf_store_f32(out, 36, 0.0f);
    rt_gltf_store_f32(out, 40, 0.0f);
    rt_gltf_store_f32(out, 44, 0.0f);
    rt_gltf_store_f32(out, 48, 1.0f);
    rt_gltf_store_f32(out, 52, 0.0f);
    rt_gltf_store_f32(out, 56, 0.0f);
    rt_gltf_store_u32(out, 60, tex_id);
  }

  return dst_v;
}
