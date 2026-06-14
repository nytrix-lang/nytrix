;; Keywords: render texture image atlas environment mip os ui
;; Backend-neutral texture helpers: CPU mip generation, atlas packing, environment images, and upload-job planning.
module std.os.ui.render.texture(
   tex_job_make, tex_job_queue_make, tex_job_queue_push, tex_job_queue_pop,
   tex_job_result_make, tex_job_cache_key, tex_job_worker_plan, tex_job_upload_plan,
   resize, rgba_mip_level_count, rgba_mip_total_bytes, generate_rgba_mips,
   RECT_PACK_HEURISTIC_BL, RECT_PACK_HEURISTIC_BF, rect_pack_init, rect_pack,
   srgb_to_linear_chan, linear_to_srgb_chan, linear_to_srgb_u8,
   image_sample_linear_rgb_uv, env_dir_to_uv, generate_spec_env_slab,
   generate_env_image, generate_neutral_env_image, generate_compare_visible_env_image,
   generate_compare_reflect_env_image, generate_studio_env_image,
   scene_prefers_studio_env, scene_prefers_neutral_env,
   scene_prefers_compare_reflect_env, scene_prefers_compare_visible_env,
   scene_prefers_optical_spec_env, scene_prefers_black_visible_env,
   scene_prefers_gray_proof_bg
)

use std.core
use std.core.mem
use std.math
use std.os.ui.render.env as render_env
use std.math.crypto.hash as hash
use std.math.float (is_nan)

fn rgba_mip_level_count(int w, int h) int {
   "Returns the number of RGBA mip levels for an image size."
   mut levels = 1
   mut cw = max(1, int(w))
   mut ch = max(1, int(h))
   while cw > 1 || ch > 1 {
      cw, ch = max(1, cw >> 1), max(1, ch >> 1)
      levels += 1
   }
   levels
}

fn rgba_mip_total_bytes(int w, int h) int {
   "Returns the total bytes needed for all packed RGBA mip levels."
   mut total = 0
   mut cw = max(1, int(w))
   mut ch = max(1, int(h))
   while cw > 0 && ch > 0 {
      total += cw * ch * 4
      if cw == 1 && ch == 1 { break }
      cw, ch = max(1, cw >> 1), max(1, ch >> 1)
   }
   total
}

fn generate_rgba_mips(ptr src_pixels, int w, int h, bool copy_single=false) any {
   "Generates packed RGBA mip levels from a source RGBA image."
   def iw, ih = int(w), int(h)
   if !src_pixels || iw <= 0 || ih <= 0 { return 0 }
   def levels = rgba_mip_level_count(iw, ih)
   if levels <= 1 {
      if !copy_single { return src_pixels }
      def single_bytes = iw * ih * 4
      def copy = malloc(single_bytes)
      if !copy { return 0 }
      memcpy(copy, src_pixels, single_bytes)
      return copy
   }
   def total = rgba_mip_total_bytes(iw, ih)
   mut dst = malloc(total)
   if !dst { return 0 }
   memcpy(dst, src_pixels, iw * ih * 4)
   mut src_off = 0
   mut dst_off = iw * ih * 4
   mut prev_w = iw
   mut prev_h = ih
   mut i = 1
   while i < levels {
      mut next_w, next_h = prev_w >> 1, prev_h >> 1
      if next_w < 1 { next_w = 1 }
      if next_h < 1 { next_h = 1 }
      mut y = 0
      while y < next_h {
         mut x = 0
         while x < next_w {
            mut sx0, sy0 = x << 1, y << 1
            if sx0 >= prev_w { sx0 = prev_w - 1 }
            if sy0 >= prev_h { sy0 = prev_h - 1 }
            mut sx1, sy1 = sx0 + 1, sy0 + 1
            if sx1 >= prev_w { sx1 = prev_w - 1 }
            if sy1 >= prev_h { sy1 = prev_h - 1 }
            def p00, p10 = src_off + (sy0 * prev_w + sx0) * 4, src_off + (sy0 * prev_w + sx1) * 4
            def p01, p11 = src_off + (sy1 * prev_w + sx0) * 4, src_off + (sy1 * prev_w + sx1) * 4
            def dp = dst_off + (y * next_w + x) * 4
            mut c = 0
            while c < 4 {
               def sum = int(load8(dst, p00 + c)) + int(load8(dst, p10 + c)) + int(load8(dst, p01 + c)) + int(load8(dst, p11 + c))
               store8(dst, (sum + 2) / 4, dp + c)
               c += 1
            }
            x += 1
         }
         y += 1
      }
      src_off = dst_off
      dst_off += next_w * next_h * 4
      prev_w, prev_h = next_w, next_h
      i += 1
   }
   dst
}

fn resize(dict img, int new_w, int new_h) any {
   "Resizes an image dictionary using bilinear interpolation."
   if !is_dict(img) { return 0 }
   def w, h = img.get("width"), img.get("height")
   def pixels = img.get("data")
   def new_pixels = malloc(new_w * new_h * 4)
   if !new_pixels { return 0 }
   def x_ratio, y_ratio = float(w - 1) / float(new_w), float(h - 1) / float(new_h)
   mut y = 0
   while y < new_h {
      mut x = 0
      while x < new_w {
         def px, py = float(x) * x_ratio, float(y) * y_ratio
         def x_l, x_h = int(floor(px)), int(ceil(px))
         def y_l, y_h = int(floor(py)), int(ceil(py))
         def x_weight, y_weight = px - float(x_l), py - float(y_l)
         mut c = 0
         while c < 4 {
            def a, b = float(load8(pixels, (y_l * w + x_l) * 4 + c)), float(load8(pixels, (y_l * w + x_h) * 4 + c))
            def d, e = float(load8(pixels, (y_h * w + x_l) * 4 + c)), float(load8(pixels, (y_h * w + x_h) * 4 + c))
            def val = a * (1 - x_weight) * (1 - y_weight) +
            b * x_weight * (1 - y_weight) +
            d * y_weight * (1 - x_weight) +
            e * x_weight * y_weight
            store8(new_pixels, int(val), (y * new_w + x) * 4 + c)
            c += 1
         }
         x += 1
      }
      y += 1
   }
   return {"data": new_pixels, "width": new_w, "height": new_h, "channels": 4}
}

def RECT_PACK_HEURISTIC_BL = 0
def RECT_PACK_HEURISTIC_BF = 1

fn _node_new(list nodes, any x, any y, any nxt) int {
   def d = [x, y, nxt]
   def idx = nodes.len
   nodes.append(d)
   idx
}

fn _nx(list nodes, any idx) any { nodes.get(idx).get(0) }

fn _ny(list nodes, any idx) any { nodes.get(idx).get(1) }

fn _nn(list nodes, any idx) any { nodes.get(idx).get(2) }

fn _set_nxt(list nodes, any idx, any v) any { nodes.get(idx).set(2, v) }

fn _set_x(list nodes, any idx, any v) any { nodes.get(idx).set(0, v) }

fn rect_pack_init(any width, any height, any heuristic=0) dict {
   "Creates a new texture-atlas rect-pack context for a bin of `width` x `height`."
   def nodes = list(width + 4)
   _node_new(nodes, 0,     0,        1)
   _node_new(nodes, width, 0x3FFFFFFF, -1)
   return {
      "w": width,
      "h": height,
      "heur": heuristic,
      "nodes": nodes,
      "active": 0,
      "free": -1,
      "align": 1
   }
}

fn _skyline_find_min_y(list nodes, any first_idx, any x0, any width) list {
   def x1 = x0 + width
   mut node_idx = first_idx
   mut min_y = 0
   mut waste = 0
   mut visited_w = 0
   while _nx(nodes, node_idx) < x1 {
      def ny = _ny(nodes, node_idx)
      def nn_idx = _nn(nodes, node_idx)
      def nx2 = _nx(nodes, nn_idx)
      if ny > min_y {
         waste += visited_w * (ny - min_y)
         min_y = ny
         if _nx(nodes, node_idx) < x0 { visited_w += nx2 - x0 } else { visited_w += nx2 - _nx(nodes, node_idx) }
      } else {
         mut under_w = nx2 - _nx(nodes, node_idx)
         if under_w + visited_w > width { under_w = width - visited_w }
         waste += under_w * (min_y - ny)
         visited_w += under_w
      }
      node_idx = nn_idx
   }
   [min_y, waste]
}

fn _skyline_find_best_pos(dict ctx, any w, any h) list {
   def nodes  = ctx.get("nodes")
   def cw     = ctx.get("w")
   def ch     = ctx.get("h")
   def heur   = ctx.get("heur")
   def align  = ctx.get("align")
   mut aw = (w + align - 1)
   aw -= (aw % align)
   if aw > cw || h > ch { return [0, 0, 0, -1] }
   mut best_waste = 0x3FFFFFFF
   mut best_x = 0
   mut best_y = 0x3FFFFFFF
   mut best_prev = -2
   mut prev_idx = -1
   mut node_idx = ctx.get("active")
   while _nx(nodes, node_idx) + aw <= cw {
      def res = _skyline_find_min_y(nodes, node_idx, _nx(nodes, node_idx), aw)
      def y   = res.get(0)
      def wst = res.get(1)
      if heur == RECT_PACK_HEURISTIC_BL {
         if y < best_y {
            best_y    = y
            best_prev = prev_idx
            best_x    = _nx(nodes, node_idx)
         }
      } else {
         if y + h <= ch {
            if y < best_y || (y == best_y && wst < best_waste) {
               best_y    = y
               best_waste = wst
               best_prev = prev_idx
               best_x    = _nx(nodes, node_idx)
            }
         }
      }
      prev_idx = node_idx
      node_idx = _nn(nodes, node_idx)
   }
   if heur == RECT_PACK_HEURISTIC_BF {
      mut tail_idx = ctx.get("active")
      mut pv2 = -1
      mut nd2 = ctx.get("active")
      while _nx(nodes, tail_idx) < aw { tail_idx = _nn(nodes, tail_idx) }
      while tail_idx != -1 {
         def xpos = _nx(nodes, tail_idx) - aw
         if xpos < 0 { tail_idx = _nn(nodes, tail_idx) }
         while _nx(nodes, _nn(nodes, nd2)) <= xpos {
            pv2 = nd2
            nd2 = _nn(nodes, nd2)
         }
         def res = _skyline_find_min_y(nodes, nd2, xpos, aw)
         def y   = res.get(0)
         def wst = res.get(1)
         if y + h <= ch && y <= best_y {
            if y < best_y || wst < best_waste || (wst == best_waste && xpos < best_x) {
               best_x, best_y = xpos, y
               best_waste = wst
               best_prev = pv2
            }
         }
         tail_idx = _nn(nodes, tail_idx)
      }
   }
   if best_prev == -2 { return [0, 0, 0, -1] }
   [1, best_x, best_y, best_prev]
}

fn _skyline_pack_one(dict ctx, any w, any h) list {
   def nodes   = ctx.get("nodes")
   def ch      = ctx.get("h")
   def align   = ctx.get("align")
   mut aw = (w + align - 1)
   aw -= (aw % align)
   def res = _skyline_find_best_pos(ctx, w, h)
   def found   = res.get(0)
   def rx      = res.get(1)
   def ry      = res.get(2)
   def prev_idx = res.get(3)
   if !found || ry + h > ch { return [0, 0, 0] }
   def new_idx = _node_new(nodes, rx, ry + h, -1)
   def active = ctx.get("active")
   def cur_idx = (prev_idx == -1) ? active : _nn(nodes, prev_idx)
   if _nx(nodes, cur_idx) < rx {
      def after = _nn(nodes, cur_idx)
      _set_nxt(nodes, cur_idx, new_idx)
      _set_nxt(nodes, new_idx, after)
      mut scan = after
      while scan != -1 && _nn(nodes, scan) != -1 && _nx(nodes, _nn(nodes, scan)) <= rx + aw {
         def next = _nn(nodes, scan)
         scan = next
      }
      _set_nxt(nodes, new_idx, scan)
   } else {
      if prev_idx == -1 { ctx.set("active", new_idx) } else { _set_nxt(nodes, prev_idx, new_idx) }
      _set_nxt(nodes, new_idx, cur_idx)
      mut scan = cur_idx
      while scan != -1 && _nn(nodes, scan) != -1 && _nx(nodes, _nn(nodes, scan)) <= rx + aw { scan = _nn(nodes, scan) }
      _set_nxt(nodes, new_idx, scan)
      if scan != -1 && _nx(nodes, scan) < rx + aw { _set_x(nodes, scan, rx + aw) }
   }
   [1, rx, ry]
}

fn _sort_by_height(list rects) any {
   def n = rects.len
   mut i = 1
   while i < n {
      def key = rects.get(i)
      def kh = key.get("h")
      def kw = key.get("w")
      mut j = i - 1
      while j >= 0 {
         def rj = rects.get(j)
         def rjh = rj.get("h")
         def rjw = rj.get("w")
         if rjh > kh || (rjh == kh && rjw >= kw) { break }
         rects.set(j + 1, rj)
         j -= 1
      }
      rects.set(j + 1, key)
      i += 1
   }
}

fn rect_pack(dict ctx, list rects) int {
   "Packs texture-atlas rect dicts {id, w, h} into ctx. Sets x, y, packed on each."
   def n = rects.len
   mut i = 0
   while i < n {
      rects.get(i).set("_ord", i)
      i += 1
   }
   _sort_by_height(rects)
   mut all_packed = 1
   i = 0
   while i < n {
      def r  = rects.get(i)
      def rw = r.get("w")
      def rh = r.get("h")
      if rw == 0 || rh == 0 {
         r.set("x", 0)
         r.set("y", 0)
         r.set("packed", 1)
      } else {
         def res = _skyline_pack_one(ctx, rw, rh)
         if res.get(0) {
            r.set("x", res.get(1))
            r.set("y", res.get(2))
            r.set("packed", 1)
         } else {
            r.set("x", 0)
            r.set("y", 0)
            r.set("packed", 0)
            all_packed = 0
         }
      }
      i += 1
   }
   def sorted = list(n)
   i = 0
   while i < n { sorted.append(0) i += 1 }
   i = 0
   while i < n {
      def r = rects.get(i)
      sorted.set(r.get("_ord"), r)
      i += 1
   }
   i = 0
   while i < n { rects.set(i, sorted.get(i)) i += 1 }
   all_packed
}

@jit
fn _v3_norm(any v) list {
   def x = float(v.get(0, 0.0))
   def y = float(v.get(1, 0.0))
   def z = float(v.get(2, 0.0))
   def l = sqrt(x * x + y * y + z * z)
   if l <= 0.000000001 { return [0.0, 0.0, 0.0] }
   [x / l, y / l, z / l]
}

@jit
fn _v3_dot(any a, any b) f64 {
   float(a.get(0, 0.0)) * float(b.get(0, 0.0)) +
   float(a.get(1, 0.0)) * float(b.get(1, 0.0)) +
   float(a.get(2, 0.0)) * float(b.get(2, 0.0))
}

@jit
fn _v3_cross(any a, any b) list {
   def ax, ay = float(a.get(0, 0.0)), float(a.get(1, 0.0))
   def az = float(a.get(2, 0.0))
   def bx, by = float(b.get(0, 0.0)), float(b.get(1, 0.0))
   def bz = float(b.get(2, 0.0))
   [ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx]
}

@jit
fn srgb_to_linear_chan(any x) f64 {
   "Converts a normalized sRGB channel to linear."
   def c = clamp(float(x), 0.0, 1.0)
   if c <= 0.04045 { return c / 12.92 }
   pow((c + 0.055) / 1.055, 2.4)
}

@jit
fn linear_to_srgb_chan(any x) f64 {
   "Converts a normalized linear channel to sRGB."
   mut c = float(x)
   if is_nan(c) { c = 0.0 }
   if c < 0.0 { c = 0.0 } elif c > 1.0 { c = 1.0 }
   if c <= 0.0031308 { return c * 12.92 }
   1.055 * pow(c, 1.0 / 2.4) - 0.055
}

@jit
fn linear_to_srgb_u8(any x) int {
   "Converts a normalized linear channel to an 8-bit sRGB channel."
   def y = linear_to_srgb_chan(x)
   if is_nan(y) { return 0 }
   clamp(int(y * 255.0 + 0.5), 0, 255)
}

@jit
fn image_sample_linear_rgb_uv(any im, any u, any v) list {
   "Samples an RGBA image dictionary in linear RGB using wrapped U and clamped V."
   if !is_dict(im) { return [0.0, 0.0, 0.0] }
   def data = im.get("data", 0)
   def w = int(im.get("width", 0))
   def h = int(im.get("height", 0))
   if !data || !is_str(data) || w <= 0 || h <= 0 { return [0.0, 0.0, 0.0] }
   mut uu = float(u) - floor(float(u))
   mut vv = clamp(float(v), 0.0, 1.0)
   def fx, fy = uu * float(w) - 0.5, vv * float(h) - 0.5
   mut x0, y0 = int(floor(fx)), int(floor(fy))
   mut x1, y1 = x0 + 1, y0 + 1
   def tx, ty = fx - float(x0), fy - float(y0)
   while x0 < 0 { x0 += w }
   while x1 < 0 { x1 += w }
   x0, x1 = x0 % w, x1 % w
   if y0 < 0 { y0 = 0 }
   if y1 < 0 { y1 = 0 }
   if y0 >= h { y0 = h - 1 }
   if y1 >= h { y1 = h - 1 }
   def i00, i10 = ((y0 * w) + x0) * 4, ((y0 * w) + x1) * 4
   def i01, i11 = ((y1 * w) + x0) * 4, ((y1 * w) + x1) * 4
   def c00 = [
      srgb_to_linear_chan(float(load8(data, i00 + 0) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i00 + 1) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i00 + 2) & 255) / 255.0)
   ]
   def c10 = [
      srgb_to_linear_chan(float(load8(data, i10 + 0) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i10 + 1) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i10 + 2) & 255) / 255.0)
   ]
   def c01 = [
      srgb_to_linear_chan(float(load8(data, i01 + 0) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i01 + 1) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i01 + 2) & 255) / 255.0)
   ]
   def c11 = [
      srgb_to_linear_chan(float(load8(data, i11 + 0) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i11 + 1) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i11 + 2) & 255) / 255.0)
   ]
   def a0 = [
      c00.get(0, 0.0) + (c10.get(0, 0.0) - c00.get(0, 0.0)) * tx,
      c00.get(1, 0.0) + (c10.get(1, 0.0) - c00.get(1, 0.0)) * tx,
      c00.get(2, 0.0) + (c10.get(2, 0.0) - c00.get(2, 0.0)) * tx
   ]
   def a1 = [
      c01.get(0, 0.0) + (c11.get(0, 0.0) - c01.get(0, 0.0)) * tx,
      c01.get(1, 0.0) + (c11.get(1, 0.0) - c01.get(1, 0.0)) * tx,
      c01.get(2, 0.0) + (c11.get(2, 0.0) - c01.get(2, 0.0)) * tx
   ]
   [
      a0.get(0, 0.0) + (a1.get(0, 0.0) - a0.get(0, 0.0)) * ty,
      a0.get(1, 0.0) + (a1.get(1, 0.0) - a0.get(1, 0.0)) * ty,
      a0.get(2, 0.0) + (a1.get(2, 0.0) - a0.get(2, 0.0)) * ty
   ]
}

@jit
fn env_dir_to_uv(any d) list {
   "Converts a direction vector to equirectangular UV coordinates."
   def n = _v3_norm(d)
   def x = float(n.get(0, 0.0))
   def y, z = float(n.get(1, 0.0)), float(n.get(2, 0.0))
   def u_raw = 0.5 + atan2(x, 0.0 - z) / 6.283185307179586
   def v = clamp(0.5 - asin(clamp(y, -1.0, 1.0)) / 3.141592653589793, 0.00001, 0.99999)
   [u_raw - floor(u_raw), v]
}

@jit
fn _radical_inverse_vdc32(any bits0) f64 {
   mut bits = int(bits0)
   bits = ((bits << 16) | ((bits >> 16) & 0xffff)) & 0xffffffff
   bits = (((bits & 0x55555555) << 1) | ((bits & 0xaaaaaaaa) >> 1)) & 0xffffffff
   bits = (((bits & 0x33333333) << 2) | ((bits & 0xcccccccc) >> 2)) & 0xffffffff
   bits = (((bits & 0x0f0f0f) << 4) | ((bits & 0xf0f0f0) >> 4)) & 0xffffffff
   bits = (((bits & 0x00ff00ff) << 8) | ((bits & 0xff00ff00) >> 8)) & 0xffffffff
   float(bits) * 2.3283064365386963e-10
}

@jit
fn _importance_sample_ggx(any xi_x, any xi_y, any roughness, any N) list {
   def a = roughness * roughness
   def a2 = a * a
   def phi = 6.283185307179586 * xi_x
   def cos_theta = sqrt((1.0 - xi_y) / max(1.0 + (a2 - 1.0) * xi_y, 1e-6))
   def sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0))
   def H = [cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta]
   def Nz = float(N.get(2, 0.0))
   def up = (abs(Nz) < 0.999) ? [0.0, 0.0, 1.0] : [1.0, 0.0, 0.0]
   def tangent = _v3_norm(_v3_cross(up, N))
   def bitangent = _v3_cross(N, tangent)
   _v3_norm([
         tangent.get(0, 0.0) * H.get(0, 0.0) + bitangent.get(0, 0.0) * H.get(1, 0.0) + float(N.get(0, 0.0)) * H.get(2, 0.0),
         tangent.get(1, 0.0) * H.get(0, 0.0) + bitangent.get(1, 0.0) * H.get(1, 0.0) + float(N.get(1, 0.0)) * H.get(2, 0.0),
         tangent.get(2, 0.0) * H.get(0, 0.0) + bitangent.get(2, 0.0) * H.get(1, 0.0) + float(N.get(2, 0.0)) * H.get(2, 0.0)
   ])
}

fn generate_spec_env_slab(any im, int base_w=256) any {
   "Generates a packed specular environment mip slab from an equirectangular RGBA image."
   if !is_dict(im) { return 0 }
   def src_w, src_h = int(im.get("width", 0)), int(im.get("height", 0))
   if src_w <= 0 || src_h <= 0 { return 0 }
   mut w0 = clamp(int(base_w), 64, 512)
   if w0 > src_w { w0 = src_w }
   def h0 = max(1, w0 / 2)
   mut levels = 1
   mut tw = w0
   mut th = h0
   mut total = 0
   while true {
      total += tw * th * 4
      if tw <= 1 && th <= 1 { break }
      tw, th = max(1, tw >> 1), max(1, th >> 1)
      levels += 1
   }
   def slab = malloc(total)
   if !slab { return 0 }
   mut off = 0
   mut level = 0
   while level < levels {
      def w, h = max(1, w0 >> level), max(1, h0 >> level)
      def roughness = (levels > 1) ? float(level) / float(levels - 1) : 0.0
      mut sample_count = 1
      if level > 0 {
         if roughness < 0.15 { sample_count = 64 }
         elif roughness < 0.5 { sample_count = 32 }
         else { sample_count = 16 }
      }
      mut y = 0
      while y < h {
         def vv = (float(y) + 0.5) / float(h)
         def elev = (0.5 - vv) * 3.141592653589793
         def sin_e = sin(elev)
         def cos_e = cos(elev)
         mut x = 0
         while x < w {
            def uu = (float(x) + 0.5) / float(w)
            def phi = (uu - 0.5) * 6.283185307179586
            def N = _v3_norm([cos_e * cos(phi), sin_e, cos_e * sin(phi)])
            mut c0, c1 = 0.0, 0.0
            mut c2 = 0.0
            mut weight = 0.0
            if roughness <= 0.0 || sample_count <= 1 {
               def uv = env_dir_to_uv(N)
               def s = image_sample_linear_rgb_uv(im, uv.get(0, 0.0), uv.get(1, 0.0))
               c0, c1 = s.get(0, 0.0), s.get(1, 0.0)
               c2 = s.get(2, 0.0)
               weight = 1.0
            } else {
               mut i = 0
               while i < sample_count {
                  def xi_x, xi_y = float(i) / float(sample_count), _radical_inverse_vdc32(i)
                  def H = _importance_sample_ggx(xi_x, xi_y, roughness, N)
                  def VoH = max(_v3_dot(N, H), 0.0)
                  def L = _v3_norm([
                        2.0 * VoH * H.get(0, 0.0) - N.get(0, 0.0),
                        2.0 * VoH * H.get(1, 0.0) - N.get(1, 0.0),
                        2.0 * VoH * H.get(2, 0.0) - N.get(2, 0.0)
                  ])
                  def NoL = max(_v3_dot(N, L), 0.0)
                  if NoL > 0.0 {
                     def uv = env_dir_to_uv(L)
                     def s = image_sample_linear_rgb_uv(im, uv.get(0, 0.0), uv.get(1, 0.0))
                     c0 += s.get(0, 0.0) * NoL
                     c1 += s.get(1, 0.0) * NoL
                     c2 += s.get(2, 0.0) * NoL
                     weight += NoL
                  }
                  i += 1
               }
            }
            if weight > 0.0 {
               c0, c1 = c0 / weight, c1 / weight
               c2 = c2 / weight
            }
            def dp = off + ((y * w) + x) * 4
            store8(slab, linear_to_srgb_u8(c0), dp + 0)
            store8(slab, linear_to_srgb_u8(c1), dp + 1)
            store8(slab, linear_to_srgb_u8(c2), dp + 2)
            store8(slab, 255, dp + 3)
            x += 1
         }
         y += 1
      }
      off += w * h * 4
      level += 1
   }
   mut out = dict(8)
   out["pixels"] = slab
   out["width"] = w0
   out["height"] = h0
   out["levels"] = levels
   out["bytes"] = total
   out
}

fn _rgba_image_result(any pixels, int w, int h) dict {
   mut out = dict(8)
   out["data"] = init_str(pixels, w * h * 4)
   out["width"] = w
   out["height"] = h
   out["channels"] = 4
   out
}

fn generate_env_image(int kind=0, int w=1024, int h=512) any {
   "Generates a procedural RGBA environment image."
   def iw, ih = max(1, int(w)), max(1, int(h))
   def pixels = malloc(iw * ih * 4)
   if !pixels { return 0 }
   def fw, fh = float(iw), float(ih)
   mut y = 0
   while y < ih {
      def v = (float(y) + 0.5) / fh
      def elev = (0.5 - v) * 3.141592653589793
      def dy = sin(elev)
      def sky_t = clamp(dy * 0.5 + 0.5, 0.0, 1.0)
      def top_t = clamp(dy, 0.0, 1.0)
      def floor_t = clamp(-dy, 0.0, 1.0)
      def row = y * iw * 4
      mut x = 0
      while x < iw {
         def u = (float(x) + 0.5) / fw
         mut c0, c1 = 0.0, 0.0
         mut c2 = 0.0
         if kind == 0 {
            c0, c1 = (0.58 * (1.0 - sky_t)) + ((0.78 + 0.035 * top_t) * sky_t), (0.60 * (1.0 - sky_t)) + ((0.80 + 0.036 * top_t) * sky_t)
            c2 = (0.66 * (1.0 - sky_t)) + ((0.86 + 0.040 * top_t) * sky_t)
            def key1_dx, key1_dy = (u - 0.74) / 0.055, (v - 0.27) / 0.085
            def key1 = exp(-(key1_dx * key1_dx + key1_dy * key1_dy))
            def key2_dx = (u - 0.28) / 0.070
            def key2_dy = (v - 0.30) / 0.095
            def key2 = exp(-(key2_dx * key2_dx + key2_dy * key2_dy))
            def top_strip = exp(-pow((v - 0.17) / 0.040, 2.0)) * exp(-pow((u - 0.50) / 0.32, 6.0))
            def horizon = exp(-pow((v - 0.50) / 0.11, 2.0)) * 0.012
            c0 += key1 * 0.10 + key2 * 0.08 + top_strip * 0.034 + horizon
            c1 += key1 * 0.10 + key2 * 0.08 + top_strip * 0.034 + horizon
            c2 += key1 * 0.11 + key2 * 0.09 + top_strip * 0.038 + horizon
         } elif kind == 1 {
            c0, c1 = 0.70 + 0.15 * top_t - 0.08 * floor_t, 0.66 + 0.14 * top_t - 0.07 * floor_t
            c2 = 0.74 + 0.16 * top_t - 0.04 * floor_t
            def broad1_dx, broad1_dy = (u - 0.72) / 0.18, (v - 0.24) / 0.16
            def broad1 = exp(-(broad1_dx * broad1_dx + broad1_dy * broad1_dy))
            def broad2_dx = (u - 0.28) / 0.18
            def broad2_dy = (v - 0.30) / 0.18
            def broad2 = exp(-(broad2_dx * broad2_dx + broad2_dy * broad2_dy))
            def top_strip = exp(-pow((v - 0.16) / 0.040, 2.0)) * exp(-pow((u - 0.50) / 0.34, 6.0))
            def horizon = exp(-pow((v - 0.56) / 0.14, 2.0))
            def warm_floor = exp(-pow((v - 0.82) / 0.16, 2.0))
            c0 += broad1 * 0.16 + broad2 * 0.12 + top_strip * 0.08 + horizon * 0.06 + warm_floor * 0.05
            c1 += broad1 * 0.12 + broad2 * 0.10 + top_strip * 0.06 + horizon * 0.04 + warm_floor * 0.03
            c2 += broad1 * 0.18 + broad2 * 0.16 + top_strip * 0.10 + horizon * 0.08 + warm_floor * 0.05
         } elif kind == 2 {
            mut l = 0.30 + 0.42 * top_t + 0.02 * floor_t
            def key_left_dx, key_left_dy = (u - 0.22) / 0.090, (v - 0.23) / 0.085
            def key_left = exp(-(key_left_dx * key_left_dx + key_left_dy * key_left_dy))
            def key_right_dx = (u - 0.78) / 0.095
            def key_right_dy = (v - 0.24) / 0.090
            def key_right = exp(-(key_right_dx * key_right_dx + key_right_dy * key_right_dy))
            def top_strip = exp(-pow((v - 0.15) / 0.070, 2.0)) * exp(-pow((u - 0.50) / 0.42, 4.0))
            def mid_soft = exp(-pow((v - 0.44) / 0.16, 2.0)) * exp(-pow((u - 0.50) / 0.36, 4.0))
            def floor_soft = exp(-pow((v - 0.82) / 0.18, 2.0))
            def center_shadow_x = max(abs((u - 0.50) / 0.25), abs((v - 0.63) / 0.30))
            def center_shadow = exp(-pow(center_shadow_x, 4.0))
            l += key_left * 1.05 + key_right * 1.00 + top_strip * 0.24 + mid_soft * 0.10 + floor_soft * 0.035
            l -= center_shadow * 0.035
            l = max(l, 0.075)
            def warm = key_left * 0.035 + floor_t * 0.018
            def cool = key_right * 0.025 + top_t * 0.020
            c0, c1 = l * 1.01 + warm, l * 1.00
            c2 = l * 1.02 + cool
         } else {
            c0, c1 = 0.18 + 0.08 * sky_t - 0.02 * floor_t, 0.18 + 0.08 * sky_t - 0.02 * floor_t
            c2 = 0.20 + 0.09 * sky_t - 0.02 * floor_t
            def soft1_dx, soft1_dy = (u - 0.24) / 0.060, (v - 0.23) / 0.055
            def soft1 = exp(-(soft1_dx * soft1_dx + soft1_dy * soft1_dy))
            def soft2_dx = (u - 0.76) / 0.060
            def soft2_dy = (v - 0.23) / 0.055
            def soft2 = exp(-(soft2_dx * soft2_dx + soft2_dy * soft2_dy))
            def fill_dx = (u - 0.50) / 0.20
            def fill_dy = (v - 0.19) / 0.08
            def fill = exp(-(fill_dx * fill_dx + fill_dy * fill_dy))
            def warm_dx = (u - 0.50) / 0.30
            def warm_dy = (v - 0.74) / 0.16
            def warm = exp(-(warm_dx * warm_dx + warm_dy * warm_dy))
            def horizon = exp(-pow((v - 0.50) / 0.090, 2.0))
            c0 += soft1 * 1.14 + soft2 * 1.14 + fill * 0.30 + warm * 0.04 + horizon * 0.03
            c1 += soft1 * 1.14 + soft2 * 1.14 + fill * 0.30 + warm * 0.04 + horizon * 0.03
            c2 += soft1 * 1.16 + soft2 * 1.16 + fill * 0.31 + warm * 0.03 + horizon * 0.04
         }
         def p = row + x * 4
         store8(pixels, linear_to_srgb_u8(c0), p + 0)
         store8(pixels, linear_to_srgb_u8(c1), p + 1)
         store8(pixels, linear_to_srgb_u8(c2), p + 2)
         store8(pixels, 255, p + 3)
         x += 1
      }
      y += 1
   }
   _rgba_image_result(pixels, iw, ih)
}

fn generate_neutral_env_image(int w=1024, int h=512) any { generate_env_image(0, w, h) }

fn generate_compare_visible_env_image(int w=1024, int h=512) any { generate_env_image(1, w, h) }

fn generate_compare_reflect_env_image(int w=1024, int h=512) any { generate_env_image(2, w, h) }

fn generate_studio_env_image(int w=1024, int h=512) any { generate_env_image(3, w, h) }

fn scene_prefers_studio_env(any name) bool { render_env.scene_prefers_studio_env(name) }

fn scene_prefers_neutral_env(any name) bool { render_env.scene_prefers_neutral_env(name) }

fn scene_prefers_compare_reflect_env(any name) bool { render_env.scene_prefers_compare_reflect_env(name) }

fn scene_prefers_compare_visible_env(any name) bool { render_env.scene_prefers_compare_visible_env(name) }

fn scene_prefers_optical_spec_env(any name) bool { render_env.scene_prefers_optical_spec_env(name) }

fn scene_prefers_black_visible_env(any name) bool { render_env.scene_prefers_black_visible_env(name) }

fn scene_prefers_gray_proof_bg(any name) bool { render_env.scene_prefers_gray_proof_bg(name) }

fn tex_job_make(int index, any uri, any mime="", any sampler=0, int material=-1, any slot="") dict {
   "Creates a texture decode/upload planning job."
   def j = {
      "index": int(index), "uri": to_str(uri), "mime": to_str(mime),
      "sampler": sampler, "material": int(material), "slot": to_str(slot)
   }
   j
}

fn tex_job_queue_make() dict {
   "Creates an in-memory texture job queue."
   def q = {"head": 0, "items": list(0)}
   q
}

fn tex_job_queue_push(any q, any job) dict {
   "Pushes a texture job into a queue."
   if !is_dict(q) { q = tex_job_queue_make() }
   def items = q.get("items", [])
   q["items"] = items.append(job)
   q
}

fn tex_job_queue_pop(any q) any {
   "Pops the next texture job from a queue."
   if !is_dict(q) { return 0 }
   def items = q.get("items", [])
   mut head = int(q.get("head", 0))
   if !is_list(items) || head >= items.len { return 0 }
   def job = items.get(head, 0)
   q["head"] = head + 1
   job
}

fn tex_job_result_make(any job, int width, int height, any rgba_or_mips, bool ok=true, any err="") dict {
   "Creates a texture job result record."
   def r = {
      "job": job, "ok": ok ? true : false, "error": to_str(err),
      "width": int(width), "height": int(height), "pixels": rgba_or_mips
   }
   r
}

fn tex_job_cache_key(any uri, any mime="", int flags=0) str {
   "Builds a stable cache key for a decoded texture job."
   "ntex_" + to_str(hash.fnv1a(to_str(uri) + "|" + to_str(mime) + "|" + to_str(flags)))
}

fn tex_job_worker_plan(int worker_count=4) dict {
   "Describes how texture decode jobs should be split from renderer upload work."
   def p = {
      "workers": int(worker_count), "worker_touches_renderer": false,
      "worker_output": "decoded_rgba_or_prebaked_mip_slab",
      "main_thread_upload": true, "preserve_material_ids": true
   }
   p
}

fn tex_job_upload_plan(any results) list {
   "Filters completed texture job results down to uploadable records."
   if !is_list(results) { return [] }
   mut out = list(0)
   mut i = 0
   def results_n = results.len
   while i < results_n {
      def r = results.get(i, 0)
      if is_dict(r) && r.get("ok", false) { out = out.append(r) }
      i += 1
   }
   out
}
