;; Keywords: 3d meshopt meshoptimizer parse gltf
;; meshoptimizer decoding and mesh-compression support for glTF assets.
;; References:
;; - std.parse.3d
;; - std.parse
module std.parse.3d.meshopt(meshopt_process_mesh, meshopt_select_lod_cut)
use std.core
use std.math
use std.math.bin (f32le)
use std.core.str as str
use std.core.common as common

fn vec_push(list xs, any v) list { xs.append(v) }

fn _filled_list(int count, any value) list {
   mut out = list(count)
   mut i = 0
   while i < count {
      out[i] = value
      i += 1
   }
   out
}

mut _meshopt_trace_cache = -1

fn _meshopt_trace_enabled() bool {
   _meshopt_trace_cache = common.cached_env_truthy(_meshopt_trace_cache, "NY_MESHOPT_TRACE")
   _meshopt_trace_cache == 1
}

layout MeshletBounds {
   f32 center_x,
   f32 center_y,
   f32 center_z,
   f32 radius,
   f32 cone_ax,
   f32 cone_ay,
   f32 cone_az,
   f32 cone_cutoff
}

layout Meshlet {
   u32 vertex_offset,
   u32 triangle_offset,
   u32 vertex_count,
   u32 triangle_count,
   MeshletBounds bounds,
   f32 cluster_error,
   f32 parent_error,
   u32 lod_level
}

fn _px(any vbuf, int vi, int stride) any { f32le(vbuf, vi * stride) }

fn _py(any vbuf, int vi, int stride) any { f32le(vbuf, vi * stride + 4) }

fn _pz(any vbuf, int vi, int stride) any { f32le(vbuf, vi * stride + 8) }

fn _len3(any x, any y, any z) any { sqrt(x*x + y*y + z*z) }

fn _dot3(any ax, any ay, any az, any bx, any by, any bz) any { ax*bx + ay*by + az*bz }

fn _norm3(any x, any y, any z) list {
   def l = _len3(x, y, z)
   if l < 1e-15 { return [0.0, 1.0, 0.0] }
   [x/l, y/l, z/l]
}

fn _build_adj(list indices, int idx_count, int vcnt) dict {
   mut counts = _filled_list(vcnt, 0)
   mut offsets = _filled_list(vcnt, 0)
   mut data = _filled_list(idx_count, 0)
   def tri_count = idx_count / 3
   mut k = 0
   while k < idx_count {
      def v = int(indices.get(k))
      counts[v] = counts.get(v) + 1
      k += 1
   }
   mut off = 0
   k = 0
   while k < vcnt {
      offsets[k] = off
      off += counts.get(k)
      k += 1
   }
   mut write_pos = list(vcnt)
   k = 0
   while k < vcnt {
      write_pos[k] = offsets.get(k)
      k += 1
   }
   mut i = 0
   while i < tri_count {
      def a, b = int(indices.get(i*3)), int(indices.get(i*3+1))
      def c = int(indices.get(i*3+2))
      data[write_pos.get(a)] = i
      write_pos[a] = write_pos.get(a) + 1
      data[write_pos.get(b)] = i
      write_pos[b] = write_pos.get(b) + 1
      data[write_pos.get(c)] = i
      write_pos[c] = write_pos.get(c) + 1
      i += 1
   }
   return { "counts": counts, "offsets": offsets, "data": data }
}

def _CACHE  = [0.0, 0.779, 0.791, 0.789, 0.981, 0.843, 0.726, 0.847,
0.882, 0.867, 0.799, 0.642, 0.613, 0.600, 0.568, 0.372, 0.234]
def _VALENCE = [0.0, 0.995, 0.713, 0.450, 0.404, 0.059, 0.005, 0.147, 0.006]

fn _vscore(int cache_pos, int live) any {
   def cs = _CACHE.get(cache_pos + 1)
   def lv = live < 8 ? live : 8
   cs + _VALENCE.get(lv)
}

fn meshopt_optimize_vertex_cache(list indices, int vertex_count) list {
   "Runs the meshopt optimize vertex cache operation."
   def idx_count = indices.len
   def tri_count = idx_count / 3
   if tri_count == 0 { return indices }
   def adj = _build_adj(indices, idx_count, vertex_count)
   def live   = adj.counts
   def offsets = adj.offsets
   def adj_data = adj.data
   mut emitted = _filled_list(tri_count, 0)
   mut vscores = _filled_list(vertex_count, 0.0)
   mut tscores = _filled_list(tri_count, 0.0)
   mut dest = _filled_list(idx_count, 0)
   mut i = 0
   while i < vertex_count {
      vscores = vec_push(vscores, _vscore(-1, live.get(i)))
      i += 1
   }
   i = 0
   while i < tri_count {
      def a, b = int(indices.get(i*3)), int(indices.get(i*3+1))
      def c = int(indices.get(i*3+2))
      tscores = vec_push(tscores, vscores.get(a) + vscores.get(b) + vscores.get(c))
      i += 1
   }
   def cache_sz = 16
   mut cache = [-1]*cache_sz
   mut cache_cnt = 0
   mut cur = 0
   mut out_tri = 0
   mut fallback_cursor = 1
   while cur != -1 {
      def ta, tb = int(indices.get(cur*3)), int(indices.get(cur*3+1))
      def tc = int(indices.get(cur*3+2))
      dest[out_tri*3] = ta
      dest[out_tri*3+1] = tb
      dest[out_tri*3+2] = tc
      out_tri += 1
      emitted[cur] = 1
      tscores[cur] = 0.0
      mut new_cache = [ta, tb, tc]
      i = 0
      while i < cache_cnt {
         def cv = cache.get(i)
         if cv != ta && cv != tb && cv != tc { new_cache = vec_push(new_cache, cv) }
         i += 1
      }
      cache_cnt = new_cache.len
      if cache_cnt > cache_sz { cache_cnt = cache_sz }
      i = 0
      while i < cache_cnt {
         cache[i] = new_cache.get(i)
         i += 1
      }
      mut k3 = 0
      while k3 < 3 {
         def vi = int(indices.get(cur*3+k3))
         def base = offsets.get(vi)
         def cnt  = live.get(vi)
         mut ai = 0
         while ai < cnt {
            if adj_data.get(base+ai) == cur {
               adj_data[base+ai] = adj_data.get(base+cnt-1)
               live[vi] = cnt - 1
               break
            }
            ai += 1
         }
         k3 += 1
      }
      mut best_tri = -1
      mut best_score = 0.0
      i = 0
      while i < cache_cnt {
         def vi = cache.get(i)
         def cnt = live.get(vi)
         if cnt == 0 {
            i += 1
            continue
         }
         def cp = i < cache_sz ? i : -1
         def ns = _vscore(cp, cnt)
         def diff = ns - vscores.get(vi)
         vscores[vi] = ns
         def base = offsets.get(vi)
         mut ai = 0
         while ai < live.get(vi) {
            def t = int(adj_data.get(base+ai))
            if emitted.get(t) == 0 {
               def ts = tscores.get(t) + diff
               tscores[t] = ts
               if ts > best_score {
                  best_score = ts
                  best_tri = t
               }
            }
            ai += 1
         }
         i += 1
      }
      cur = best_tri
      if cur == -1 {
         while fallback_cursor < tri_count {
            if emitted.get(fallback_cursor) == 0 {
               cur = fallback_cursor
               fallback_cursor += 1
               break
            }
            fallback_cursor += 1
         }
      }
   }
   dest
}

fn _compute_sphere(any vbuf, list idx_list, int pos_stride) dict {
   def n = idx_list.len
   if n == 0 { return { "cx":0.0, "cy":0.0, "cz":0.0, "r":0.0 } }
   mut cx, cy = _px(vbuf, idx_list.get(0), pos_stride), _py(vbuf, idx_list.get(0), pos_stride)
   mut cz = _pz(vbuf, idx_list.get(0), pos_stride)
   mut r  = 0.0
   mut pass = 0
   while pass < 2 {
      mut i = 0
      while i < n {
         def v = idx_list.get(i)
         def dx = _px(vbuf, v, pos_stride) - cx
         def dy = _py(vbuf, v, pos_stride) - cy
         def dz = _pz(vbuf, v, pos_stride) - cz
         def d  = _len3(dx, dy, dz)
         if d > r {
            def excess = (d - r) * 0.5
            r  += excess
            def inv = excess / d
            cx += dx * inv
            cy += dy * inv
            cz += dz * inv
         }
         i += 1
      }
      pass += 1
   }
   return { "cx":cx, "cy":cy, "cz":cz, "r":r }
}

fn _compute_cone(any vbuf, list tri_indices, int pos_stride) dict {
   mut anx, any = 0.0, 0.0
   mut anz = 0.0
   mut ti = 0
   def tri_count = tri_indices.len / 3
   while ti < tri_count {
      def i0, i1 = tri_indices.get(ti*3), tri_indices.get(ti*3+1)
      def i2 = tri_indices.get(ti*3+2)
      def ax = _px(vbuf, i0, pos_stride)
      def ay = _py(vbuf, i0, pos_stride)
      def az = _pz(vbuf, i0, pos_stride)
      def bx = _px(vbuf, i1, pos_stride) - ax
      def by = _py(vbuf, i1, pos_stride) - ay
      def bz = _pz(vbuf, i1, pos_stride) - az
      def cx = _px(vbuf, i2, pos_stride) - ax
      def cy = _py(vbuf, i2, pos_stride) - ay
      def cz = _pz(vbuf, i2, pos_stride) - az
      anx += by*cz - bz*cy
      any += bz*cx - bx*cz
      anz += bx*cy - by*cx
      ti += 1
   }
   def nn = _norm3(anx, any, anz)
   def nax = nn.get(0)
   def nay = nn.get(1)
   def naz = nn.get(2)
   mut cutoff = 1.0
   ti = 0
   while ti < tri_count {
      def i0, i1 = tri_indices.get(ti*3), tri_indices.get(ti*3+1)
      def i2 = tri_indices.get(ti*3+2)
      def ax = _px(vbuf, i0, pos_stride)
      def ay = _py(vbuf, i0, pos_stride)
      def az = _pz(vbuf, i0, pos_stride)
      def bx = _px(vbuf, i1, pos_stride) - ax
      def by = _py(vbuf, i1, pos_stride) - ay
      def bz = _pz(vbuf, i1, pos_stride) - az
      def cx = _px(vbuf, i2, pos_stride) - ax
      def cy = _py(vbuf, i2, pos_stride) - ay
      def cz = _pz(vbuf, i2, pos_stride) - az
      def fnx = by*cz - bz*cy
      def fny = bz*cx - bx*cz
      def fnz = bx*cy - by*cx
      def fn_norm = _norm3(fnx, fny, fnz)
      def d = _dot3(fn_norm.get(0), fn_norm.get(1), fn_norm.get(2), nax, nay, naz)
      if d < cutoff { cutoff = d }
      ti += 1
   }
   return { "ax":nax, "ay":nay, "az":naz, "cutoff":cutoff }
}

fn _qem_from_plane(any nx, any ny, any nz, any d) list {
   [nx*nx, nx*ny, nx*nz, nx*d,
      ny*ny, ny*nz, ny*d,
      nz*nz, nz*d,
   d*d]
}

fn _qem_add(list a, list b) list {
   [a.get(0)+b.get(0), a.get(1)+b.get(1), a.get(2)+b.get(2), a.get(3)+b.get(3),
      a.get(4)+b.get(4), a.get(5)+b.get(5), a.get(6)+b.get(6),
      a.get(7)+b.get(7), a.get(8)+b.get(8),
   a.get(9)+b.get(9)]
}

fn _qem_eval(list q, any x, any y, any z) any {
   def a=q.get(0) def b=q.get(1) def c=q.get(2) def d=q.get(3)
   def e=q.get(4) def f=q.get(5) def g=q.get(6)
   def h=q.get(7) def ii=q.get(8)
   def j=q.get(9)
   x*(a*x+b*y+c*z+d)+y*(b*x+e*y+f*z+g)+z*(c*x+f*y+h*z+ii)+(d*x+g*y+ii*z+j)
}

def _QEM_ZERO = [0.0,0.0,0.0,0.0, 0.0,0.0,0.0, 0.0,0.0, 0.0]

fn meshopt_simplify(list indices, any vbuf, int pos_stride, int target_tris) dict {
   "Runs the meshopt simplify operation."
   def tc = indices.len / 3
   if tc <= target_tris { return { "indices": indices, "error": 0.0 } }
   if _meshopt_trace_enabled() { print("[meshopt:simplify] tri_count=" + to_str(tc) + " -> " + to_str(target_tris)) }
   mut max_v = 0
   mut i = 0
   while i < indices.len {
      def v = int(indices.get(i))
      if v > max_v { max_v = v }
      i += 1
   }
   def vcnt = max_v + 1
   mut qs = _filled_list(vcnt, _QEM_ZERO)
   def tri_count = indices.len / 3
   i = 0
   while i < tri_count {
      def i0, i1 = int(indices.get(i*3)), int(indices.get(i*3+1))
      def i2 = int(indices.get(i*3+2))
      def ax = _px(vbuf, i0, pos_stride)
      def ay = _py(vbuf, i0, pos_stride)
      def az = _pz(vbuf, i0, pos_stride)
      def bx = _px(vbuf, i1, pos_stride) - ax
      def by = _py(vbuf, i1, pos_stride) - ay
      def bz = _pz(vbuf, i1, pos_stride) - az
      def cx = _px(vbuf, i2, pos_stride) - ax
      def cy = _py(vbuf, i2, pos_stride) - ay
      def cz = _pz(vbuf, i2, pos_stride) - az
      def nx = by*cz - bz*cy
      def ny = bz*cx - bx*cz
      def nz = bx*cy - by*cx
      def nl = _len3(nx, ny, nz)
      if nl > 1e-12 {
         def inv = 1.0 / nl
         def fnx = nx*inv
         def fny = ny*inv
         def fnz = nz*inv
         def fd  = -(fnx*ax + fny*ay + fnz*az)
         def q   = _qem_from_plane(fnx, fny, fnz, fd)
         qs[i0] = _qem_add(qs.get(i0), q)
         qs[i1] = _qem_add(qs.get(i1), q)
         qs[i2] = _qem_add(qs.get(i2), q)
      }
      i += 1
   }
   mut collapse = list(vcnt)
   i = 0
   while i < vcnt {
      collapse[i] = i
      i += 1
   }
   mut cur_idx = list(0)
   i = 0
   while i < indices.len {
      cur_idx = vec_push(cur_idx, indices.get(i))
      i += 1
   }
   mut cur_tris = tri_count
   mut max_error = 0.0
   while cur_tris > target_tris {
      mut best_cost = 1e30
      mut best_va = -1
      mut best_vb = -1
      mut best_mx = 0.0
      mut best_my = 0.0
      mut best_mz = 0.0
      mut t = 0
      while t < cur_tris {
         mut e = 0
         while e < 3 {
            def va, vb = int(cur_idx.get(t*3 + e)), int(cur_idx.get(t*3 + ((e+1)%3)))
            def mx = (_px(vbuf, va, pos_stride) + _px(vbuf, vb, pos_stride)) * 0.5
            def my = (_py(vbuf, va, pos_stride) + _py(vbuf, vb, pos_stride)) * 0.5
            def mz = (_pz(vbuf, va, pos_stride) + _pz(vbuf, vb, pos_stride)) * 0.5
            def qab = _qem_add(qs.get(va), qs.get(vb))
            def cost = _qem_eval(qab, mx, my, mz)
            if cost < best_cost {
               best_cost = cost
               best_va = va
               best_vb = vb
               best_mx = mx
               best_my = my
               best_mz = mz
            }
            e += 1
         }
         t += 1
      }
      if best_va < 0 { break }
      if best_cost > max_error { max_error = best_cost }
      collapse[best_vb] = best_va
      qs[best_va] = _qem_add(qs.get(best_va), qs.get(best_vb))
      mut new_idx = []
      mut t2 = 0
      while t2 < cur_tris {
         mut v0, v1 = int(cur_idx.get(t2*3)), int(cur_idx.get(t2*3+1))
         mut v2 = int(cur_idx.get(t2*3+2))
         mut lim = 32
         while collapse.get(v0) != v0 && lim > 0 {
            v0 = collapse.get(v0)
            lim -= 1
         }
         lim = 32
         while collapse.get(v1) != v1 && lim > 0 {
            v1 = collapse.get(v1)
            lim -= 1
         }
         lim = 32
         while collapse.get(v2) != v2 && lim > 0 {
            v2 = collapse.get(v2)
            lim -= 1
         }
         if v0 != v1 && v1 != v2 && v0 != v2 {
            new_idx = vec_push(new_idx, v0)
            new_idx = vec_push(new_idx, v1)
            new_idx = vec_push(new_idx, v2)
         }
         t2 += 1
      }
      cur_tris = new_idx.len / 3
      cur_idx = new_idx
   }
   return { "indices": cur_idx, "error": max_error }
}

fn _meshlet_find_seed(list emitted, int tri_count, int unseen) list {
   mut u = unseen
   while u < tri_count {
      if emitted.get(u) == 0 { return [u, u + 1] }
      u += 1
   }
   [-1, u]
}

fn _meshlet_local_vertex(list used, list m_verts, int gi) list {
   mut li = used.get(gi)
   if li < 0 {
      li = m_verts.len
      used[gi] = li
      m_verts = vec_push(m_verts, gi)
   }
   [li, m_verts]
}

fn _meshlet_append_tri(list indices, int t, list used, list live, list emitted, list m_verts, list m_tris) list {
   def gi0 = int(indices.get(t*3))
   def gi1 = int(indices.get(t*3+1))
   def gi2 = int(indices.get(t*3+2))
   def lv0 = _meshlet_local_vertex(used, m_verts, gi0)
   def li0 = int(lv0.get(0))
   m_verts = lv0.get(1)
   def lv1 = _meshlet_local_vertex(used, m_verts, gi1)
   def li1 = int(lv1.get(0))
   m_verts = lv1.get(1)
   def lv2 = _meshlet_local_vertex(used, m_verts, gi2)
   def li2 = int(lv2.get(0))
   m_verts = lv2.get(1)
   m_tris = vec_push(m_tris, (li0 & 255) | ((li1 & 255) << 8) | ((li2 & 255) << 16))
   emitted[t] = 1
   live[gi0] = live.get(gi0) - 1
   live[gi1] = live.get(gi1) - 1
   live[gi2] = live.get(gi2) - 1
   [m_verts, m_tris]
}

fn _meshlet_best_neighbor(list indices, list adj_offsets, list adj_counts, list adj_data, list emitted, list live, list used, list m_verts, int max_verts) int {
   mut nb = -1
   mut best_pri = 5
   mut best_score = 0
   mut bvi = 0
   while bvi < m_verts.len {
      def bgv = m_verts.get(bvi)
      def b_base = adj_offsets.get(bgv)
      def b_cnt = adj_counts.get(bgv)
      mut bai = 0
      while bai < b_cnt {
         def bt = int(adj_data.get(b_base+bai))
         if emitted.get(bt) == 0 {
            def bi0, bi1 = int(indices.get(bt*3)), int(indices.get(bt*3+1))
            def bi2 = int(indices.get(bt*3+2))
            mut extra = 0
            if used.get(bi0) < 0 { extra += 1 }
            if used.get(bi1) < 0 { extra += 1 }
            if used.get(bi2) < 0 { extra += 1 }
            if m_verts.len + extra <= max_verts {
               mut pri = 2 + extra
               if extra == 0 { pri = 0 }
               elif live.get(bi0) == 1 || live.get(bi1) == 1 || live.get(bi2) == 1 { pri = 1 }
               def score = -extra
               if pri < best_pri || (pri == best_pri && score > best_score) {
                  best_pri = pri
                  best_score = score
                  nb = bt
               }
            }
         }
         bai += 1
      }
      bvi += 1
   }
   nb
}

fn _meshlet_flat_indices(list m_verts, list m_tris) list {
   mut flat = []
   mut ti_f = 0
   while ti_f < m_tris.len {
      def packed = m_tris.get(ti_f)
      def li0 = packed & 255
      def li1 = (packed >> 8) & 255
      def li2 = (packed >> 16) & 255
      flat = vec_push(flat, m_verts.get(li0))
      flat = vec_push(flat, m_verts.get(li1))
      flat = vec_push(flat, m_verts.get(li2))
      ti_f += 1
   }
   flat
}

fn _meshlet_flush(any vbuf, int pos_stride, int lod_level, any cluster_error, list used, list out_meshlets, list out_vert_data, list out_tri_data, list m_verts, list m_tris) list {
   def flat = _meshlet_flat_indices(m_verts, m_tris)
   def sphere = _compute_sphere(vbuf, flat, pos_stride)
   def cone = _compute_cone(vbuf, flat, pos_stride)
   def bnd = {
      "center_x": sphere.cx,
      "center_y": sphere.cy,
      "center_z": sphere.cz,
      "radius": sphere.r,
      "cone_ax": cone.ax,
      "cone_ay": cone.ay,
      "cone_az": cone.az,
      "cone_cutoff": cone.cutoff
   }
   def m = {
      "vertex_offset": out_vert_data.len,
      "triangle_offset": out_tri_data.len,
      "vertex_count": m_verts.len,
      "triangle_count": m_tris.len,
      "bounds": bnd,
      "cluster_error": cluster_error,
      "parent_error": 1e30,
      "lod_level": lod_level
   }
   out_meshlets = vec_push(out_meshlets, m)
   mut vi_f = 0
   while vi_f < m_verts.len {
      def gv = m_verts.get(vi_f)
      out_vert_data = vec_push(out_vert_data, gv)
      used[gv] = -1
      vi_f += 1
   }
   mut ti_emit = 0
   while ti_emit < m_tris.len {
      out_tri_data = vec_push(out_tri_data, m_tris.get(ti_emit))
      ti_emit += 1
   }
   [out_meshlets, out_vert_data, out_tri_data]
}

fn meshopt_build_meshlets(list indices, int vcnt, any vbuf, int pos_stride, int max_verts, int max_tris, int lod_level, any cluster_error) dict {
   "Runs the meshopt build meshlets operation."
   def tri_count = indices.len / 3
   if tri_count == 0 { return { "meshlets":[], "vertex_data":[], "triangle_data":[] } }
   def adj = _build_adj(indices, tri_count, vcnt)
   def adj_offsets = adj.offsets
   def adj_counts  = adj.counts
   def adj_data    = adj.data
   mut live = list(vcnt)
   mut i = 0
   while i < vcnt {
      live[i] = adj_counts.get(i)
      i += 1
   }
   mut emitted = _filled_list(tri_count, 0)
   mut out_meshlets = []
   mut out_vert_data = []
   mut out_tri_data = []
   mut used = _filled_list(vcnt, -1)
   mut m_verts = []
   mut m_tris = []
   mut unseen = 0
   while unseen < tri_count {
      def seed_res = _meshlet_find_seed(emitted, tri_count, unseen)
      def seed = int(seed_res.get(0))
      unseen = int(seed_res.get(1))
      if seed < 0 { break }
      def seed_append = _meshlet_append_tri(indices, seed, used, live, emitted, m_verts, m_tris)
      m_verts = seed_append.get(0)
      m_tris = seed_append.get(1)
      while true {
         if m_tris.len >= max_tris || m_verts.len >= max_verts - 2 { break }
         def nb = _meshlet_best_neighbor(indices, adj_offsets, adj_counts, adj_data, emitted, live, used, m_verts, max_verts)
         if nb < 0 { break }
         def nb_append = _meshlet_append_tri(indices, nb, used, live, emitted, m_verts, m_tris)
         m_verts = nb_append.get(0)
         m_tris = nb_append.get(1)
      }
      if m_tris.len > 0 {
         def flushed = _meshlet_flush(vbuf, pos_stride, lod_level, cluster_error, used, out_meshlets, out_vert_data, out_tri_data, m_verts, m_tris)
         out_meshlets = flushed.get(0)
         out_vert_data = flushed.get(1)
         out_tri_data = flushed.get(2)
         m_verts = []
         m_tris  = []
      }
   }
   return { "meshlets": out_meshlets, "vertex_data": out_vert_data, "triangle_data": out_tri_data }
}

fn _group_clusters(dict meshlets_result) list {
   def ms, mc = meshlets_result.meshlets, ms.len
   def target_group_sz = 4
   mut groups = []
   mut cur_grp = []
   mut ci = 0
   while ci < mc {
      cur_grp = vec_push(cur_grp, ci)
      if cur_grp.len >= target_group_sz || ci == mc - 1 {
         groups = vec_push(groups, cur_grp)
         cur_grp = []
      }
      ci += 1
   }
   groups
}

fn meshopt_build_lod_hierarchy(list indices, int vcnt, any vbuf, int pos_stride, int max_levels) dict {
   "Runs the meshopt build lod hierarchy operation."
   mut lods = []
   def opt0 = meshopt_optimize_vertex_cache(indices, vcnt)
   def lod0 = meshopt_build_meshlets(opt0, vcnt, vbuf, pos_stride, 128, 128, 0, 0.0)
   lods = vec_push(lods, { "level": 0, "result": lod0, "error": 0.0 })
   mut cur = list(0)
   mut i = 0
   while i < indices.len {
      cur = vec_push(cur, indices.get(i))
      i += 1
   }
   mut cur_tris  = indices.len / 3
   mut prev_err  = 0.0
   mut lod_level = 1
   while lod_level < max_levels && cur_tris > 64 {
      def prev_lod = lods.get(lod_level - 1)
      def groups   = _group_clusters(prev_lod.result)
      mut new_idx = []
      mut stuck = true
      mut gi = 0
      while gi < groups.len {
         def grp = groups.get(gi)
         def prev_result = prev_lod.result
         mut merged = []
         mut ci = 0
         while ci < grp.len {
            def m_idx = grp.get(ci)
            def m     = prev_result.meshlets.get(m_idx)
            def voff  = m.vertex_offset
            def toff  = m.triangle_offset
            def tc    = m.triangle_count
            mut ti = 0
            while ti < tc {
               def packed = prev_result.triangle_data.get(toff + ti)
               merged = vec_push(merged, prev_result.vertex_data.get(voff + (packed & 255)))
               merged = vec_push(merged, prev_result.vertex_data.get(voff + ((packed >> 8) & 255)))
               merged = vec_push(merged, prev_result.vertex_data.get(voff + ((packed >> 16) & 255)))
               ti += 1
            }
            ci += 1
         }
         def target_grp_tris = merged.len / 3 / 2
         if target_grp_tris < 2 {
            gi += 1
            continue
         }
         def simp = meshopt_simplify(merged, vbuf, pos_stride, target_grp_tris)
         def ratio = float(len(simp.indices)) / float(merged.len)
         if ratio < 0.92 {
            mut k = 0
            while k < len(simp.indices) {
               new_idx = vec_push(new_idx, simp.indices.get(k))
               k += 1
            }
            stuck = false
         } else {
            mut k = 0
            while k < merged.len {
               new_idx = vec_push(new_idx, merged.get(k))
               k += 1
            }
         }
         gi += 1
      }
      if stuck || new_idx.len < 9 { break }
      def new_tris = new_idx.len / 3
      if new_tris >= int(float(cur_tris) * 0.95) { break }
      def opt_n = meshopt_optimize_vertex_cache(new_idx, vcnt)
      def simp_err = prev_err + 1.0
      def lod_n = meshopt_build_meshlets(opt_n, vcnt, vbuf, pos_stride, 128, 128, lod_level, prev_err)
      def prev_ms2 = prev_lod.result.meshlets
      mut mi = 0
      while mi < prev_ms2.len {
         def m = prev_ms2.get(mi)
         def updated_m = {
            "vertex_offset":   m.vertex_offset,
            "triangle_offset": m.triangle_offset,
            "vertex_count":    m.vertex_count,
            "triangle_count":  m.triangle_count,
            "bounds":          m.bounds,
            "cluster_error":   m.cluster_error,
            "parent_error":    simp_err,
            "lod_level":       m.lod_level
         }
         prev_ms2[mi] = updated_m
         mi += 1
      }
      lods = vec_push(lods, { "level": lod_level, "result": lod_n, "error": simp_err })
      cur = new_idx
      cur_tris  = new_tris
      prev_err  = simp_err
      lod_level += 1
   }
   return { "lods": lods }
}

fn meshopt_process_mesh(list indices, int vcnt, any vbuf, int pos_stride, int max_levels) dict {
   "Runs the meshopt process mesh operation."
   if _meshopt_trace_enabled() { print("[meshopt] Processing mesh: indices=" + to_str(indices.len) + " verts=" + to_str(vcnt)) }
   def opt = meshopt_optimize_vertex_cache(indices, vcnt)
   if _meshopt_trace_enabled() { print("[meshopt] Cache optimization complete.") }
   def res = meshopt_build_lod_hierarchy(opt, vcnt, vbuf, pos_stride, max_levels)
   if _meshopt_trace_enabled() { print("[meshopt] Hierarchy build complete.") }
   return res
}

fn meshopt_cluster_screen_error(any bounds_cx, any bounds_cy, any bounds_cz, any bounds_r, any cluster_error, any cam_x, any cam_y, any cam_z, any cam_proj, any cam_znear) any {
   "Runs the meshopt cluster screen error operation."
   def dx, dy = bounds_cx - cam_x, bounds_cy - cam_y
   def dz = bounds_cz - cam_z
   def dist = _len3(dx, dy, dz) - bounds_r
   def safe_dist = dist > cam_znear ? dist : cam_znear
   cluster_error / safe_dist * (cam_proj * 0.5)
}

fn meshopt_select_lod_cut(dict lod_hierarchy, any cam_x, any cam_y, any cam_z, any cam_proj, any cam_znear, any pixel_error_threshold, any screen_h) list {
   "Runs the meshopt select lod cut operation."
   def threshold = pixel_error_threshold / screen_h
   def lods = lod_hierarchy.lods
   mut render_list = []
   mut li = 0
   while li < lods.len {
      def lod_rec = lods.get(li)
      def ms = lod_rec.result.meshlets
      mut mi = 0
      while mi < ms.len {
         def m = ms.get(mi)
         def bnd = m.bounds
         def own_err = meshopt_cluster_screen_error(
            bnd.center_x, bnd.center_y, bnd.center_z, bnd.radius, m.cluster_error,
         cam_x, cam_y, cam_z, cam_proj, cam_znear)
         def par_err = m.parent_error > 1e29 ? 1e30 :
         meshopt_cluster_screen_error(
            bnd.center_x, bnd.center_y, bnd.center_z, bnd.radius, m.parent_error,
         cam_x, cam_y, cam_z, cam_proj, cam_znear)
         if own_err <= threshold && par_err > threshold { render_list = vec_push(render_list, m) }
         mi += 1
      }
      li += 1
   }
   if render_list.len == 0 && lods.len > 0 {
      def lod0 = lods.get(0)
      render_list = lod0.result.meshlets
   }
   return render_list
}
