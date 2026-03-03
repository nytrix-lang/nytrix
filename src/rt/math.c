#include "rt/shared.h"
#include <math.h>

static inline double get_flt(int64_t v) {
  if (v & 1) return (double)(v >> 1);
  int64_t bits = __rt_flt_unbox_val(v);
  double d;
  memcpy(&d, &bits, 8);
  return d;
}

static inline void set_mat_flt(int64_t m, int idx, double val) {
  int64_t bits;
  memcpy(&bits, &val, 8);
  // Nytrix list data starts at offset 16 (index 2).
  __rt_store_item_fast(m, (int64_t)((idx + 2) << 1 | 1), __flt_box_val(bits));
}

static uint64_t __rng_state = 0x123456789ABCDEF0ULL;
static int __rng_forced_prng = 0;

int64_t __srand(int64_t s) {
  __rng_state = (uint64_t)(s >> 1);
  __rng_forced_prng = 1;
  return s;
}

int64_t __rand64(void) {
  uint64_t val = 0;
  int ok = 0;
#if defined(__x86_64__)
  if (!__rng_forced_prng) {
    __asm__ volatile("rdrand %0; setc %b1" : "=r"(val), "=q"(ok));
  }
#endif
  if (!ok) {
    __rng_state += 0x9e3779b97f4a7c15ULL;
    uint64_t z = __rng_state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    val = z ^ (z >> 31);
  }
  uint64_t res = ((uint64_t)(val & 0x3FFFFFFFFFFFFFFFULL) << 1) | 1ULL;
  return (int64_t)res;
}

static __thread void *g_flt_cache = NULL;

static void *__flt_alloc_slot(void) {
  if (g_flt_cache) {
    void *p = g_flt_cache;
    g_flt_cache = *(void **)p;
    return p;
  }
  size_t chunk_size = 4096;
  char *chunk = (char *)__malloc((int64_t)((chunk_size << 1) | 1));
  if (!chunk) return NULL;
  size_t slot_count = chunk_size / 16;
  for (size_t i = 0; i < slot_count - 1; i++) {
    void *curr = chunk + (i * 16);
    void *next = chunk + ((i + 1) * 16);
    *(void **)curr = next;
  }
  *(void **)(chunk + ((slot_count - 1) * 16)) = NULL;
  g_flt_cache = (void *)chunk;
  void *p = g_flt_cache;
  g_flt_cache = *(void **)p;
  return p;
}

void __flt_free(int64_t v) {
  if (!v) return;
  void *slot = (void *)((char *)(uintptr_t)v - 8);
  *(void **)slot = g_flt_cache;
  g_flt_cache = slot;
}

int64_t __flt_box_val(int64_t bits) {
  void *slot = __flt_alloc_slot();
  if (!slot) return 0;
  *(int64_t *)slot = TAG_FLOAT;
  memcpy((char *)slot + 8, &bits, 8);
  return (int64_t)(uintptr_t)((char *)slot + 8);
}

int64_t __flt_box_val32(int64_t bits32) {
  uint32_t raw = (bits32 & 1) ? (uint32_t)(bits32 >> 1) : (uint32_t)bits32;
  float f; memcpy(&f, &raw, 4);
  double d = (double)f;
  int64_t b; memcpy(&b, &d, 8);
  return __flt_box_val(b);
}

int64_t __flt_unbox_val32(int64_t v) {
  double d = 0;
  if (v & 1) d = (double)(v >> 1);
  else if (is_v_flt(v)) memcpy(&d, (const void *)(uintptr_t)v, 8);
  float f = (float)d;
  uint32_t b; memcpy(&b, &f, 4);
  return (int64_t)b << 1 | 1;
}

int64_t __flt_from_int(int64_t v) {
  if (v & 1) {
    double d = (double)(v >> 1);
    int64_t b; memcpy(&b, &d, 8);
    return b;
  }
  return 0;
}

int64_t __flt_to_int(int64_t v) {
  int64_t b = __flt_unbox_val(v);
  double d; memcpy(&d, &b, 8);
  return rt_tag_v((int64_t)d);
}

int64_t __flt_trunc(int64_t v) { return __flt_to_int(v); }

#define FLT_OP(name, op) \
  int64_t __flt_##name(int64_t a, int64_t b) { \
    double da = get_flt(a); \
    double db = get_flt(b); \
    double r = da op db; \
    int64_t rr; memcpy(&rr, &r, 8); \
    return __flt_box_val(rr); \
  }

FLT_OP(add, +)
FLT_OP(sub, -)
FLT_OP(mul, *)
FLT_OP(div, /)

#define FLT_CMP(name, op) \
  int64_t __flt_##name(int64_t a, int64_t b) { \
    double da = get_flt(a); \
    double db = get_flt(b); \
    return (da op db) ? 2 : 4; \
  }

FLT_CMP(lt, <)
FLT_CMP(gt, >)
FLT_CMP(le, <=)
FLT_CMP(ge, >=)
FLT_CMP(eq, ==)

int64_t __add(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return (int64_t)((uint64_t)a + (uint64_t)b - 1);
  if (is_v_flt(a) || is_v_flt(b)) return __flt_add(a, b);
  if (is_v_str(a) && is_v_str(b)) return __str_concat(a, b);
  if (is_any_ptr(a) && (b & 1)) return a + (b >> 1);
  if ((a & 1) && is_any_ptr(b)) return b + (a >> 1);
  return 1;
}

int64_t __sub(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return (int64_t)((uint64_t)a - (uint64_t)b + 1);
  if (is_v_flt(a) || is_v_flt(b)) return __flt_sub(a, b);
  if (is_any_ptr(a) && (b & 1)) return a - (b >> 1);
  return 1;
}

int64_t __mul(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return rt_tag_v((a >> 1) * (b >> 1));
  if (is_v_flt(a) || is_v_flt(b)) return __flt_mul(a, b);
  return 1;
}

int64_t __div(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    int64_t bv = b >> 1;
    if (bv == 0) return 1;
    return rt_tag_v((a >> 1) / bv);
  }
  if (is_v_flt(a) || is_v_flt(b)) return __flt_div(a, b);
  return 1;
}

int64_t __mod(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    int64_t bv = b >> 1;
    if (bv == 0) return 1;
    return rt_tag_v((a >> 1) % bv);
  }
  return 1;
}

int64_t __eq(int64_t a, int64_t b) {
  if (a == b) return 2;
  if ((a == 0 && b == 1) || (a == 1 && b == 0)) return 2;
  if ((a & 1) != (b & 1)) return 4;
  if (is_ptr(a) && is_ptr(b)) {
    if (a <= 4 || b <= 4) return 4;
    if (is_v_flt(a) || is_v_flt(b)) return __flt_eq(a, b);
    if (is_v_str(a) && is_v_str(b)) {
      uintptr_t la_p = (uintptr_t)a - 16;
      uintptr_t lb_p = (uintptr_t)b - 16;
      int64_t la_tagged = *(int64_t*)la_p;
      int64_t lb_tagged = *(int64_t*)lb_p;
      if (la_tagged != lb_tagged) return 4;
      size_t la = (size_t)(la_tagged >> 1);
      if (la == 0) return 2;
      return memcmp((const void *)(uintptr_t)a, (const void *)(uintptr_t)b, la) == 0 ? 2 : 4;
    }
  }
  return 4;
}

int64_t __lt(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return (a >> 1) < (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b)) return __flt_lt(a, b);
  if (is_ptr(a) && is_ptr(b)) return a < b ? 2 : 4;
  return 4;
}
int64_t __le(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return (a >> 1) <= (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b)) return __flt_le(a, b);
  if (is_ptr(a) && is_ptr(b)) return a <= b ? 2 : 4;
  return 4;
}
int64_t __gt(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return (a >> 1) > (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b)) return __flt_gt(a, b);
  if (is_ptr(a) && is_ptr(b)) return a > b ? 2 : 4;
  return 4;
}
int64_t __ge(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return (a >> 1) >= (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b)) return __flt_ge(a, b);
  if (is_ptr(a) && is_ptr(b)) return a >= b ? 2 : 4;
  return 4;
}

int64_t __and(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) & (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t __or(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) | (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t __xor(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) ^ (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t __shl(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) << (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t __shr(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) >> (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t __not(int64_t a) {
  return (int64_t)(((~(uint64_t)(a & 1 ? a >> 1 : a)) << 1) | 1);
}

int64_t __flt_unbox_val(int64_t v) {
  return __rt_flt_unbox_val(v);
}

typedef struct {
  float x, y, z;
  float u, v;
  uint32_t color;
} __vkr_vertex_t;

static inline double get_flt_any(int64_t v) {
  if (v & 1) return (double)(v >> 1);
  if (is_ptr(v)) {
    if (is_v_flt(v)) {
      int64_t bits;
      memcpy(&bits, (const void *)(uintptr_t)v, 8);
      double d;
      memcpy(&d, &bits, 8);
      return d;
    }
  }
  return (double)v;
}

void __mat4_mul(int64_t dst_ptr_v, int64_t a_list, int64_t b_list) {
  if (!is_ptr(a_list) || !is_ptr(b_list)) return;
  float a[16], b[16];
  for (int i = 0; i < 16; i++) {
    // Column-Major: items start at offset 16 in the list structure (idx 2).
    a[i] = (float)get_flt(__rt_load_item_fast(a_list, (int64_t)((2 + i) << 1 | 1)));
    b[i] = (float)get_flt(__rt_load_item_fast(b_list, (int64_t)((2 + i) << 1 | 1)));
  }
  float *dst = (float *)(uintptr_t)rt_untag_v(dst_ptr_v);
  if (!dst) return;
  
  // Standard Column-Major multiplication: C[i][j] = sum(A[k][j] * B[i][k])
  // Wait, standard Column-Major order for mat mul is C = A * B:
  // C[row][col] = sum(A[row][k] * B[k][col])
  // In Column-Major index: idx = col * 4 + row
  float res[16];
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      double sum = 0.0;
      for (int k = 0; k < 4; k++) {
        // A[row][k] => index k*4 + row
        // B[k][col] => index col*4 + k
        sum += (double)a[k * 4 + row] * (double)b[col * 4 + k];
      }
      res[col * 4 + row] = (float)sum;
    }
  }
  for (int i = 0; i < 16; i++) dst[i] = res[i];
}

int64_t __mat4_to_buffer(int64_t m_v, int64_t buf_ptr_v) {
  if (!is_ptr(m_v)) return buf_ptr_v;
  float *dst = (float *)(uintptr_t)rt_untag_v(buf_ptr_v);
  if (!dst) return buf_ptr_v;
  int64_t m = rt_untag_v(m_v);
  for (int i = 0; i < 16; i++) {
    int64_t val = __rt_load_item_fast(m, (int64_t)((2 + i) << 1 | 1));
    dst[i] = (float)get_flt(val);
  }
  return buf_ptr_v;
}

int64_t __mat4_from_buffer(int64_t m_v, int64_t buf_ptr_v) {
  if (!is_ptr(m_v)) return m_v;
  float *src = (float *)(uintptr_t)rt_untag_v(buf_ptr_v);
  if (!src) return m_v;
  int64_t m = rt_untag_v(m_v);
  for (int i = 0; i < 16; i++) {
    set_mat_flt(m, i, (double)src[i]);
  }
  return m_v;
}

int64_t __vkr_push_rect_tex(int64_t ptr_v, int64_t x_v, int64_t y_v, int64_t w_v, int64_t h_v,
                         int64_t u1_v, int64_t v1_v, int64_t u2_v, int64_t v2_v, int64_t color_v) {
  void *untagged_ptr = (void *)(uintptr_t)rt_untag_v(ptr_v);
  if (!untagged_ptr) return 0;
  __vkr_vertex_t *v = (__vkr_vertex_t *)untagged_ptr;
  float x = (float)get_flt_any(x_v), y = (float)get_flt_any(y_v), w = (float)get_flt_any(w_v), h = (float)get_flt_any(h_v);
  float u1 = (float)get_flt_any(u1_v), v1 = (float)get_flt_any(v1_v), u2 = (float)get_flt_any(u2_v), v2 = (float)get_flt_any(v2_v);
  uint32_t c = (uint32_t)get_flt_any(color_v);

  #define SET_V(I, PX, PY, PZ, PU, PV, PC) \
    v[I].x = (PX); v[I].y = (PY); v[I].z = (PZ); \
    v[I].u = (PU); v[I].v = (PV); v[I].color = (PC);

  SET_V(0, x,   y,   0.0f, u1, v1, c);
  SET_V(1, x,   y+h, 0.0f, u1, v2, c);
  SET_V(2, x+w, y+h, 0.0f, u2, v2, c);
  SET_V(3, x+w, y+h, 0.0f, u2, v2, c);
  SET_V(4, x+w, y,   0.0f, u2, v1, c);
  SET_V(5, x,   y,   0.0f, u1, v1, c);
  #undef SET_V
  return 0;
}

typedef struct {
  float advance;
  float xoff, yoff;
  float bw, bh;
  float u1, v1, u2, v2;
  int32_t tex_id;
  int32_t present;
  int32_t padding;
} __vkr_glyph_t;

void __vkr_draw_text(int64_t vbo_ptr_v, int64_t text_v, int64_t x_v, int64_t y_v, int64_t color_v,
                     int64_t glyphs_ptr_v, int64_t ascent_v, int64_t line_h_v, int64_t out_info_ptr_v) {
  if (!is_ptr(text_v)) return;
  int64_t text_p = rt_untag_v(text_v);
  const char *s = (const char *)(uintptr_t)text_p;
  size_t len = (size_t)rt_untag_v(*(int64_t *)((char *)(uintptr_t)text_p - 16));
  
  __vkr_vertex_t *v = (__vkr_vertex_t *)(uintptr_t)rt_untag_v(vbo_ptr_v);
  __vkr_glyph_t *glyphs = (__vkr_glyph_t *)(uintptr_t)rt_untag_v(glyphs_ptr_v);
  if (!v || !glyphs) return;

  float pen_x = (float)get_flt_any(x_v);
  float pen_y = (float)get_flt_any(y_v) + (float)get_flt_any(ascent_v);
  float start_x = pen_x;
  float line_h = (float)get_flt_any(line_h_v);
  uint32_t c = (uint32_t)get_flt_any(color_v);
  
  int vert_idx = 0;
  int last_tex = -1;

  for (size_t i = 0; i < len; ) {
    unsigned char c1 = (unsigned char)s[i];
    uint32_t cp = 0;
    int step = 1;
    if (c1 < 0x80) cp = c1;
    else if ((c1 & 0xE0) == 0xC0 && i+1 < len) { cp = ((c1 & 0x1F) << 6) | (s[i+1] & 0x3F); step = 2; }
    else if ((c1 & 0xF0) == 0xE0 && i+2 < len) { cp = ((c1 & 0x0F) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F); step = 3; }
    else if ((c1 & 0xF8) == 0xF0 && i+3 < len) { cp = ((c1 & 0x07) << 18) | ((s[i+1] & 0x3F) << 12) | ((s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F); step = 4; }
    i += step;

    if (cp == '\r') continue;
    if (cp == '\n') { pen_x = start_x; pen_y += line_h; continue; }
    if (cp == '\t') { pen_x += glyphs[' '].advance * 4.0f; continue; }
    
    if (cp >= 256 || !glyphs[cp].present) {
        if (glyphs['?'].present) cp = '?';
        else continue;
    }

    __vkr_glyph_t *g = &glyphs[cp];
    if (g->tex_id >= 0) last_tex = g->tex_id;

    if (g->bw > 0.0f && g->bh > 0.0f) {
      float gx = pen_x + g->xoff;
      float gy = pen_y - g->yoff;
      
      #define SET_VTX(IDX, PX, PY, PU, PV) \
        v[vert_idx+IDX].x = (PX); v[vert_idx+IDX].y = (PY); v[vert_idx+IDX].z = 0.0f; \
        v[vert_idx+IDX].u = (PU); v[vert_idx+IDX].v = (PV); v[vert_idx+IDX].color = c;

      SET_VTX(0, gx,       gy,       g->u1, g->v1);
      SET_VTX(1, gx,       gy+g->bh, g->u1, g->v2);
      SET_VTX(2, gx+g->bw, gy+g->bh, g->u2, g->v2);
      SET_VTX(3, gx+g->bw, gy+g->bh, g->u2, g->v2);
      SET_VTX(4, gx+g->bw, gy,       g->u2, g->v1);
      SET_VTX(5, gx,       gy,       g->u1, g->v1);
      #undef SET_VTX
      
      vert_idx += 6;
    }
    pen_x += g->advance;
  }
  
  if (out_info_ptr_v) {
      int64_t *info = (int64_t *)rt_untag_v(out_info_ptr_v);
      if (info) {
          info[0] = rt_tag_v((int64_t)vert_idx);
          info[1] = rt_tag_v((int64_t)last_tex);
      }
  }
}

void __vkr_push_line(int64_t ptr_v, int64_t x1_v, int64_t y1_v, int64_t x2_v, int64_t y2_v, int64_t thickness_v, int64_t color_v) {
  __vkr_vertex_t *v = (__vkr_vertex_t *)(uintptr_t)rt_untag_v(ptr_v);
  float x1 = (float)get_flt_any(x1_v), y1 = (float)get_flt_any(y1_v);
  float x2 = (float)get_flt_any(x2_v), y2 = (float)get_flt_any(y2_v);
  float th = (float)get_flt_any(thickness_v);
  uint32_t c = (uint32_t)get_flt_any(color_v);

  float dx = x2 - x1, dy = y2 - y1;
  float l = sqrtf(dx*dx + dy*dy);
  if (l == 0.0f) return;
  float nx = -dy / l * (th * 0.5f);
  float ny =  dx / l * (th * 0.5f);

  #define SET_V(I, PX, PY, PC) \
    v[I].x = (PX); v[I].y = (PY); v[I].z = 0.0f; \
    v[I].u = 0.0f; v[I].v = 0.0f; v[I].color = (PC);

  SET_V(0, x1+nx, y1+ny, c);
  SET_V(1, x1-nx, y1-ny, c);
  SET_V(2, x2-nx, y2-ny, c);
  SET_V(3, x1+nx, y1+ny, c);
  SET_V(4, x2-nx, y2-ny, c);
  SET_V(5, x2+nx, y2+ny, c);
  #undef SET_V
}

void __vkr_push_rect(int64_t ptr_v, int64_t x_v, int64_t y_v, int64_t w_v, int64_t h_v, int64_t color_v) {
  __vkr_push_rect_tex(ptr_v, x_v, y_v, w_v, h_v, 0, 0, 0, 0, color_v);
}

int64_t __vkr_pack_color(int64_t r_v, int64_t g_v, int64_t b_v, int64_t a_v) {
  uint32_t r = (uint32_t)(get_flt_any(r_v) * 255.0);
  uint32_t g = (uint32_t)(get_flt_any(g_v) * 255.0);
  uint32_t b = (uint32_t)(get_flt_any(b_v) * 255.0);
  uint32_t a = (uint32_t)(get_flt_any(a_v) * 255.0);
  return rt_tag_v((int64_t)(a << 24 | b << 16 | g << 8 | r));
}

void __cam_compute_vectors(int64_t yaw_v, int64_t pitch_v, int64_t out_list_v) {
  double yaw = get_flt(yaw_v);
  double pitch = get_flt(pitch_v);
  double cp = cos(pitch);
  double fx = cos(yaw) * cp;
  double fy = sin(pitch);
  double fz = sin(yaw) * cp;
  int64_t out = rt_untag_v(out_list_v);
  if (!is_ptr(out)) return;
  set_mat_flt(out, 0, fx);
  set_mat_flt(out, 1, fy);
  set_mat_flt(out, 2, fz);
}

void __vkr_push_vertex(int64_t off_v, int64_t x_v, int64_t y_v, int64_t z_v, int64_t u_v, int64_t v_v, int64_t color_v) {
  __vkr_vertex_t *v = (__vkr_vertex_t *)(uintptr_t)rt_untag_v(off_v);
  if (!v) return;
  v->x = (float)get_flt_any(x_v);
  v->y = (float)get_flt_any(y_v);
  v->z = (float)get_flt_any(z_v);
  v->u = (float)get_flt_any(u_v);
  v->v = (float)get_flt_any(v_v);
  v->color = (uint32_t)get_flt_any(color_v);
}
