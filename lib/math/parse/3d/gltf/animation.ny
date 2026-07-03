;; Keywords: 3d gltf glb parse
;; Submodule: animation
module std.math.parse.3d.gltf.animation(_gltf_read_mat4_accessor_value, _gltf_mat4_transpose, _gltf_pack_skin_sidecars, gltf_skin_joint_mats, _gltf_skin_inv_bind_mats, _gltf_mesh_inv_key, _gltf_mesh_inv_cached, _gltf_pack_skin_mat_slab, _gltf_write_skin_mat_slab, _gltf_skin_mats_cache_record, gltf_free_skin_mats_cache, _gltf_apply_skinning_slab, _gltf_skin_weighted_mat4_vec3, _gltf_apply_skinning_fallback, gltf_apply_skinning, gltf_skin_count, gltf_skin_info, gltf_morph_target_count, _gltf_mesh_morph_weights, _gltf_collect_morph_targets, _gltf_release_morph_targets, _gltf_read_acc_f32, _gltf_read_acc_components, _gltf_read_acc_scalar_tuple, _gltf_read_anim_tuple, _gltf_lerp_vec, _gltf_nlerp_quat, _gltf_normalize_quat, _gltf_read_norm_i16_quat, _gltf_find_time_bracket, _gltf_sample_channel, gltf_animation_count, _gltf_animation_duration_from_anim, _gltf_animation_duration_from_samples, gltf_animation_info, _gltf_anim_fast_records, _gltf_anim_record_bracket, _gltf_anim_clean_tiny, _gltf_anim_fast_value, _gltf_sample_animation_fast, _gltf_anim_sample_component_count, _gltf_anim_store_override, gltf_sample_animation, gltf_sample_animation_merged, gltf_apply_morph_weights, _gltf_build_node_world_mats_animated_fast, gltf_rebuild_animated_mats)
use std.core
use std.math.bin
use std.math
use std.math.float (is_nan, is_inf)
use std.core.str as str
use std.core.common as common
use std.core.cache as cache
use std.math.parse.3d.gltf.math as gltf_math
use std.math.parse.3d.gltf.shared as shr
use std.math.parse.3d.gltf.load as ld
use std.math.parse.3d.gltf.material as mat

fn _gltf_read_mat4_accessor_value(any res, int idx) list {
   if !is_dict(res) { return gltf_math.mat4_identity() }
   def ptr = res.get("ptr", 0)
   if !ptr { return gltf_math.mat4_identity() }
   def stride = int(res.get("stride", 64))
   def off = idx * stride
   [
      load32_f32(ptr, off + 0), load32_f32(ptr, off + 4), load32_f32(ptr, off + 8), load32_f32(ptr, off + 12),
      load32_f32(ptr, off + 16), load32_f32(ptr, off + 20), load32_f32(ptr, off + 24), load32_f32(ptr, off + 28),
      load32_f32(ptr, off + 32), load32_f32(ptr, off + 36), load32_f32(ptr, off + 40), load32_f32(ptr, off + 44),
      load32_f32(ptr, off + 48), load32_f32(ptr, off + 52), load32_f32(ptr, off + 56), load32_f32(ptr, off + 60),
      "mat4", 400
   ]
}

fn _gltf_mat4_transpose(any m) list {
   if !is_list(m) || m.len < 16 { return gltf_math.mat4_identity() }
   [
      m.get(0, 1.0), m.get(4, 0.0), m.get(8, 0.0), m.get(12, 0.0),
      m.get(1, 0.0), m.get(5, 1.0), m.get(9, 0.0), m.get(13, 0.0),
      m.get(2, 0.0), m.get(6, 0.0), m.get(10, 1.0), m.get(14, 0.0),
      m.get(3, 0.0), m.get(7, 0.0), m.get(11, 0.0), m.get(15, 1.0),
      "mat4", 400
   ]
}

fn _gltf_pack_skin_sidecars(dict g, any data, int joints_acc_idx, int weights_acc_idx, int count) any {
   if count <= 0 { return 0 }
   def joints_res = ld._gltf_resolve_accessor_data(g, joints_acc_idx, data)
   def weights_res = ld._gltf_resolve_accessor_data(g, weights_acc_idx, data)
   if !is_dict(joints_res) || !is_dict(weights_res) {
      ld._gltf_release_accessor_data(joints_res)
      ld._gltf_release_accessor_data(weights_res)
      return 0
   }
   def joints_cnt = int(joints_res.get("count", 0))
   def weights_cnt = int(weights_res.get("count", 0))
   def source_limit = min(joints_cnt, weights_cnt)
   def joints_ptr = joints_res.get("ptr", 0)
   def weights_ptr = weights_res.get("ptr", 0)
   def joints_comp = int(joints_res.get("comp", 0))
   def weights_comp = int(weights_res.get("comp", 0))
   def joints_stride = int(joints_res.get("stride", 0))
   def weights_stride = int(weights_res.get("stride", 0))
   def weights_norm = weights_res.get("normalized", false)
   def use_count = min(count, source_limit)
   if !joints_ptr || !weights_ptr || use_count <= 0 || source_limit <= 0 {
      ld._gltf_release_accessor_data(joints_res)
      ld._gltf_release_accessor_data(weights_res)
      return 0
   }
   def joints_sidecar = malloc(use_count * 16)
   def weights_sidecar = malloc(use_count * 16)
   if !joints_sidecar || !weights_sidecar {
      if joints_sidecar { free(joints_sidecar) }
      if weights_sidecar { free(weights_sidecar) }
      ld._gltf_release_accessor_data(joints_res)
      ld._gltf_release_accessor_data(weights_res)
      return 0
   }
   memset(joints_sidecar, 0, use_count * 16)
   memset(weights_sidecar, 0, use_count * 16)
   mut single_influence = true
   mut vi = 0
   while vi < use_count {
      def joff, woff = vi * joints_stride, vi * weights_stride
      mut sj0 = 0
      mut sw0 = 0.0
      mut sw1 = 0.0
      mut sw2 = 0.0
      mut sw3 = 0.0
      mut k = 0
      while k < 4 {
         mut jv = 0
         if joints_comp == GLTF_COMP_UBYTE { jv = load8(joints_ptr, joff + k) }
         elif joints_comp == GLTF_COMP_USHORT { jv = u16le(joints_ptr, joff + k * 2) }
         elif joints_comp == GLTF_COMP_UINT { jv = u32le(joints_ptr, joff + k * 4) }
         elif joints_comp == GLTF_COMP_BYTE {
            def raw = int(load8(joints_ptr, joff + k))
            jv = raw >= 128 ? raw - 256 : raw
         } else {
            jv = 0
         }
         store32(joints_sidecar, int(jv), vi * 16 + k * 4)
         if k == 0 { sj0 = int(jv) }
         def wv = shr._gltf_read_f32_acc(
            weights_ptr,
            woff + k * max(1, shr._gltf_comp_size(weights_comp)),
            weights_comp,
            weights_norm
         )
         if k == 0 { sw0 = wv }
         elif k == 1 { sw1 = wv }
         elif k == 2 { sw2 = wv }
         else { sw3 = wv }
         store32_f32(weights_sidecar, wv, vi * 16 + k * 4)
         k += 1
      }
      if single_influence && !(sj0 >= 0 && sw0 >= 0.999 && abs(sw1) <= 0.0001 && abs(sw2) <= 0.0001 && abs(sw3) <= 0.0001) { single_influence = false }
      vi += 1
   }
   ld._gltf_release_accessor_data(joints_res)
   ld._gltf_release_accessor_data(weights_res)
   return {"joints_ptr": joints_sidecar, "weights_ptr": weights_sidecar, "count": use_count, "single_influence": single_influence}
}

fn gltf_skin_joint_mats(any gltf_data, int skin_idx, dict node_world_mats, any mesh_node_world=0) list {
   "Builds mesh-local skin joint matrices for the given skin."
   def skin = gltf_skin_info(gltf_data, skin_idx)
   if !is_dict(skin) { return [] }
   def joints = skin.get("joints", [])
   if !is_list(joints) || joints.len == 0 { return [] }
   def joints_n = joints.len
   def inv_bind_mats = _gltf_skin_inv_bind_mats(gltf_data, skin_idx)
   def skin_no_mesh_inv = shr._gltf_skin_no_mesh_inv_enabled()
   def skin_transpose_inv_bind = shr._gltf_skin_transpose_inv_bind_enabled()
   def skin_invbind_first = shr._gltf_skin_invbind_first_enabled()
   def mesh_inv = skin_no_mesh_inv ? gltf_math.mat4_identity() : _gltf_mesh_inv_cached(mesh_node_world)
   def world_list = is_dict(node_world_mats) ? node_world_mats.get("__world_list", 0) : 0
   mut out = list(joints_n)
   __list_set_len(out, joints_n)
   mut ji = 0
   while ji < joints_n {
      def joint_idx = int(joints.get(ji, -1))
      def joint_world = (is_list(world_list) && joint_idx >= 0 && joint_idx < world_list.len && is_list(world_list.get(joint_idx, 0))) ? world_list.get(joint_idx, gltf_math.mat4_identity()) : node_world_mats.get(joint_idx, gltf_math.mat4_identity())
      mut inv_bind = inv_bind_mats.get(ji, gltf_math.mat4_identity())
      if skin_transpose_inv_bind { inv_bind = _gltf_mat4_transpose(inv_bind) }
      mut jm = skin_invbind_first ? gltf_math.mat4_mul(inv_bind, joint_world) : gltf_math.mat4_mul(joint_world, inv_bind)
      jm = gltf_math.mat4_mul(mesh_inv, jm)
      out[ji] = jm
      ji += 1
   }
   out
}

fn _gltf_skin_inv_bind_mats(any gltf_data, int skin_idx) list {
   shr._gltf_ensure_caches()
   def skin = gltf_skin_info(gltf_data, skin_idx)
   if !is_dict(skin) { return [] }
   def joints = skin.get("joints", [])
   def joints_n = is_list(joints) ? joints.len : 0
   if joints_n <= 0 { return [] }
   def g = gltf_data.get("gltf", 0)
   def data = ld._gltf_primary_data_ptr(gltf_data)
   def inv_bind_acc = int(skin.get("inverse_bind_accessor", -1))
   def source_key = to_str(gltf_data.get("source_path", "")) + "|" + to_str(gltf_data.get("base_path", ""))
   def key = source_key + ":" + to_str(to_int(data)) + ":" + to_str(int(skin_idx)) + ":" + to_str(inv_bind_acc) + ":" + to_str(joints_n)
   def cached = _gltf_skin_inv_bind_cache.get(key, 0)
   if is_list(cached) && cached.len == joints_n { return cached }
   def inv_bind_res = ld._gltf_resolve_accessor_data(g, inv_bind_acc, data)
   mut mats = list(joints_n)
   __list_set_len(mats, joints_n)
   mut ji = 0
   while ji < joints_n {
      if is_dict(inv_bind_res) && ji < int(inv_bind_res.get("count", 0)) { mats[ji] = _gltf_read_mat4_accessor_value(inv_bind_res, ji) } else { mats[ji] = gltf_math.mat4_identity() }
      ji += 1
   }
   ld._gltf_release_accessor_data(inv_bind_res)
   _gltf_skin_inv_bind_cache = cache.cache_put_reset(shr._gltf_skin_inv_bind_cache, key, mats, shr._GLTF_CACHE_LIMIT_SMALL, 32)
   mats
}

fn _gltf_mesh_inv_key(any mesh_node_world) str {
   mut key = "mesh_inv"
   mut i = 0
   while i < 16 {
      key = key + ":" + to_str(float(mesh_node_world.get(i, (i == 0 || i == 5 || i == 10 || i == 15) ? 1.0 : 0.0)))
      i += 1
   }
   key
}

fn _gltf_mesh_inv_cached(any mesh_node_world) list {
   shr._gltf_ensure_caches()
   if !is_list(mesh_node_world) || mesh_node_world.len < 16 { return gltf_math.mat4_identity() }
   def key = _gltf_mesh_inv_key(mesh_node_world)
   def cached = _gltf_mesh_inv_cache.get(key, 0)
   if is_list(cached) { return cached }
   def inv = gltf_math.mat4_inverse_affine(mesh_node_world)
   _gltf_mesh_inv_cache = cache.cache_put_reset(shr._gltf_mesh_inv_cache, key, inv, shr._GLTF_CACHE_LIMIT_SMALL, 32)
   inv
}

fn _gltf_pack_skin_mat_slab(any skin_mats) any {
   if !is_list(skin_mats) { return 0 }
   def count = skin_mats.len
   if count <= 0 { return 0 }
   def slab = malloc(count * 64)
   if !slab { return 0 }
   if !_gltf_write_skin_mat_slab(skin_mats, slab) {
      free(slab)
      return 0
   }
   slab
}

fn _gltf_write_skin_mat_slab(any skin_mats, any slab) bool {
   if !is_list(skin_mats) || !slab { return false }
   def count = skin_mats.len
   mut i = 0
   while i < count {
      def m = skin_mats[i]
      def base = slab + i * 64
      if is_list(m) {
         mut j = 0
         while j < 16 {
            store32_f32(base, float(m.get(j, (j == 0 || j == 5 || j == 10 || j == 15) ? 1.0 : 0.0)), j * 4)
            j += 1
         }
      } else {
         memset(base, 0, 64)
         store32_f32(base, 1.0, 0)
         store32_f32(base, 1.0, 20)
         store32_f32(base, 1.0, 40)
         store32_f32(base, 1.0, 60)
      }
      i += 1
   }
   true
}

fn _gltf_skin_mats_cache_record(any gltf_data, int skin_idx, dict node_world_mats, any mesh_bind_world, any skin_mats_cache) any {
   def cache_key = to_str(skin_idx) + ":" + to_str(to_int(mesh_bind_world))
   if is_dict(skin_mats_cache) {
      def cached = skin_mats_cache.get(cache_key, 0)
      if is_list(cached) { return cached }
   }
   def skin_mats = gltf_skin_joint_mats(gltf_data, skin_idx, node_world_mats, mesh_bind_world)
   if !is_list(skin_mats) || skin_mats.len == 0 { return 0 }
   def slab = _gltf_pack_skin_mat_slab(skin_mats)
   def rec = [skin_mats, slab, skin_mats.len]
   if is_dict(skin_mats_cache) {
      skin_mats_cache[cache_key] = rec
      mut keys = skin_mats_cache.get("__keys", [])
      keys = keys.append(cache_key)
      skin_mats_cache["__keys"] = keys
   }
   rec
}

fn gltf_free_skin_mats_cache(any skin_mats_cache) bool {
   "Runs the free skin mats cache operation."
   if !is_dict(skin_mats_cache) { return false }
   def keys = skin_mats_cache.get("__keys", [])
   if !is_list(keys) { return false }
   def keys_n = keys.len
   mut i = 0
   while i < keys_n {
      def key = keys[i]
      def rec = skin_mats_cache.get(key, 0)
      if is_list(rec) {
         def slab = rec.get(1, 0)
         if slab { free(slab) }
      }
      i += 1
   }
   skin_mats_cache["__keys"] = []
   true
}

fn _gltf_apply_skinning_slab(any vptr, any bind_vptr, any joints_ptr, any weights_ptr, int vcnt, any skin_slab, int mat_count, bool copy_static_attrs=true) bool {
   if !vptr || !bind_vptr || !joints_ptr || !weights_ptr || !skin_slab || vcnt <= 0 || mat_count <= 0 { return false }
   ;; The whole vertex record only needs to be copied once: UVs, color,
   ;; material ids, tangents, and other static attributes do not change during
   ;; skinning.  Per-frame copies of large meshes such as BrainStem were
   ;; burning bandwidth before the actual joint math even started.
   if copy_static_attrs { memcpy(vptr, bind_vptr, vcnt * shr._GLTF_VTX_STRIDE) }
   mut vi = 0
   while vi < vcnt {
      def boff = vi * shr._GLTF_VTX_STRIDE
      def px = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_X)
      def py = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_Y)
      def pz = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_Z)
      def nx0 = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_NX)
      def ny0 = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_NY)
      def nz0 = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_NZ)
      def has_norm = (nx0 * nx0 + ny0 * ny0 + nz0 * nz0) > 0.000001
      def side_off = vi * 16
      def j0 = int(load32(joints_ptr, side_off + 0))
      def j1 = int(load32(joints_ptr, side_off + 4))
      def j2 = int(load32(joints_ptr, side_off + 8))
      def j3 = int(load32(joints_ptr, side_off + 12))
      def w0 = load32_f32(weights_ptr, side_off + 0)
      def w1 = load32_f32(weights_ptr, side_off + 4)
      def w2 = load32_f32(weights_ptr, side_off + 8)
      def w3 = load32_f32(weights_ptr, side_off + 12)
      def use0 = w0 > 0.000001 && j0 >= 0 && j0 < mat_count
      def use1 = w1 > 0.000001 && j1 >= 0 && j1 < mat_count
      def use2 = w2 > 0.000001 && j2 >= 0 && j2 < mat_count
      def use3 = w3 > 0.000001 && j3 >= 0 && j3 < mat_count
      def ew0 = use0 ? w0 : 0.0
      def ew1 = use1 ? w1 : 0.0
      def ew2 = use2 ? w2 : 0.0
      def ew3 = use3 ? w3 : 0.0
      def wsum = ew0 + ew1 + ew2 + ew3
      if wsum > 0.000001 {
         def inv_w = 1.0 / wsum
         mut sx = 0.0
         mut sy = 0.0
         mut sz = 0.0
         mut snx = 0.0
         mut sny = 0.0
         mut snz = 0.0
         if use0 {
            def mb = j0 * 64
            sx += ew0 * (load32_f32(skin_slab, mb + 0) * px + load32_f32(skin_slab, mb + 16) * py + load32_f32(skin_slab, mb + 32) * pz + load32_f32(skin_slab, mb + 48))
            sy += ew0 * (load32_f32(skin_slab, mb + 4) * px + load32_f32(skin_slab, mb + 20) * py + load32_f32(skin_slab, mb + 36) * pz + load32_f32(skin_slab, mb + 52))
            sz += ew0 * (load32_f32(skin_slab, mb + 8) * px + load32_f32(skin_slab, mb + 24) * py + load32_f32(skin_slab, mb + 40) * pz + load32_f32(skin_slab, mb + 56))
            if has_norm {
               snx += ew0 * (load32_f32(skin_slab, mb + 0) * nx0 + load32_f32(skin_slab, mb + 16) * ny0 + load32_f32(skin_slab, mb + 32) * nz0)
               sny += ew0 * (load32_f32(skin_slab, mb + 4) * nx0 + load32_f32(skin_slab, mb + 20) * ny0 + load32_f32(skin_slab, mb + 36) * nz0)
               snz += ew0 * (load32_f32(skin_slab, mb + 8) * nx0 + load32_f32(skin_slab, mb + 24) * ny0 + load32_f32(skin_slab, mb + 40) * nz0)
            }
         }
         if use1 {
            def mb = j1 * 64
            sx += ew1 * (load32_f32(skin_slab, mb + 0) * px + load32_f32(skin_slab, mb + 16) * py + load32_f32(skin_slab, mb + 32) * pz + load32_f32(skin_slab, mb + 48))
            sy += ew1 * (load32_f32(skin_slab, mb + 4) * px + load32_f32(skin_slab, mb + 20) * py + load32_f32(skin_slab, mb + 36) * pz + load32_f32(skin_slab, mb + 52))
            sz += ew1 * (load32_f32(skin_slab, mb + 8) * px + load32_f32(skin_slab, mb + 24) * py + load32_f32(skin_slab, mb + 40) * pz + load32_f32(skin_slab, mb + 56))
            if has_norm {
               snx += ew1 * (load32_f32(skin_slab, mb + 0) * nx0 + load32_f32(skin_slab, mb + 16) * ny0 + load32_f32(skin_slab, mb + 32) * nz0)
               sny += ew1 * (load32_f32(skin_slab, mb + 4) * nx0 + load32_f32(skin_slab, mb + 20) * ny0 + load32_f32(skin_slab, mb + 36) * nz0)
               snz += ew1 * (load32_f32(skin_slab, mb + 8) * nx0 + load32_f32(skin_slab, mb + 24) * ny0 + load32_f32(skin_slab, mb + 40) * nz0)
            }
         }
         if use2 {
            def mb = j2 * 64
            sx += ew2 * (load32_f32(skin_slab, mb + 0) * px + load32_f32(skin_slab, mb + 16) * py + load32_f32(skin_slab, mb + 32) * pz + load32_f32(skin_slab, mb + 48))
            sy += ew2 * (load32_f32(skin_slab, mb + 4) * px + load32_f32(skin_slab, mb + 20) * py + load32_f32(skin_slab, mb + 36) * pz + load32_f32(skin_slab, mb + 52))
            sz += ew2 * (load32_f32(skin_slab, mb + 8) * px + load32_f32(skin_slab, mb + 24) * py + load32_f32(skin_slab, mb + 40) * pz + load32_f32(skin_slab, mb + 56))
            if has_norm {
               snx += ew2 * (load32_f32(skin_slab, mb + 0) * nx0 + load32_f32(skin_slab, mb + 16) * ny0 + load32_f32(skin_slab, mb + 32) * nz0)
               sny += ew2 * (load32_f32(skin_slab, mb + 4) * nx0 + load32_f32(skin_slab, mb + 20) * ny0 + load32_f32(skin_slab, mb + 36) * nz0)
               snz += ew2 * (load32_f32(skin_slab, mb + 8) * nx0 + load32_f32(skin_slab, mb + 24) * ny0 + load32_f32(skin_slab, mb + 40) * nz0)
            }
         }
         if use3 {
            def mb = j3 * 64
            sx += ew3 * (load32_f32(skin_slab, mb + 0) * px + load32_f32(skin_slab, mb + 16) * py + load32_f32(skin_slab, mb + 32) * pz + load32_f32(skin_slab, mb + 48))
            sy += ew3 * (load32_f32(skin_slab, mb + 4) * px + load32_f32(skin_slab, mb + 20) * py + load32_f32(skin_slab, mb + 36) * pz + load32_f32(skin_slab, mb + 52))
            sz += ew3 * (load32_f32(skin_slab, mb + 8) * px + load32_f32(skin_slab, mb + 24) * py + load32_f32(skin_slab, mb + 40) * pz + load32_f32(skin_slab, mb + 56))
            if has_norm {
               snx += ew3 * (load32_f32(skin_slab, mb + 0) * nx0 + load32_f32(skin_slab, mb + 16) * ny0 + load32_f32(skin_slab, mb + 32) * nz0)
               sny += ew3 * (load32_f32(skin_slab, mb + 4) * nx0 + load32_f32(skin_slab, mb + 20) * ny0 + load32_f32(skin_slab, mb + 36) * nz0)
               snz += ew3 * (load32_f32(skin_slab, mb + 8) * nx0 + load32_f32(skin_slab, mb + 24) * ny0 + load32_f32(skin_slab, mb + 40) * nz0)
            }
         }
         sx *= inv_w
         sy *= inv_w
         sz *= inv_w
         if shr._gltf_float_bad(sx) || shr._gltf_float_bad(sy) || shr._gltf_float_bad(sz) {
            store32_f32(vptr, px, boff + shr._GLTF_VTX_OFF_X)
            store32_f32(vptr, py, boff + shr._GLTF_VTX_OFF_Y)
            store32_f32(vptr, pz, boff + shr._GLTF_VTX_OFF_Z)
         } else {
            store32_f32(vptr, sx, boff + shr._GLTF_VTX_OFF_X)
            store32_f32(vptr, sy, boff + shr._GLTF_VTX_OFF_Y)
            store32_f32(vptr, sz, boff + shr._GLTF_VTX_OFF_Z)
         }
         if has_norm {
            snx *= inv_w
            sny *= inv_w
            snz *= inv_w
            def nl = sqrt(snx * snx + sny * sny + snz * snz)
            if nl > 0.000001 && !shr._gltf_float_bad(nl) {
               def inv_n = 1.0 / nl
               store32_f32(vptr, snx * inv_n, boff + shr._GLTF_VTX_OFF_NX)
               store32_f32(vptr, sny * inv_n, boff + shr._GLTF_VTX_OFF_NY)
               store32_f32(vptr, snz * inv_n, boff + shr._GLTF_VTX_OFF_NZ)
            } else {
               store32_f32(vptr, nx0, boff + shr._GLTF_VTX_OFF_NX)
               store32_f32(vptr, ny0, boff + shr._GLTF_VTX_OFF_NY)
               store32_f32(vptr, nz0, boff + shr._GLTF_VTX_OFF_NZ)
            }
         }
      } else {
         store32_f32(vptr, px, boff + shr._GLTF_VTX_OFF_X)
         store32_f32(vptr, py, boff + shr._GLTF_VTX_OFF_Y)
         store32_f32(vptr, pz, boff + shr._GLTF_VTX_OFF_Z)
         if has_norm {
            store32_f32(vptr, nx0, boff + shr._GLTF_VTX_OFF_NX)
            store32_f32(vptr, ny0, boff + shr._GLTF_VTX_OFF_NY)
            store32_f32(vptr, nz0, boff + shr._GLTF_VTX_OFF_NZ)
         }
      }
      vi += 1
   }
   true
}

fn _gltf_apply_skinning_slab_single(any vptr, any bind_vptr, any joints_ptr, any weights_ptr, int vcnt, any skin_slab, int mat_count, bool copy_static_attrs=true) bool {
   if !vptr || !bind_vptr || !joints_ptr || !weights_ptr || !skin_slab || vcnt <= 0 || mat_count <= 0 { return false }
   if copy_static_attrs { memcpy(vptr, bind_vptr, vcnt * shr._GLTF_VTX_STRIDE) }
   mut vi = 0
   while vi < vcnt {
      def boff = vi * shr._GLTF_VTX_STRIDE
      def side_off = vi * 16
      def j0 = int(load32(joints_ptr, side_off))
      def w0 = load32_f32(weights_ptr, side_off)
      def px = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_X)
      def py = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_Y)
      def pz = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_Z)
      if j0 >= 0 && j0 < mat_count && w0 > 0.000001 {
         def mb = j0 * 64
         def m00 = load32_f32(skin_slab, mb + 0)
         def m01 = load32_f32(skin_slab, mb + 4)
         def m02 = load32_f32(skin_slab, mb + 8)
         def m10 = load32_f32(skin_slab, mb + 16)
         def m11 = load32_f32(skin_slab, mb + 20)
         def m12 = load32_f32(skin_slab, mb + 24)
         def m20 = load32_f32(skin_slab, mb + 32)
         def m21 = load32_f32(skin_slab, mb + 36)
         def m22 = load32_f32(skin_slab, mb + 40)
         def m30 = load32_f32(skin_slab, mb + 48)
         def m31 = load32_f32(skin_slab, mb + 52)
         def m32 = load32_f32(skin_slab, mb + 56)
         def sx = m00 * px + m10 * py + m20 * pz + m30
         def sy = m01 * px + m11 * py + m21 * pz + m31
         def sz = m02 * px + m12 * py + m22 * pz + m32
         if shr._gltf_float_bad(sx) || shr._gltf_float_bad(sy) || shr._gltf_float_bad(sz) {
            store32_f32(vptr, px, boff + shr._GLTF_VTX_OFF_X)
            store32_f32(vptr, py, boff + shr._GLTF_VTX_OFF_Y)
            store32_f32(vptr, pz, boff + shr._GLTF_VTX_OFF_Z)
         } else {
            store32_f32(vptr, sx, boff + shr._GLTF_VTX_OFF_X)
            store32_f32(vptr, sy, boff + shr._GLTF_VTX_OFF_Y)
            store32_f32(vptr, sz, boff + shr._GLTF_VTX_OFF_Z)
         }
         def nx0 = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_NX)
         def ny0 = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_NY)
         def nz0 = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_NZ)
         if (nx0 * nx0 + ny0 * ny0 + nz0 * nz0) > 0.000001 {
            def snx = m00 * nx0 + m10 * ny0 + m20 * nz0
            def sny = m01 * nx0 + m11 * ny0 + m21 * nz0
            def snz = m02 * nx0 + m12 * ny0 + m22 * nz0
            def nl = sqrt(snx * snx + sny * sny + snz * snz)
            if nl > 0.000001 && !shr._gltf_float_bad(nl) {
               def inv_n = 1.0 / nl
               store32_f32(vptr, snx * inv_n, boff + shr._GLTF_VTX_OFF_NX)
               store32_f32(vptr, sny * inv_n, boff + shr._GLTF_VTX_OFF_NY)
               store32_f32(vptr, snz * inv_n, boff + shr._GLTF_VTX_OFF_NZ)
            }
         }
      } else {
         store32_f32(vptr, px, boff + shr._GLTF_VTX_OFF_X)
         store32_f32(vptr, py, boff + shr._GLTF_VTX_OFF_Y)
         store32_f32(vptr, pz, boff + shr._GLTF_VTX_OFF_Z)
      }
      vi += 1
   }
   true
}

fn _gltf_skin_weighted_mat4_vec3(
   any jm0, any jm1, any jm2, any jm3,
   f64 ew0, f64 ew1, f64 ew2, f64 ew3, f64 inv_w,
   f64 x, f64 y, f64 z, bool translate
) list {
   def x0 = (0.0 + jm0.get(0, 1.0)) * x + (0.0 + jm0.get(4, 0.0)) * y + (0.0 + jm0.get(8, 0.0)) * z + (translate ? (0.0 + jm0.get(12, 0.0)) : 0.0)
   def y0 = (0.0 + jm0.get(1, 0.0)) * x + (0.0 + jm0.get(5, 1.0)) * y + (0.0 + jm0.get(9, 0.0)) * z + (translate ? (0.0 + jm0.get(13, 0.0)) : 0.0)
   def z0 = (0.0 + jm0.get(2, 0.0)) * x + (0.0 + jm0.get(6, 0.0)) * y + (0.0 + jm0.get(10, 1.0)) * z + (translate ? (0.0 + jm0.get(14, 0.0)) : 0.0)
   def x1 = (0.0 + jm1.get(0, 1.0)) * x + (0.0 + jm1.get(4, 0.0)) * y + (0.0 + jm1.get(8, 0.0)) * z + (translate ? (0.0 + jm1.get(12, 0.0)) : 0.0)
   def y1 = (0.0 + jm1.get(1, 0.0)) * x + (0.0 + jm1.get(5, 1.0)) * y + (0.0 + jm1.get(9, 0.0)) * z + (translate ? (0.0 + jm1.get(13, 0.0)) : 0.0)
   def z1 = (0.0 + jm1.get(2, 0.0)) * x + (0.0 + jm1.get(6, 0.0)) * y + (0.0 + jm1.get(10, 1.0)) * z + (translate ? (0.0 + jm1.get(14, 0.0)) : 0.0)
   def x2 = (0.0 + jm2.get(0, 1.0)) * x + (0.0 + jm2.get(4, 0.0)) * y + (0.0 + jm2.get(8, 0.0)) * z + (translate ? (0.0 + jm2.get(12, 0.0)) : 0.0)
   def y2 = (0.0 + jm2.get(1, 0.0)) * x + (0.0 + jm2.get(5, 1.0)) * y + (0.0 + jm2.get(9, 0.0)) * z + (translate ? (0.0 + jm2.get(13, 0.0)) : 0.0)
   def z2 = (0.0 + jm2.get(2, 0.0)) * x + (0.0 + jm2.get(6, 0.0)) * y + (0.0 + jm2.get(10, 1.0)) * z + (translate ? (0.0 + jm2.get(14, 0.0)) : 0.0)
   def x3 = (0.0 + jm3.get(0, 1.0)) * x + (0.0 + jm3.get(4, 0.0)) * y + (0.0 + jm3.get(8, 0.0)) * z + (translate ? (0.0 + jm3.get(12, 0.0)) : 0.0)
   def y3 = (0.0 + jm3.get(1, 0.0)) * x + (0.0 + jm3.get(5, 1.0)) * y + (0.0 + jm3.get(9, 0.0)) * z + (translate ? (0.0 + jm3.get(13, 0.0)) : 0.0)
   def z3 = (0.0 + jm3.get(2, 0.0)) * x + (0.0 + jm3.get(6, 0.0)) * y + (0.0 + jm3.get(10, 1.0)) * z + (translate ? (0.0 + jm3.get(14, 0.0)) : 0.0)
   [
      (x0 * ew0 + x1 * ew1 + x2 * ew2 + x3 * ew3) * inv_w,
      (y0 * ew0 + y1 * ew1 + y2 * ew2 + y3 * ew3) * inv_w,
      (z0 * ew0 + z1 * ew1 + z2 * ew2 + z3 * ew3) * inv_w
   ]
}

fn _gltf_apply_skinning_fallback(
   dict part, list skin_mats,
   any bind_vptr, any joints_ptr, any weights_ptr, any vptr, int vcnt
) any {
   memcpy(vptr, bind_vptr, vcnt * shr._GLTF_VTX_STRIDE)
   mut vi = 0
   while vi < vcnt {
      def boff = vi * _GLTF_VTX_STRIDE
      def px = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_X)
      def py = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_Y)
      def pz = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_Z)
      def nx0 = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_NX)
      def ny0 = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_NY)
      def nz0 = load32_f32(bind_vptr, boff + shr._GLTF_VTX_OFF_NZ)
      def has_norm = (nx0 * nx0 + ny0 * ny0 + nz0 * nz0) > 0.000001
      def side_off = vi * 16
      def j0 = int(load32(joints_ptr, side_off + 0))
      def j1 = int(load32(joints_ptr, side_off + 4))
      def j2 = int(load32(joints_ptr, side_off + 8))
      def j3 = int(load32(joints_ptr, side_off + 12))
      def w0 = load32_f32(weights_ptr, side_off + 0)
      def w1 = load32_f32(weights_ptr, side_off + 4)
      def w2 = load32_f32(weights_ptr, side_off + 8)
      def w3 = load32_f32(weights_ptr, side_off + 12)
      def use0 = w0 > 0.000001 && j0 >= 0 && j0 < skin_mats.len
      def use1 = w1 > 0.000001 && j1 >= 0 && j1 < skin_mats.len
      def use2 = w2 > 0.000001 && j2 >= 0 && j2 < skin_mats.len
      def use3 = w3 > 0.000001 && j3 >= 0 && j3 < skin_mats.len
      def ew0 = use0 ? w0 : 0.0
      def ew1 = use1 ? w1 : 0.0
      def ew2 = use2 ? w2 : 0.0
      def ew3 = use3 ? w3 : 0.0
      def jm0 = use0 ? skin_mats.get(j0, gltf_math.mat4_identity()) : gltf_math.mat4_identity()
      def jm1 = use1 ? skin_mats.get(j1, gltf_math.mat4_identity()) : gltf_math.mat4_identity()
      def jm2 = use2 ? skin_mats.get(j2, gltf_math.mat4_identity()) : gltf_math.mat4_identity()
      def jm3 = use3 ? skin_mats.get(j3, gltf_math.mat4_identity()) : gltf_math.mat4_identity()
      def wsum = ew0 + ew1 + ew2 + ew3
      if wsum > 0.000001 {
         def inv_w = 1.0 / wsum
         def skin_pos = _gltf_skin_weighted_mat4_vec3(jm0, jm1, jm2, jm3, ew0, ew1, ew2, ew3, inv_w, px, py, pz, true)
         def sx, sy = float(skin_pos.get(0, px)), float(skin_pos.get(1, py))
         def sz = float(skin_pos.get(2, pz))
         if shr._gltf_float_bad(sx) || shr._gltf_float_bad(sy) || shr._gltf_float_bad(sz) {
            store32_f32(vptr, px, boff + shr._GLTF_VTX_OFF_X)
            store32_f32(vptr, py, boff + shr._GLTF_VTX_OFF_Y)
            store32_f32(vptr, pz, boff + shr._GLTF_VTX_OFF_Z)
         } else {
            store32_f32(vptr, sx, boff + shr._GLTF_VTX_OFF_X)
            store32_f32(vptr, sy, boff + shr._GLTF_VTX_OFF_Y)
            store32_f32(vptr, sz, boff + shr._GLTF_VTX_OFF_Z)
         }
         if has_norm {
            def skin_norm = _gltf_skin_weighted_mat4_vec3(jm0, jm1, jm2, jm3, ew0, ew1, ew2, ew3, inv_w, nx0, ny0, nz0, false)
            def nnx, nny = float(skin_norm.get(0, nx0)), float(skin_norm.get(1, ny0))
            def nnz = float(skin_norm.get(2, nz0))
            def nl = sqrt(nnx * nnx + nny * nny + nnz * nnz)
            if nl > 0.000001 && !shr._gltf_float_bad(nl) {
               def inv_n = 1.0 / nl
               store32_f32(vptr, nnx * inv_n, boff + shr._GLTF_VTX_OFF_NX)
               store32_f32(vptr, nny * inv_n, boff + shr._GLTF_VTX_OFF_NY)
               store32_f32(vptr, nnz * inv_n, boff + shr._GLTF_VTX_OFF_NZ)
            } else {
               store32_f32(vptr, nx0, boff + shr._GLTF_VTX_OFF_NX)
               store32_f32(vptr, ny0, boff + shr._GLTF_VTX_OFF_NY)
               store32_f32(vptr, nz0, boff + shr._GLTF_VTX_OFF_NZ)
            }
         }
      } else {
         store32_f32(vptr, px, boff + shr._GLTF_VTX_OFF_X)
         store32_f32(vptr, py, boff + shr._GLTF_VTX_OFF_Y)
         store32_f32(vptr, pz, boff + shr._GLTF_VTX_OFF_Z)
         if has_norm {
            store32_f32(vptr, nx0, boff + shr._GLTF_VTX_OFF_NX)
            store32_f32(vptr, ny0, boff + shr._GLTF_VTX_OFF_NY)
            store32_f32(vptr, nz0, boff + shr._GLTF_VTX_OFF_NZ)
         }
      }
      vi += 1
   }
   part
}

fn gltf_apply_skinning(any part, any gltf_data, any node_world_mats, any skin_mats_cache=0) any {
   "Applies CPU skinning in-place to a loaded part if it carries skin sidecars."
   if !is_dict(part) || !is_dict(gltf_data) || !is_dict(node_world_mats) { return part }
   def skin_idx = int(part.get("skin_idx", -1))
   if skin_idx < 0 { return part }
   if shr._gltf_disable_skinning_enabled() { return part }
   def bind_vptr = part.get("skin_bind_vptr", 0)
   def joints_ptr = part.get("skin_joints_ptr", 0)
   def weights_ptr = part.get("skin_weights_ptr", 0)
   def vptr = part.get("vptr", 0)
   if !bind_vptr || !joints_ptr || !weights_ptr || !vptr { return part }
   def vcnt = int(part.get("skin_vcnt", part.get("vcnt", 0)))
   if vcnt <= 0 { return part }
   def node_idx = int(part.get("node_idx", -1))
   mut mesh_bind_world = node_idx >= 0 ? node_world_mats.get(node_idx, part.get("model", 0)) : part.get("model", 0)
   if !is_list(mesh_bind_world)|| mesh_bind_world.len < 16 { mesh_bind_world = part.get("skin_mesh_bind_world", part.get("model", 0)) }
   if !is_dict(skin_mats_cache) { skin_mats_cache = dict(4) }
   def skin_rec = _gltf_skin_mats_cache_record(gltf_data, skin_idx, node_world_mats, mesh_bind_world, skin_mats_cache)
   mut skin_mats = is_list(skin_rec) ? skin_rec.get(0, 0) : 0
   def skin_slab = is_list(skin_rec) ? skin_rec.get(1, 0) : 0
   def skin_mat_count = is_list(skin_rec) ? int(skin_rec.get(2, 0)) : 0
   if skin_slab && skin_mat_count > 0 && !shr._gltf_skin_raw_off_enabled() {
      def copy_static_attrs = !bool(part.get("skin_static_attrs_ready", false)) || bool(part.get("skin_force_static_copy", false)) || int(part.get("morph_target_count", 0)) > 0
      if bool(part.get("skin_single_influence", false)) {
         _gltf_apply_skinning_slab_single(vptr, bind_vptr, joints_ptr, weights_ptr, vcnt, skin_slab, skin_mat_count, copy_static_attrs)
      } else {
         _gltf_apply_skinning_slab(vptr, bind_vptr, joints_ptr, weights_ptr, vcnt, skin_slab, skin_mat_count, copy_static_attrs)
      }
      if copy_static_attrs && int(part.get("morph_target_count", 0)) <= 0 { part["skin_static_attrs_ready"] = true }
      return part
   }
   if !is_list(skin_mats) || skin_mats.len == 0 { return part }
   _gltf_apply_skinning_fallback(part, skin_mats, bind_vptr, joints_ptr, weights_ptr, vptr, vcnt)
}

fn gltf_skin_count(any gltf_data) int {
   "Returns number of skin objects in the glTF asset."
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return 0 }
   def skins = g.get("skins", 0)
   is_list(skins) ? skins.len : 0
}

fn gltf_skin_info(any gltf_data, int skin_idx) any {
   "Runs the skin info operation."
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return 0 }
   def skins = g.get("skins", [])
   if !is_list(skins) || skin_idx < 0 || skin_idx >= skins.len { return 0 }
   def skin = skins.get(skin_idx, 0)
   if !is_dict(skin) { return 0 }
   {
      "index": skin_idx,
      "name": to_str(skin.get("name", "")),
      "skeleton": int(skin.get("skeleton", -1)),
      "joints": skin.get("joints", []),
      "inverse_bind_accessor": int(skin.get("inverseBindMatrices", -1))
   }
}

fn gltf_morph_target_count(any gltf_data) int {
   "Returns the maximum morph target count found on any primitive."
   def meshes = shr._gltf_meshes(gltf_data)
   if !meshes { return 0 }
   def meshes_n = meshes.len
   mut best = 0
   mut mi = 0
   while mi < meshes_n {
      def mesh = meshes[mi]
      def prims = is_dict(mesh) ? mesh.get("primitives", 0) : 0
      if is_list(prims) {
         def prims_n = prims.len
         mut pi = 0
         while pi < prims_n {
            def prim = prims[pi]
            def targets = is_dict(prim) ? prim.get("targets", 0) : 0
            if is_list(targets) {
               def targets_n = targets.len
               if targets_n > best { best = targets_n }
            }
            pi += 1
         }
      }
      mi += 1
   }
   best
}

fn _gltf_mesh_morph_weights(any g, int mesh_idx, int target_count) list {
   mut out = list(max(0, target_count))
   if target_count > 0 { __list_set_len(out, target_count) }
   mut i = 0
   while i < target_count { out[i] = 0.0 i += 1 }
   if target_count <= 0 { return out }
   def meshes = g.get("meshes")
   if !is_list(meshes) || mesh_idx < 0 || mesh_idx >= meshes.len { return out }
   def mesh = meshes[mesh_idx]
   def weights = is_dict(mesh) ? mesh.get("weights", 0) : 0
   if !is_list(weights) { return out }
   def weights_n = weights.len
   i = 0
   while i < target_count && i < weights_n {
      out[i] = float(weights[i])
      i += 1
   }
   out
}

fn _gltf_collect_morph_targets(dict g, any data, any targets, any morph_weights) list {
   mut morph_targets = list(0)
   if !is_list(targets) { return morph_targets }
   def targets_n = targets.len
   mut ti = 0
   while ti < targets_n {
      def target = targets[ti]
      def weight = is_list(morph_weights) ? float(morph_weights.get(ti, 0.0)) : 0.0
      if is_dict(target) {
         ;; Keep target accessors even when the current weight is zero.  Animated
         ;; morphs often start at 0 then rise/fall later; dropping zero-weight
         ;; targets made MorphStressTest stick at the pulled pose or rebuild
         ;; through the non-morph texture path.
         def t_pos_res = ld._gltf_resolve_accessor_data(g, int(target.get("POSITION", -1)), data)
         def t_norm_res = ld._gltf_resolve_accessor_data(g, int(target.get("NORMAL", -1)), data)
         def t_tan_res = ld._gltf_resolve_accessor_data(g, int(target.get("TANGENT", -1)), data)
         if is_dict(t_pos_res) || is_dict(t_norm_res) || is_dict(t_tan_res) {
            morph_targets = morph_targets.append({"weight": weight, "pos_res": t_pos_res, "norm_res": t_norm_res, "tan_res": t_tan_res})
         } else {
            ld._gltf_release_accessor_data(t_pos_res)
            ld._gltf_release_accessor_data(t_norm_res)
            ld._gltf_release_accessor_data(t_tan_res)
         }
      }
      ti += 1
   }
   morph_targets
}

fn _gltf_release_morph_targets(list morph_targets) bool {
   def morph_targets_n = morph_targets.len
   mut mti = 0
   while mti < morph_targets_n {
      def mt = morph_targets.get(mti, 0)
      ld._gltf_release_accessor_data(mt.get("pos_res", 0))
      ld._gltf_release_accessor_data(mt.get("norm_res", 0))
      ld._gltf_release_accessor_data(mt.get("tan_res", 0))
      mti += 1
   }
   true
}

fn _gltf_read_acc_f32(any data, dict acc_res, int elem_idx, int comp_idx) f64 {
   def ptr = acc_res.get("ptr", 0)
   if !ptr { return 0.0 }
   def stride = int(acc_res.get("stride", 4))
   def comp = int(acc_res.get("comp", shr.GLTF_COMP_FLOAT))
   def norm = acc_res.get("normalized", false)
   def cs = shr._gltf_comp_size(comp)
   def cols = int(acc_res.get("cols", 1))
   def rows = int(acc_res.get("rows", int(acc_res.get("type_count", 1))))
   mut byte_off = 0
   if cols <= 1 { byte_off = elem_idx * stride + comp_idx * cs } else {
      def col = comp_idx / rows
      def row = comp_idx % rows
      def col_size = shr._gltf_align_up(rows * cs, 4)
      byte_off = elem_idx * stride + col * col_size + row * cs
   }
   shr._gltf_read_f32_acc(ptr, byte_off, comp, norm)
}

fn _gltf_read_acc_components(any data, any acc_res, int elem_idx, int n_comp) list {
   def ptr = acc_res.get("ptr", 0)
   if !ptr { mut out = list(max(0, n_comp)) if n_comp > 0 { __list_set_len(out, n_comp) } return out }
   def stride = int(acc_res.get("stride", 4))
   def comp = int(acc_res.get("comp", shr.GLTF_COMP_FLOAT))
   def norm = acc_res.get("normalized", false)
   def cs = shr._gltf_comp_size(comp)
   def cols = int(acc_res.get("cols", 1))
   def rows = int(acc_res.get("rows", int(acc_res.get("type_count", 1))))
   def col_size = cols <= 1 ? 0 : shr._gltf_align_up(rows * cs, 4)
   def base_off = elem_idx * stride
   mut out = list(max(0, n_comp))
   if n_comp > 0 { __list_set_len(out, n_comp) }
   mut i = 0
   while i < n_comp {
      if cols <= 1 {
         out[i] = shr._gltf_read_f32_acc(ptr, base_off + i * cs, comp, norm)
      } else {
         def col = i / rows
         def row = i % rows
         out[i] = shr._gltf_read_f32_acc(ptr, base_off + col * col_size + row * cs, comp, norm)
      }
      i += 1
   }
   out
}

fn _gltf_read_acc_scalar_tuple(any data, any acc_res, int tuple_idx, int n_comp) list {
   mut out = list(max(0, n_comp))
   if n_comp > 0 { __list_set_len(out, n_comp) }
   def ptr = acc_res.get("ptr", 0)
   if !ptr { return out }
   def stride = int(acc_res.get("stride", 4))
   def comp = int(acc_res.get("comp", shr.GLTF_COMP_FLOAT))
   def norm = acc_res.get("normalized", false)
   def cs = shr._gltf_comp_size(comp)
   def base_idx = tuple_idx * n_comp
   mut i = 0
   while i < n_comp {
      def elem_idx = base_idx + i
      def byte_off = elem_idx * stride
      out[i] = shr._gltf_read_f32_acc(ptr, byte_off, comp, norm)
      i += 1
   }
   out
}

fn _gltf_read_anim_tuple(any data, any output_res, int idx, int n_comp, bool packed_scalar_tuple) list {
   if packed_scalar_tuple { return _gltf_read_acc_scalar_tuple(data, output_res, idx, n_comp) }
   _gltf_read_acc_components(data, output_res, idx, n_comp)
}

fn _gltf_clamp01(f64 t) f64 {
   if is_nan(t) || is_inf(t) { return 0.0 }
   if t < 0.0 { return 0.0 }
   if t > 1.0 { return 1.0 }
   t
}

fn _gltf_anim_interp_mode(any raw) str {
   def mode = str.upper(str.strip(to_str(raw)))
   mode.len > 0 ? mode : "LINEAR"
}

fn _gltf_anim_alpha(f64 time_sec, f64 t_lo, f64 t_hi) f64 {
   ;; Keep this math explicitly floating-point.  A regression made the
   ;; bracket alpha collapse to 0/1 on some runtime paths, which made
   ;; LINEAR AnimatedCube look exactly like STEP/keyframe snapping.
   def lo_f = float(t_lo)
   def hi_f = float(t_hi)
   def dt = hi_f - lo_f
   if is_nan(dt) || is_inf(dt) || dt <= 0.00000001 { return 0.0 }
   _gltf_clamp01((float(time_sec) - lo_f) / dt)
}

fn _gltf_lerp_vec(list a, list b, f64 t, int n) list {
   def tc = _gltf_clamp01(t)
   mut out = list(max(0, n))
   if n > 0 { __list_set_len(out, n) }
   mut i = 0
   while i < n {
      def av, bv = 0.0 + a.get(i, 0.0), 0.0 + b.get(i, 0.0)
      out[i] = av + (bv - av) * tc
      i += 1
   }
   out
}

fn _gltf_nlerp_quat(list a, list b, f64 t) list {
   def tc = _gltf_clamp01(t)
   ;; glTF LINEAR rotation interpolation for quaternions should take the
   ;; shortest arc and keep the result normalized.  This is used by both the
   ;; generic and fast animation samplers.
   def ax, ay = 0.0 + a.get(0, 0.0), 0.0 + a.get(1, 0.0)
   def az, aw = 0.0 + a.get(2, 0.0), 0.0 + a.get(3, 1.0)
   mut bx, by = 0.0 + b.get(0, 0.0), 0.0 + b.get(1, 0.0)
   mut bz, bw = 0.0 + b.get(2, 0.0), 0.0 + b.get(3, 1.0)
   mut dot = ax * bx + ay * by + az * bz + aw * bw
   if dot < 0.0 {
      bx, by = -bx, -by
      bz, bw = -bz, -bw
      dot = 0.0 - dot
   }
   if dot > 0.9995 {
      def rx, ry = ax + (bx - ax) * tc, ay + (by - ay) * tc
      def rz, rw = az + (bz - az) * tc, aw + (bw - aw) * tc
      def len2 = rx * rx + ry * ry + rz * rz + rw * rw
      def inv_len = len2 > 0.000001 ? 1.0 / sqrt(len2) : 1.0
      return [rx * inv_len, ry * inv_len, rz * inv_len, rw * inv_len]
   }
   if dot > 1.0 { dot = 1.0 }
   def theta_0 = acos(dot)
   def sin_theta_0 = sin(theta_0)
   if abs(sin_theta_0) <= 0.000001 { return [ax, ay, az, aw] }
   def theta = theta_0 * tc
   def sin_theta = sin(theta)
   def s0 = cos(theta) - dot * sin_theta / sin_theta_0
   def s1 = sin_theta / sin_theta_0
   def rx, ry = ax * s0 + bx * s1, ay * s0 + by * s1
   def rz, rw = az * s0 + bz * s1, aw * s0 + bw * s1
   def len2 = rx * rx + ry * ry + rz * rz + rw * rw
   if len2 <= 0.000001 { return [0.0, 0.0, 0.0, 1.0] }
   def inv_len = 1.0 / sqrt(len2)
   [rx * inv_len, ry * inv_len, rz * inv_len, rw * inv_len]
}

fn _gltf_normalize_quat(any q) list {
   def x, y = 0.0 + q.get(0, 0.0), 0.0 + q.get(1, 0.0)
   def z, w = 0.0 + q.get(2, 0.0), 0.0 + q.get(3, 1.0)
   def len2 = x * x + y * y + z * z + w * w
   if len2 <= 0.000001 { return [0.0, 0.0, 0.0, 1.0] }
   def inv_len = 1.0 / sqrt(len2)
   [x * inv_len, y * inv_len, z * inv_len, w * inv_len]
}

fn _gltf_read_norm_i16_quat(any data_ptr, int off) list {
   mut x = u16le(data_ptr, off + 0)
   mut y = u16le(data_ptr, off + 2)
   mut z = u16le(data_ptr, off + 4)
   mut w = u16le(data_ptr, off + 6)
   if x >= 32768 { x = x - 65536 }
   if y >= 32768 { y = y - 65536 }
   if z >= 32768 { z = z - 65536 }
   if w >= 32768 { w = w - 65536 }
   mut qx, qy = x * shr._GLTF_INV_32767, y * shr._GLTF_INV_32767
   mut qz, qw = z * shr._GLTF_INV_32767, w * shr._GLTF_INV_32767
   if qx < -1.0 { qx = -1.0 }
   if qy < -1.0 { qy = -1.0 }
   if qz < -1.0 { qz = -1.0 }
   if qw < -1.0 { qw = -1.0 }
   [qx, qy, qz, qw]
}

fn _gltf_find_time_bracket(any data, any input_res, f64 time_sec) list {
   def ptr = input_res.get("ptr", 0)
   def count = int(input_res.get("count", 0))
   if !ptr || count <= 0 { return [0, 0, 0.0] }
   def stride = int(input_res.get("stride", 4))
   if count == 1 { return [0, 0, 0.0] }
   def t_first = f32le(ptr, 0)
   def t_last  = f32le(ptr, (count - 1) * stride)
   if time_sec <= t_first { return [0, 0, 0.0] }
   if time_sec >= t_last { return [count-1, count-1, 0.0] }
   mut lo = 0
   mut hi = count - 1
   while hi - lo > 1 {
      def mid = (lo + hi) / 2
      def t_mid = f32le(ptr, mid * stride)
      if t_mid <= time_sec { lo = mid }
      else { hi = mid }
   }
   def t_lo = f32le(ptr, lo * stride)
   def t_hi = f32le(ptr, hi * stride)
   def alpha = _gltf_anim_alpha(time_sec, t_lo, t_hi)
   [lo, hi, alpha]
}

fn _gltf_sample_channel(any data, dict sampler, dict input_res, dict output_res, f64 time_sec, int n_comp, bool is_rotation, bool packed_scalar_tuple=false) list {
   def interp = _gltf_anim_interp_mode(sampler.get("interpolation", "LINEAR"))
   def bracket = _gltf_find_time_bracket(data, input_res, time_sec)
   def lo = int(bracket.get(0, 0))
   def hi = int(bracket.get(1, 0))
   mut t = _gltf_clamp01(float(bracket.get(2, 0.0)))
   def rot_short_norm =
   is_rotation &&
   n_comp == 4 &&
   output_res.get("comp", 0) == GLTF_COMP_SHORT &&
   output_res.get("normalized", false) &&
   int(output_res.get("cols", 1)) <= 1
   if rot_short_norm {
      def ptr = output_res.get("ptr", 0)
      def stride = int(output_res.get("stride", 0))
      if ptr && stride >= 8 {
         def alo = lo * stride
         def ahi = hi * stride
         def qa = _gltf_read_norm_i16_quat(ptr, alo)
         if lo == hi || eq(interp, "STEP") { return qa }
         def qb = _gltf_read_norm_i16_quat(ptr, ahi)
         return _gltf_nlerp_quat(qa, qb, t)
      }
   }
   if lo == hi {
      def base_idx = eq(interp, "CUBICSPLINE") ? lo * 3 + 1 : lo
      return _gltf_read_anim_tuple(data, output_res, base_idx, n_comp, packed_scalar_tuple)
   }
   if eq(interp, "STEP") {
      return _gltf_read_anim_tuple(data, output_res, lo, n_comp, packed_scalar_tuple)
   }
   if eq(interp, "CUBICSPLINE") {
      def tk = _gltf_read_acc_f32(data, input_res, lo, 0)
      def tk1 = _gltf_read_acc_f32(data, input_res, hi, 0)
      def td = tk1 - tk
      def vk = _gltf_read_anim_tuple(data, output_res, lo * 3 + 1, n_comp, packed_scalar_tuple)
      def bk = _gltf_read_anim_tuple(data, output_res, lo * 3 + 2, n_comp, packed_scalar_tuple)
      def ak1 = _gltf_read_anim_tuple(data, output_res, hi * 3 + 0, n_comp, packed_scalar_tuple)
      def vk1 = _gltf_read_anim_tuple(data, output_res, hi * 3 + 1, n_comp, packed_scalar_tuple)
      def t2, t3 = t * t, t2 * t
      def h00, h10 = 2.0 * t3 - 3.0 * t2 + 1.0, t3 - 2.0 * t2 + t
      def h01, h11 = -2.0 * t3 + 3.0 * t2, t3 - t2
      mut out = list(max(0, n_comp))
      if n_comp > 0 { __list_set_len(out, n_comp) }
      mut i = 0
      while i < n_comp {
         out[i] = h00 * (0.0 + vk.get(i, 0.0)) +
            h10 * td * (0.0 + bk.get(i, 0.0)) +
            h01 * (0.0 + vk1.get(i, 0.0)) +
         h11 * td * (0.0 + ak1.get(i, 0.0))
         i += 1
      }
      if is_rotation { return _gltf_normalize_quat(out) }
      return out
   }
   def a_val = _gltf_read_anim_tuple(data, output_res, lo, n_comp, packed_scalar_tuple)
   def b_val = _gltf_read_anim_tuple(data, output_res, hi, n_comp, packed_scalar_tuple)
   if is_rotation { return _gltf_nlerp_quat(a_val, b_val, t) }
   _gltf_lerp_vec(a_val, b_val, t, n_comp)
}

fn gltf_animation_count(any gltf_data) int {
   "Returns the number of animations in the glTF asset."
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return 0 }
   def anims = g.get("animations", 0)
   is_list(anims) ? anims.len : 0
}

fn _gltf_animation_duration_from_anim(any doc, int anim_idx) f64 {
   if !is_dict(doc) { return 0.0 }
   def anims = doc.get("animations", 0)
   def accs = doc.get("accessors", 0)
   if !is_list(anims) || !is_list(accs) || anim_idx < 0 || anim_idx >= anims.len { return 0.0 }
   def anim = anims[anim_idx]
   if !is_dict(anim) { return 0.0 }
   def samplers = anim.get("samplers", [])
   if !is_list(samplers) { return 0.0 }
   def samplers_n = samplers.len
   def accs_n = accs.len
   mut duration = 0.0
   mut si = 0
   while si < samplers_n {
      def samp = samplers[si]
      if !is_dict(samp) { si += 1 continue }
      def input_raw = samp.get("input", -1)
      if !is_int(input_raw) && !is_float(input_raw) { si += 1 continue }
      def input_idx = int(input_raw)
      if input_idx >= 0 && input_idx < accs_n {
         def input_acc = accs[input_idx]
         if is_dict(input_acc) {
            def acc_max = input_acc.get("max", 0)
            if is_list(acc_max) && acc_max.len > 0 {
               def last_t = shr._gltf_num_or(acc_max.get(0, 0.0), 0.0)
               if last_t > duration { duration = last_t }
            }
         }
      }
      si += 1
   }
   duration
}

fn _gltf_animation_duration_from_samples(any gltf_data, int anim_idx) f64 {
   if !is_dict(gltf_data) { return 0.0 }
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return 0.0 }
   def data = ld._gltf_primary_data_ptr(gltf_data)
   if !data { return 0.0 }
   def anims = g.get("animations", 0)
   if !is_list(anims) || anim_idx < 0 || anim_idx >= anims.len { return 0.0 }
   def anim = anims[anim_idx]
   if !is_dict(anim) { return 0.0 }
   def samplers = anim.get("samplers", [])
   if !is_list(samplers) { return 0.0 }
   def samplers_n = samplers.len
   mut duration = 0.0
   mut si = 0
   while si < samplers_n {
      def samp = samplers[si]
      if !is_dict(samp) { si += 1 continue }
      def input_idx = int(samp.get("input", -1))
      def input_res = ld._gltf_resolve_accessor_data(g, input_idx, data)
      if is_dict(input_res) {
         def count = int(input_res.get("count", 0))
         if count > 0 {
            def last_t = _gltf_read_acc_f32(data, input_res, count - 1, 0)
            if shr._gltf_anim_duration_valid(last_t) && last_t > duration { duration = last_t }
         }
         ld._gltf_release_accessor_data(input_res)
      }
      si += 1
   }
   duration
}

fn gltf_animation_info(any gltf_data, int anim_idx) any {
   "Returns {name, duration} for animation at anim_idx. Duration in seconds."
   shr._gltf_ensure_caches()
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return 0 }
   def anims = g.get("animations", 0)
   if !is_list(anims) || anim_idx < 0 || anim_idx >= anims.len { return 0 }
   def anims_n = anims.len
   def anim_cache_key = to_str(gltf_data.get("source_path", "")) + "|" + to_str(anim_idx) + "|" + to_str(anims_n)
   if _gltf_anim_info_cache.contains(anim_cache_key) { return _gltf_anim_info_cache.get(anim_cache_key, 0) }
   def anim = anims.get(anim_idx)
   mut duration = _gltf_animation_duration_from_anim(g, anim_idx)
   if !shr._gltf_anim_duration_valid(duration) { duration = _gltf_animation_duration_from_samples(gltf_data, anim_idx) }
   if !shr._gltf_anim_duration_valid(duration) { duration = float(gltf_data.get("anim_duration_hint", 0.0)) }
   if !shr._gltf_anim_duration_valid(duration) { duration = 0.0 }
   def out = {"name": to_str(anim.get("name", "")), "duration": duration}
   _gltf_anim_info_cache = cache.cache_put_reset(shr._gltf_anim_info_cache, anim_cache_key, out, shr._GLTF_CACHE_LIMIT_MED, 64)
   out
}

fn _gltf_anim_fast_records_fail(str key) any {
   _gltf_anim_sample_cache = cache.cache_put_reset(shr._gltf_anim_sample_cache, key, false, shr._GLTF_CACHE_LIMIT_MED, 64)
   false
}

fn _gltf_anim_fast_records(any gltf_data, int anim_idx) any {
   shr._gltf_ensure_caches()
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return 0 }
   def data = ld._gltf_primary_data_ptr(gltf_data)
   if !data { return 0 }
   def key = shr._gltf_cache_key_from_data(gltf_data) + ":data:" + to_str(to_int(data)) + ":anim:" + to_str(int(anim_idx))
   if _gltf_anim_sample_cache.contains(key) { return _gltf_anim_sample_cache.get(key, 0) }
   def anims = g.get("animations", 0)
   if !is_list(anims) || anim_idx < 0 || anim_idx >= anims.len { return _gltf_anim_fast_records_fail(key) }
   def anim = anims.get(anim_idx)
   if !is_dict(anim) { return _gltf_anim_fast_records_fail(key) }
   def channels = anim.get("channels", [])
   def samplers = anim.get("samplers", [])
   if !is_list(channels) || !is_list(samplers) { return _gltf_anim_fast_records_fail(key) }
   def channels_n = channels.len
   def samplers_n = samplers.len
   mut records = list(0)
   mut ci = 0
   while ci < channels_n {
      def ch = channels[ci]
      def tgt = is_dict(ch) ? ch.get("target", 0) : 0
      def samp_idx = is_dict(ch) ? int(ch.get("sampler", -1)) : -1
      if !is_dict(tgt) || samp_idx < 0 || samp_idx >= samplers_n { return _gltf_anim_fast_records_fail(key) }
      if is_dict(tgt.get("extensions", 0)) { return _gltf_anim_fast_records_fail(key) }
      def node_idx = int(tgt.get("node", -1))
      def path = to_str(tgt.get("path", ""))
      mut path_code = 0
      mut n_comp = 0
      if path == "translation" { path_code = 1 n_comp = 3 }
      elif path == "rotation" { path_code = 2 n_comp = 4 }
      elif path == "scale" { path_code = 3 n_comp = 3 }
      else { return _gltf_anim_fast_records_fail(key) }
      if node_idx < 0 { return _gltf_anim_fast_records_fail(key) }
      def samp = samplers[samp_idx]
      if !is_dict(samp) { return _gltf_anim_fast_records_fail(key) }
      def interp = _gltf_anim_interp_mode(samp.get("interpolation", "LINEAR"))
      mut interp_code = 0
      if interp == "STEP" { interp_code = 1 }
      elif interp != "LINEAR" { return _gltf_anim_fast_records_fail(key) }
      def input_res = ld._gltf_resolve_accessor_data(g, int(samp.get("input", -1)), data)
      def output_res = ld._gltf_resolve_accessor_data(g, int(samp.get("output", -1)), data)
      if !is_dict(input_res) || !is_dict(output_res) {
         ld._gltf_release_accessor_data(input_res)
         ld._gltf_release_accessor_data(output_res)
         return _gltf_anim_fast_records_fail(key)
      }
      if input_res.get("owned", false) || output_res.get("owned", false) {
         ld._gltf_release_accessor_data(input_res)
         ld._gltf_release_accessor_data(output_res)
         return _gltf_anim_fast_records_fail(key)
      }
      if int(input_res.get("comp", 0)) != GLTF_COMP_FLOAT || int(output_res.get("comp", 0)) != GLTF_COMP_FLOAT {
         ld._gltf_release_accessor_data(input_res)
         ld._gltf_release_accessor_data(output_res)
         return _gltf_anim_fast_records_fail(key)
      }
      if int(input_res.get("type_count", 1)) != 1 || int(output_res.get("type_count", 0)) != n_comp {
         ld._gltf_release_accessor_data(input_res)
         ld._gltf_release_accessor_data(output_res)
         return _gltf_anim_fast_records_fail(key)
      }
      def in_ptr = input_res.get("ptr", 0)
      def out_ptr = output_res.get("ptr", 0)
      def count = int(input_res.get("count", 0))
      if !in_ptr || !out_ptr || count <= 0 || int(output_res.get("count", 0)) < count {
         ld._gltf_release_accessor_data(input_res)
         ld._gltf_release_accessor_data(output_res)
         return _gltf_anim_fast_records_fail(key)
      }
      records = records.append([node_idx, path_code, in_ptr, out_ptr, count, int(input_res.get("stride", 4)), int(output_res.get("stride", n_comp * 4)), n_comp, 0, interp_code])
      ld._gltf_release_accessor_data(input_res)
      ld._gltf_release_accessor_data(output_res)
      ci += 1
   }
   if records.len != channels_n { return _gltf_anim_fast_records_fail(key) }
   _gltf_anim_sample_cache = cache.cache_put_reset(shr._gltf_anim_sample_cache, key, records, shr._GLTF_CACHE_LIMIT_MED, 64)
   records
}

fn _gltf_anim_record_bracket(any rec, f64 time_sec) list {
   def in_ptr = rec.get(2, 0)
   def count = int(rec.get(4, 0))
   def stride = int(rec.get(5, 4))
   if !in_ptr || count <= 1 { return [0, 0, 0.0] }
   def first_t = f32le(in_ptr, 0)
   if time_sec <= first_t {
      rec[8] = 0
      return [0, 0, 0.0]
   }
   def last_t = f32le(in_ptr, (count - 1) * stride)
   if time_sec >= last_t {
      rec[8] = count - 1
      return [count - 1, count - 1, 0.0]
   }
   mut lo = int(rec.get(8, 0))
   if lo < 0 || lo >= count - 1 { lo = 0 }
   mut t_lo = f32le(in_ptr, lo * stride)
   mut t_hi = f32le(in_ptr, (lo + 1) * stride)
   while lo > 0 && time_sec < t_lo {
      lo -= 1
      t_lo = f32le(in_ptr, lo * stride)
      t_hi = f32le(in_ptr, (lo + 1) * stride)
   }
   while lo + 1 < count - 1 && time_sec >= t_hi {
      lo += 1
      t_lo = t_hi
      t_hi = f32le(in_ptr, (lo + 1) * stride)
   }
   rec[8] = lo
   def alpha = _gltf_anim_alpha(time_sec, t_lo, t_hi)
   [lo, lo + 1, alpha]
}

fn _gltf_anim_clean_tiny(f64 v) f64 {
   abs(v) < 0.000000000001 ? 0.0 : v
}

fn _gltf_anim_fast_tuple(any rec, int idx) list {
   def out_ptr = rec.get(3, 0)
   def stride = int(rec.get(6, 0))
   def n_comp = int(rec.get(7, 0))
   def off = idx * stride
   if n_comp == 4 {
      return [
         _gltf_anim_clean_tiny(f32le(out_ptr, off + 0)),
         _gltf_anim_clean_tiny(f32le(out_ptr, off + 4)),
         _gltf_anim_clean_tiny(f32le(out_ptr, off + 8)),
         _gltf_anim_clean_tiny(f32le(out_ptr, off + 12))
      ]
   }
   [
      _gltf_anim_clean_tiny(f32le(out_ptr, off + 0)),
      _gltf_anim_clean_tiny(f32le(out_ptr, off + 4)),
      _gltf_anim_clean_tiny(f32le(out_ptr, off + 8))
   ]
}

fn _gltf_anim_fast_value(any rec, f64 time_sec) list {
   def br = _gltf_anim_record_bracket(rec, time_sec)
   def lo = int(br.get(0, 0))
   def hi = int(br.get(1, 0))
   def t = _gltf_clamp01(float(br.get(2, 0.0)))
   if lo == hi || int(rec.get(9, 0)) == 1 { return _gltf_anim_fast_tuple(rec, lo) }
   def out_ptr = rec.get(3, 0)
   def stride = int(rec.get(6, 0))
   def n_comp = int(rec.get(7, 0))
   def lo_off = lo * stride
   def hi_off = hi * stride
   if n_comp == 4 && int(rec.get(1, 0)) == 2 {
      def ax = f32le(out_ptr, lo_off + 0)
      def ay = f32le(out_ptr, lo_off + 4)
      def az = f32le(out_ptr, lo_off + 8)
      def aw = f32le(out_ptr, lo_off + 12)
      mut bx = f32le(out_ptr, hi_off + 0)
      mut by = f32le(out_ptr, hi_off + 4)
      mut bz = f32le(out_ptr, hi_off + 8)
      mut bw = f32le(out_ptr, hi_off + 12)
      mut dot = ax * bx + ay * by + az * bz + aw * bw
      if dot < 0.0 { bx = -bx by = -by bz = -bz bw = -bw dot = 0.0 - dot }
      if dot > 0.9995 {
         def rx = ax + (bx - ax) * t
         def ry = ay + (by - ay) * t
         def rz = az + (bz - az) * t
         def rw = aw + (bw - aw) * t
         def len2 = rx * rx + ry * ry + rz * rz + rw * rw
         if len2 <= 0.000001 { return [0.0, 0.0, 0.0, 1.0] }
         def inv_len = 1.0 / sqrt(len2)
         return [
            _gltf_anim_clean_tiny(rx * inv_len),
            _gltf_anim_clean_tiny(ry * inv_len),
            _gltf_anim_clean_tiny(rz * inv_len),
            _gltf_anim_clean_tiny(rw * inv_len)
         ]
      }
      if dot > 1.0 { dot = 1.0 }
      def theta_0 = acos(dot)
      def sin_theta_0 = sin(theta_0)
      if abs(sin_theta_0) <= 0.000001 { return [ax, ay, az, aw] }
      def theta = theta_0 * t
      def sin_theta = sin(theta)
      def s0 = cos(theta) - dot * sin_theta / sin_theta_0
      def s1 = sin_theta / sin_theta_0
      def rx = ax * s0 + bx * s1
      def ry = ay * s0 + by * s1
      def rz = az * s0 + bz * s1
      def rw = aw * s0 + bw * s1
      def len2 = rx * rx + ry * ry + rz * rz + rw * rw
      if len2 <= 0.000001 { return [0.0, 0.0, 0.0, 1.0] }
      def inv_len = 1.0 / sqrt(len2)
      return [
         _gltf_anim_clean_tiny(rx * inv_len),
         _gltf_anim_clean_tiny(ry * inv_len),
         _gltf_anim_clean_tiny(rz * inv_len),
         _gltf_anim_clean_tiny(rw * inv_len)
      ]
   }
   if n_comp == 4 {
      def ax = f32le(out_ptr, lo_off + 0)
      def ay = f32le(out_ptr, lo_off + 4)
      def az = f32le(out_ptr, lo_off + 8)
      def aw = f32le(out_ptr, lo_off + 12)
      def bx = f32le(out_ptr, hi_off + 0)
      def by = f32le(out_ptr, hi_off + 4)
      def bz = f32le(out_ptr, hi_off + 8)
      def bw = f32le(out_ptr, hi_off + 12)
      return [
         _gltf_anim_clean_tiny(ax + (bx - ax) * t),
         _gltf_anim_clean_tiny(ay + (by - ay) * t),
         _gltf_anim_clean_tiny(az + (bz - az) * t),
         _gltf_anim_clean_tiny(aw + (bw - aw) * t)
      ]
   }
   def ax = f32le(out_ptr, lo_off + 0)
   def ay = f32le(out_ptr, lo_off + 4)
   def az = f32le(out_ptr, lo_off + 8)
   def bx = f32le(out_ptr, hi_off + 0)
   def by = f32le(out_ptr, hi_off + 4)
   def bz = f32le(out_ptr, hi_off + 8)
   [
      _gltf_anim_clean_tiny(ax + (bx - ax) * t),
      _gltf_anim_clean_tiny(ay + (by - ay) * t),
      _gltf_anim_clean_tiny(az + (bz - az) * t)
   ]
}

fn _gltf_sample_animation_fast(any gltf_data, int anim_idx, f64 time_sec) any {
   if !shr._gltf_anim_fast_enabled() { return 0 }
   def g = gltf_data.get("gltf", 0)
   def skins = is_dict(g) ? g.get("skins", 0) : 0
   if is_list(skins) && skins.len > 0 && !shr._gltf_anim_fast_skin_enabled() { return 0 }
   def records = _gltf_anim_fast_records(gltf_data, anim_idx)
   if !is_list(records) { return 0 }
   def nodes = is_dict(g) ? g.get("nodes", 0) : 0
   def nodes_n = is_list(nodes) ? nodes.len : 0
   mut overrides = dict(max(64, nodes_n * 4 + 16))
   mut node_slots = dict(max(32, nodes_n * 2 + 8))
   mut node_ids = list(0)
   mut node_keys = list(0)
   mut t_values = list(0)
   mut r_values = list(0)
   mut s_values = list(0)
   mut node_overrides = list(0)
   def rec_n = records.len
   mut i = 0
   while i < rec_n {
      def rec = records[i]
      def node_idx = int(rec.get(0, -1))
      def path_code = int(rec.get(1, 0))
      def val = _gltf_anim_fast_value(rec, time_sec)
      def node_key = to_str(node_idx)
      mut slot = int(node_slots.get(node_key, -1))
      if slot < 0 || slot >= node_ids.len {
         slot = node_ids.len
         node_ids = node_ids.append(node_idx)
         node_keys = node_keys.append(node_key)
         t_values = t_values.append(0)
         r_values = r_values.append(0)
         s_values = s_values.append(0)
         node_slots[node_key] = slot
      }
      if path_code == 1 { t_values[slot] = val }
      elif path_code == 2 { r_values[slot] = val }
      elif path_code == 3 { s_values[slot] = val }
      i += 1
   }
   mut fast_node_overrides = list(nodes_n)
   if nodes_n > 0 { __list_set_len(fast_node_overrides, nodes_n) }
   mut ni = 0
   while ni < nodes_n {
      fast_node_overrides[ni] = 0
      ni += 1
   }
   def node_n = node_ids.len
   mut oi = 0
   while oi < node_n {
      def node_idx = int(node_ids[oi])
      def node_key = to_str(node_keys[oi])
      mut node_ov = {"node": node_idx, "node_key": node_key}
      def tv = t_values[oi]
      def rv = r_values[oi]
      def sv = s_values[oi]
      if is_list(tv) { node_ov["T"] = tv }
      if is_list(rv) { node_ov["R"] = rv }
      if is_list(sv) { node_ov["S"] = sv }
      node_overrides = node_overrides.append(node_ov)
      if node_idx >= 0 && node_idx < nodes_n { fast_node_overrides[node_idx] = node_ov }
      overrides[node_idx] = node_ov
      overrides[node_key] = node_ov
      oi += 1
   }
   overrides["__nodes"] = node_overrides
   overrides["__fast_node_overrides"] = fast_node_overrides
   overrides["__fast_numeric"] = true
   overrides
}

fn _gltf_anim_sample_component_count(str path, bool is_visibility_pointer, any ptr_mat_target, any input_res, any output_res, int key_mult) int {
   if is_visibility_pointer { return 1 }
   if is_dict(ptr_mat_target) {
      def kind = to_str(ptr_mat_target.get("kind", ""))
      if kind == "baseColorFactor" { return 4 }
      if kind == "emissiveFactor" { return 3 }
      if kind == "metallicFactor" || kind == "roughnessFactor" || kind == "alphaCutoff" { return 1 }
      if kind == "uvOffset" || kind == "uvScale" { return 2 }
      if kind == "uvRotation" || kind == "uvTexCoord" { return 1 }
   }
   if eq(path, "rotation") { return 4 }
   if eq(path, "weights") { return max(1, int(output_res.get("count", 0)) / max(1, int(input_res.get("count", 1)) * key_mult)) }
   3
}

fn _gltf_anim_store_override(
   dict overrides, bool is_material_pointer, any ptr_mat_target,
   bool is_visibility_pointer, int ptr_vis_node_idx, int node_idx, str path, any val
) dict {
   if is_material_pointer {
      mut ptrs = overrides.get("__pointers", [])
      ptrs = ptrs.append({"material": int(ptr_mat_target.get("material", -1)), "kind": to_str(ptr_mat_target.get("kind", "")), "slot": to_str(ptr_mat_target.get("slot", "")), "value": val})
      overrides["__pointers"] = ptrs
      return overrides
   }
   def dst_node_idx = is_visibility_pointer ? ptr_vis_node_idx : node_idx
   mut node_ov = shr._gltf_anim_override_for_node(overrides, dst_node_idx)
   if !is_dict(node_ov) { node_ov = dict(4) }
   node_ov["node"] = dst_node_idx
   node_ov["node_key"] = to_str(dst_node_idx)
   def key = is_visibility_pointer ? "VIS" : (eq(path, "translation") ? "T" : (eq(path, "rotation") ? "R" : (eq(path, "scale") ? "S" : "W")))
   if is_visibility_pointer {
      def visf = 0.0 + val.get(0, 1.0)
      node_ov[key] = visf >= 0.5
   } else {
      node_ov[key] = val
   }
   overrides[dst_node_idx] = node_ov
   overrides[to_str(dst_node_idx)] = node_ov
   mut nodes_ov = overrides.get("__nodes", [])
   mut found = false
   mut ni = 0
   while is_list(nodes_ov) && ni < nodes_ov.len {
      def rec = nodes_ov[ni]
      if is_dict(rec) && to_str(rec.get("node_key", "")) == to_str(dst_node_idx) {
         nodes_ov[ni] = node_ov
         found = true
         break
      }
      ni += 1
   }
   if !found { nodes_ov = nodes_ov.append(node_ov) }
   overrides["__nodes"] = nodes_ov
   overrides
}

fn _gltf_anim_wrap_time_value(f64 t_raw, f64 dur) f64 {
   mut t = float(t_raw)
   if is_nan(t) || is_inf(t) { t = 0.0 }
   if abs(t) > 1000000.0 { t = 0.0 }
   if dur > 0.0001 {
      while t >= dur { t -= dur }
      while t < 0.0 { t += dur }
   }
   t
}

fn _gltf_anim_wrap_time(any gltf_data, int anim_idx, f64 time_sec) f64 {
   if anim_idx < 0 { return _gltf_anim_wrap_time_value(time_sec, float(gltf_data.get("anim_duration_hint", 0.0))) }
   def info = gltf_animation_info(gltf_data, anim_idx)
   def dur = is_dict(info) ? float(info.get("duration", 0.0)) : 0.0
   _gltf_anim_wrap_time_value(time_sec, dur)
}

fn gltf_sample_animation(any gltf_data, int anim_idx, f64 time_sec) any {
   "Sample animation at time_sec. Returns {node_idx: {T:[x,y,z], R:[x,y,z,w], S:[x,y,z]}} overrides."
   def sample_t = _gltf_anim_wrap_time(gltf_data, anim_idx, time_sec)
   def fast = _gltf_sample_animation_fast(gltf_data, anim_idx, sample_t)
   if is_dict(fast) { return fast }
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return 0 }
   def anims = g.get("animations", 0)
   if !is_list(anims) || anim_idx < 0 || anim_idx >= anims.len { return 0 }
   def anim = anims.get(anim_idx)
   def data = ld._gltf_primary_data_ptr(gltf_data)
   if !data { return 0 }
   def channels = anim.get("channels", [])
   def samplers  = anim.get("samplers", [])
   def nodes = g.get("nodes", 0)
   def channels_n = is_list(channels) ? channels.len : 0
   def samplers_n = is_list(samplers) ? samplers.len : 0
   def nodes_n = is_list(nodes) ? nodes.len : 0
   mut overrides = dict(max(16, nodes_n * 2))
   mut ci = 0
   while ci < channels_n {
      def ch = channels.get(ci)
      def tgt = ch.get("target", 0)
      def samp_idx = int(ch.get("sampler", -1))
      if is_dict(tgt) && samp_idx >= 0 && samp_idx < samplers_n {
         def tgt_ext = tgt.get("extensions", 0)
         def ptr_ext = is_dict(tgt_ext) ? tgt_ext.get("KHR_animation_pointer", 0) : 0
         def ptr = is_dict(ptr_ext) ? to_str(ptr_ext.get("pointer", "")) : ""
         def node_idx = int(tgt.get("node", -1))
         def path = to_str(tgt.get("path", ""))
         def ptr_vis_node_idx = shr._gltf_pointer_node_visibility_idx(ptr)
         def is_visibility_pointer = ptr_vis_node_idx >= 0
         def ptr_mat_target = shr._gltf_pointer_material_target(ptr)
         def is_material_pointer = is_dict(ptr_mat_target)
         if (node_idx >= 0
            && (eq(path, "translation")
               || eq(path, "rotation")
               || eq(path, "scale")
         || eq(path, "weights")))
         || is_visibility_pointer
         || is_material_pointer{
            def samp = samplers.get(samp_idx)
            def input_idx  = int(samp.get("input",  -1))
            def output_idx = int(samp.get("output", -1))
            def input_res  = ld._gltf_resolve_accessor_data(g, input_idx,  data)
            def output_res = ld._gltf_resolve_accessor_data(g, output_idx, data)
            if is_dict(input_res) && is_dict(output_res) {
               def key_mult = eq(_gltf_anim_interp_mode(samp.get("interpolation", "LINEAR")), "CUBICSPLINE") ? 3 : 1
               def n_comp = _gltf_anim_sample_component_count(path, is_visibility_pointer, ptr_mat_target, input_res, output_res, key_mult)
               def is_rot = eq(path, "rotation")
               def packed_scalar_tuple =
               eq(path, "weights") &&
               int(output_res.get("type_count", 1)) == 1 &&
               n_comp > 1
               def val = _gltf_sample_channel(data, samp, input_res, output_res, sample_t, n_comp, is_rot, packed_scalar_tuple)
               overrides = _gltf_anim_store_override(overrides, is_material_pointer, ptr_mat_target, is_visibility_pointer, ptr_vis_node_idx, node_idx, path, val)
            }
            ld._gltf_release_accessor_data(input_res)
            ld._gltf_release_accessor_data(output_res)
         }
      }
      ci += 1
   }
   overrides
}

fn gltf_sample_animation_merged(any gltf_data, f64 time_sec) any {
   "Samples all animation clips at time_sec and merges their overrides."
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return 0 }
   def anims = g.get("animations", 0)
   if !is_list(anims) || anims.len == 0 { return 0 }
   def anims_n = anims.len
   def node_defs = g.get("nodes", 0)
   mut merged = dict(is_list(node_defs) ? max(16, node_defs.len * 2) : 16)
   mut ai = 0
   while ai < anims_n {
      def clip_t = _gltf_anim_wrap_time(gltf_data, ai, time_sec)
      def clip = gltf_sample_animation(gltf_data, ai, clip_t)
      if is_dict(clip) {
         def clip_nodes = clip.get("__nodes", [])
         if is_list(clip_nodes) {
            def clip_nodes_n = clip_nodes.len
            mut ni = 0
            while ni < clip_nodes_n {
               def rec = clip_nodes[ni]
               if is_dict(rec) {
                  def node_idx = int(rec.get("node", -1))
                  if node_idx >= 0 {
                     ;; Merge channels per node instead of replacing the whole
                     ;; node override record. Multi-channel/multi-clip assets can
                     ;; target translation, rotation, scale, visibility, and
                     ;; weights independently for the same node.
                     mut merged_rec = merged.get(node_idx, merged.get(to_str(node_idx), 0))
                     if !is_dict(merged_rec) { merged_rec = {"node": node_idx, "node_key": to_str(node_idx)} }
                     merged_rec["node"] = node_idx
                     merged_rec["node_key"] = to_str(node_idx)
                     if rec.contains("T") { merged_rec["T"] = rec.get("T") }
                     if rec.contains("R") { merged_rec["R"] = rec.get("R") }
                     if rec.contains("S") { merged_rec["S"] = rec.get("S") }
                     if rec.contains("W") { merged_rec["W"] = rec.get("W") }
                     if rec.contains("VIS") { merged_rec["VIS"] = rec.get("VIS") }
                     merged[node_idx] = merged_rec
                     merged[to_str(node_idx)] = merged_rec
                     mut merged_nodes = merged.get("__nodes", [])
                     mut found = false
                     mut mi = 0
                     def merged_nodes_n = is_list(merged_nodes) ? merged_nodes.len : 0
                     while mi < merged_nodes_n {
                        def prev = merged_nodes[mi]
                        if is_dict(prev) && int(prev.get("node", -1)) == node_idx {
                           merged_nodes[mi] = merged_rec
                           found = true
                           break
                        }
                        mi += 1
                     }
                     if !found { merged_nodes = merged_nodes.append(merged_rec) }
                     merged["__nodes"] = merged_nodes
                  }
               }
               ni += 1
            }
         }
         def ptrs = clip.get("__pointers", [])
         if is_list(ptrs) {
            mut merged_ptrs = merged.get("__pointers", [])
            def ptrs_n = ptrs.len
            mut pi = 0
            while pi < ptrs_n {
               merged_ptrs = merged_ptrs.append(ptrs[pi])
               pi += 1
            }
            merged["__pointers"] = merged_ptrs
         }
      }
      ai += 1
   }
   merged
}

fn gltf_apply_morph_weights(any gltf_data, any overrides) list {
   "Applies sampled node weights overrides onto node.weights.
   glTF morph animation targets a node, not the shared mesh. Keeping weights on
   the node lets several instances of the same mesh hold different poses."
   if !is_dict(gltf_data) || !is_dict(overrides) { return [gltf_data, false] }
   mut g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return [gltf_data, false] }
   mut nodes = g.get("nodes", 0)
   def meshes = g.get("meshes", 0)
   if !is_list(nodes) || !is_list(meshes) { return [gltf_data, false] }
   def nodes_n = nodes.len
   def meshes_n = meshes.len
   mut changed = false
   mut ni = 0
   while ni < nodes_n {
      mut node = nodes[ni]
      if is_dict(node) {
         def node_ov = overrides.get(ni, 0)
         if is_dict(node_ov) {
            def w = node_ov.get("W", 0)
            if is_list(w) {
               def mesh_idx = int(node.get("mesh", -1))
               if mesh_idx >= 0 && mesh_idx < meshes_n {
                  def w_n = w.len
                  mut out_w = list(w_n)
                  mut wi = 0
                  mut same = false
                  def old_w = node.get("weights", 0)
                  if is_list(old_w) && old_w.len == w_n { same = true }
                  while wi < w_n {
                     def next_w = float(w[wi])
                     __store_item_fast(out_w, wi, next_w)
                     if same && abs(float(old_w.get(wi, 0.0)) - next_w) > 0.00001 { same = false }
                     wi += 1
                  }
                  __list_set_len(out_w, w_n)
                  if !same {
                     node["weights"] = out_w
                     nodes[ni] = node
                     changed = true
                  }
               }
            }
         }
      }
      ni += 1
   }
   if !changed { return [gltf_data, false] }
   g["nodes"] = nodes
   gltf_data["gltf"] = g
   [gltf_data, true]
}


fn _gltf_build_node_world_mats_animated_fast(list nodes, any base_local_mats, list world_list, any fast_node_overrides, int node_idx, list parent_m, dict node_world_mats, dict overrides) dict {
   if !is_list(nodes) || node_idx < 0 || node_idx >= nodes.len { return node_world_mats }
   def node = nodes[node_idx]
   if !is_dict(node) { return node_world_mats }
   mut anim_ov = is_list(fast_node_overrides) ? fast_node_overrides.get(node_idx, 0) : 0
   if !is_dict(anim_ov) { anim_ov = overrides.get(int(node_idx), overrides.get(to_str(node_idx), 0)) }
   mut local_m = 0
   if is_dict(anim_ov) {
      def t = anim_ov.get("T", node.get("translation", [0.0,0.0,0.0]))
      def r_raw = anim_ov.get("R", node.get("rotation", [0.0,0.0,0.0,1.0]))
      def r = _gltf_normalize_quat(r_raw)
      def s = anim_ov.get("S", node.get("scale", [1.0,1.0,1.0]))
      local_m = gltf_math.mat4_from_trs(t, r, s)
   } else {
      local_m = base_local_mats.get(node_idx, gltf_math.mat4_identity())
   }
   def world_m = gltf_math.mat4_mul(parent_m, local_m)
   node_world_mats[node_idx] = world_m
   if is_list(world_list) && node_idx >= 0 && node_idx < world_list.len { world_list[node_idx] = world_m }
   def children = node.get("children")
   if is_list(children) {
      def children_n = children.len
      mut i = 0
      while i < children_n {
         def child_idx = int(children[i])
         if child_idx >= 0 && child_idx != node_idx {
            node_world_mats = _gltf_build_node_world_mats_animated_fast(nodes,
               base_local_mats,
               world_list,
               fast_node_overrides,
               child_idx,
               world_m,
               node_world_mats,
            overrides)
         }
         i += 1
      }
   }
   node_world_mats
}

fn gltf_rebuild_animated_mats(any gltf_data, any overrides) dict {
   "Rebuild world matrices for all nodes applying TRS overrides from gltf_sample_animation.
   Returns {node_idx: mat4} dict. Pass result node_idx lookups to update gpu_parts model matrices."
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return dict(0) }
   if !is_dict(overrides) { return dict(0) }
   def nodes = g.get("nodes", 0)
   def nodes_n = is_list(nodes) ? nodes.len : 0
   mut node_world_mats = dict(max(16, nodes_n * 2))
   def base_local_mats = shr._gltf_node_local_mats(g)
   mut world_list = list(nodes_n)
   if nodes_n > 0 { __list_set_len(world_list, nodes_n) }
   mut wi = 0
   while wi < nodes_n {
      world_list[wi] = 0
      wi += 1
   }
   mut fast_node_overrides = overrides.get("__fast_node_overrides", 0)
   if !is_list(fast_node_overrides) {
      fast_node_overrides = list(nodes_n)
      if nodes_n > 0 { __list_set_len(fast_node_overrides, nodes_n) }
      wi = 0
      while wi < nodes_n {
         fast_node_overrides[wi] = 0
         wi += 1
      }
      def ov_nodes = overrides.get("__nodes", 0)
      if is_list(ov_nodes) {
         def ov_n = ov_nodes.len
         mut oi = 0
         while oi < ov_n {
            def rec = ov_nodes[oi]
            if is_dict(rec) {
               def node_idx = int(rec.get("node", -1))
               if node_idx >= 0 && node_idx < nodes_n { fast_node_overrides[node_idx] = rec }
            }
            oi += 1
         }
      }
   }
   def scenes = g.get("scenes")
   def scene_idx = shr._gltf_active_scene_idx(g, gltf_data)
   if is_list(scenes) && scene_idx >= 0 && scene_idx < scenes.len {
      def scene = scenes.get(scene_idx)
      def roots = scene.get("nodes")
      if is_list(roots) {
         def roots_n = roots.len
         def id = gltf_math.mat4_identity()
         mut ri = 0
         while ri < roots_n {
            def root_idx = int(roots[ri])
          node_world_mats = _gltf_build_node_world_mats_animated_fast(nodes,
             base_local_mats,
             world_list,
             fast_node_overrides,
             root_idx,
             id,
             node_world_mats,
          overrides)
            ri += 1
         }
      }
   }
   if is_list(world_list) {
      node_world_mats["__world_list"] = world_list
   } else {
      mut world_list2 = list(nodes_n)
      if nodes_n > 0 { __list_set_len(world_list2, nodes_n) }
      mut wi2 = 0
      while wi2 < nodes_n {
         world_list2[wi2] = node_world_mats.get(wi2, 0)
         wi2 += 1
      }
      node_world_mats["__world_list"] = world_list2
   }
   node_world_mats
}

#main {
   def in_ptr = malloc(8)
   store32(in_ptr, 0x00000000, 0)
   store32(in_ptr, 0x3f800000, 4)
   def out_ptr = malloc(24)
   store32(out_ptr, 0x00000000, 0)
   store32(out_ptr, 0x00000000, 4)
   store32(out_ptr, 0x00000000, 8)
   store32(out_ptr, 0x40000000, 12)
   store32(out_ptr, 0x40800000, 16)
   store32(out_ptr, 0x40c00000, 20)
   mut rec = [0, 1, in_ptr, out_ptr, 2, 4, 12, 3, 0, 0]
   def interp = _gltf_anim_fast_value(rec, 0.5)
   assert(interp == [1.0, 2.0, 3.0], "gltf animation fast sampler interpolates vec3")
   def interp_quarter = _gltf_anim_fast_value(rec, 0.25)
   assert(interp_quarter == [0.5, 1.0, 1.5], "gltf animation fast sampler keeps in-between vec3")
   assert(rec[8] == 0, "gltf animation fast sampler updates bracket cache")

   def q_ptr = malloc(32)
   ;; identity -> 180deg around Z should slerp to a smooth 90deg turn at t=0.5
   store32_f32(q_ptr, 0.0, 0)
   store32_f32(q_ptr, 0.0, 4)
   store32_f32(q_ptr, 0.0, 8)
   store32_f32(q_ptr, 1.0, 12)
   store32_f32(q_ptr, 0.0, 16)
   store32_f32(q_ptr, 0.0, 20)
   store32_f32(q_ptr, 1.0, 24)
   store32_f32(q_ptr, 0.0, 28)
   mut qrec = [0, 2, in_ptr, q_ptr, 2, 4, 16, 4, 0, 0]
   def qquarter = _gltf_anim_fast_value(qrec, 0.25)
   assert(abs(float(qquarter.get(2, 0.0)) - 0.3826834) < 0.0003, "gltf animation fast sampler keeps in-between quat z")
   assert(abs(float(qquarter.get(3, 0.0)) - 0.9238795) < 0.0003, "gltf animation fast sampler keeps in-between quat w")
   def qhalf = _gltf_anim_fast_value(qrec, 0.5)
   assert(abs(float(qhalf.get(2, 0.0)) - 0.70710678) < 0.0002, "gltf animation fast sampler slerps quat z")
   assert(abs(float(qhalf.get(3, 0.0)) - 0.70710678) < 0.0002, "gltf animation fast sampler slerps quat w")

   def br_q = _gltf_anim_record_bracket(qrec, 0.25)
   assert(int(br_q.get(0, -1)) == 0 && int(br_q.get(1, -1)) == 1 && abs(float(br_q.get(2, -1.0)) - 0.25) < 0.000001, "gltf animation bracket keeps fractional alpha")
   def qm = gltf_math.mat4_from_trs([0.0,0.0,0.0], qhalf, [1.0,1.0,1.0])
   def qp = gltf_math.mat4_apply_pos(qm, 1.0, 0.0, 0.0)
   assert(abs(float(qp.get(0, 0.0))) < 0.0002 && abs(float(qp.get(1, 0.0)) - 1.0) < 0.0002, "gltf animation fast sampler rotates box point")
   free(q_ptr)
   free(in_ptr)
   free(out_ptr)

   def bind = malloc(shr._GLTF_VTX_STRIDE)
   def skinned = malloc(shr._GLTF_VTX_STRIDE)
   def joints = malloc(16)
   def weights = malloc(16)
   def skin_slab = malloc(64)
   memset(bind, 0, shr._GLTF_VTX_STRIDE)
   memset(skinned, 0, shr._GLTF_VTX_STRIDE)
   memset(joints, 0, 16)
   memset(weights, 0, 16)
   memset(skin_slab, 0, 64)
   store32_f32(bind, 1.0, shr._GLTF_VTX_OFF_X)
   store32_f32(bind, 2.0, shr._GLTF_VTX_OFF_Y)
   store32_f32(bind, 3.0, shr._GLTF_VTX_OFF_Z)
   store32_f32(bind, 0.0, shr._GLTF_VTX_OFF_NX)
   store32_f32(bind, 1.0, shr._GLTF_VTX_OFF_NY)
   store32_f32(bind, 0.0, shr._GLTF_VTX_OFF_NZ)
   store32(joints, 0, 0)
   store32_f32(weights, 1.0, 0)
   store32_f32(skin_slab, 1.0, 0)
   store32_f32(skin_slab, 1.0, 20)
   store32_f32(skin_slab, 1.0, 40)
   store32_f32(skin_slab, 10.0, 48)
   store32_f32(skin_slab, 1.0, 60)
   assert(_gltf_apply_skinning_slab(skinned, bind, joints, weights, 1, skin_slab, 1), "gltf skinning slab applies")
   assert(load32_f32(skinned, shr._GLTF_VTX_OFF_X) == 11.0, "gltf skinning slab x")
   assert(load32_f32(skinned, shr._GLTF_VTX_OFF_Y) == 2.0, "gltf skinning slab y")
   assert(load32_f32(skinned, shr._GLTF_VTX_OFF_Z) == 3.0, "gltf skinning slab z")
   assert(load32_f32(skinned, shr._GLTF_VTX_OFF_NY) == 1.0, "gltf skinning slab normal")
   free(bind)
   free(skinned)
   free(joints)
   free(weights)
   free(skin_slab)
   print("✓ std.math.parse.3d.gltf.animation self-test passed")
}
