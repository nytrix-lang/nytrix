;; Keywords: 3d gltf glb parse
;; Submodule: animation
module std.math.parse.3d.gltf.animation(_gltf_read_mat4_accessor_value, _gltf_mat4_transpose, _gltf_pack_skin_sidecars, gltf_skin_joint_mats, _gltf_skin_inv_bind_mats, _gltf_mesh_inv_key, _gltf_mesh_inv_cached, _gltf_pack_skin_mat_slab, _gltf_write_skin_mat_slab, _gltf_skin_mats_cache_record, gltf_free_skin_mats_cache, _gltf_apply_skinning_slab, _gltf_apply_skinning_one_slab, _gltf_apply_skinning_one_fast_slab, _gltf_apply_part_skin_slab, _gltf_part_runtime_skin_slab, _gltf_skin_weighted_mat4_vec3, _gltf_apply_skinning_fallback, gltf_apply_skinning, gltf_skin_count, gltf_skin_info, gltf_morph_target_count, _gltf_mesh_morph_weights, _gltf_collect_morph_targets, _gltf_release_morph_targets, _gltf_read_acc_f32, _gltf_read_acc_components, _gltf_read_acc_scalar_tuple, _gltf_read_anim_tuple, _gltf_lerp_vec, _gltf_nlerp_quat, _gltf_normalize_quat, _gltf_read_norm_i16_quat, _gltf_find_time_bracket, _gltf_sample_channel, gltf_animation_count, _gltf_animation_duration_from_anim, _gltf_animation_duration_from_samples, gltf_animation_info, _gltf_anim_fast_records, _gltf_anim_record_bracket, _gltf_anim_clean_tiny, _gltf_anim_fast_value, _gltf_sample_animation_fast, _gltf_anim_sample_component_count, _gltf_anim_store_override, gltf_sample_animation, gltf_sample_animation_merged, gltf_apply_morph_weights, _gltf_build_node_world_mats_animated, _gltf_build_node_world_mats_animated_fast, gltf_rebuild_animated_mats)
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
   mut vi = 0
   while vi < use_count {
      def joff, woff = vi * joints_stride, vi * weights_stride
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
         def wv = shr._gltf_read_f32_acc(
            weights_ptr,
            woff + k * max(1, shr._gltf_comp_size(weights_comp)),
            weights_comp,
            weights_norm
         )
         store32_f32(weights_sidecar, wv, vi * 16 + k * 4)
         k += 1
      }
      vi += 1
   }
   ld._gltf_release_accessor_data(joints_res)
   ld._gltf_release_accessor_data(weights_res)
   return {"joints_ptr": joints_sidecar, "weights_ptr": weights_sidecar, "count": use_count}
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
   mut out = list(0)
   mut ji = 0
   while ji < joints_n {
      def joint_idx = int(joints.get(ji, -1))
      def joint_world = node_world_mats.get(joint_idx, gltf_math.mat4_identity())
      mut inv_bind = inv_bind_mats.get(ji, gltf_math.mat4_identity())
      if skin_transpose_inv_bind { inv_bind = _gltf_mat4_transpose(inv_bind) }
      mut jm = skin_invbind_first ? gltf_math.mat4_mul(inv_bind, joint_world) : gltf_math.mat4_mul(joint_world, inv_bind)
      jm = gltf_math.mat4_mul(mesh_inv, jm)
      out = out.append(jm)
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
   mut ji = 0
   while ji < joints_n {
      if is_dict(inv_bind_res) && ji < int(inv_bind_res.get("count", 0)) { mats = mats.append(_gltf_read_mat4_accessor_value(inv_bind_res, ji)) } else { mats = mats.append(gltf_math.mat4_identity()) }
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
   def skin_raw_off = shr._gltf_skin_raw_off_enabled()
   def skin_no_mesh_inv = shr._gltf_skin_no_mesh_inv_enabled()
   def skin_transpose_inv_bind = shr._gltf_skin_transpose_inv_bind_enabled()
   def skin_invbind_first = shr._gltf_skin_invbind_first_enabled()
   if !skin_raw_off
   && !skin_no_mesh_inv
   && !skin_transpose_inv_bind
   && !skin_invbind_first{
      def runtime_skin = gltf_skin_info(gltf_data, skin_idx)
      def runtime_joints = is_dict(runtime_skin) ? runtime_skin.get("joints", []) : []
      def runtime_count = is_list(runtime_joints) ? runtime_joints.len : 0
      if runtime_count > 0 {
         def runtime_inv_bind = _gltf_skin_inv_bind_mats(gltf_data, skin_idx)
         if is_list(runtime_inv_bind) && runtime_inv_bind.len >= runtime_count {
            def runtime_mesh_inv = _gltf_mesh_inv_cached(mesh_bind_world)
            def slab = malloc(runtime_count * 64)
            if slab {
               def runtime_world_list = node_world_mats.get("__world_list", 0)
               if is_list(runtime_world_list) {
                  __gltf_skin_mats_store_raw(
                     slab,
                     runtime_joints,
                     runtime_world_list,
                     runtime_inv_bind,
                     runtime_mesh_inv,
                     runtime_count
                  )
               } else {
                  mut rji = 0
                  while rji < runtime_count {
                     def joint_idx = int(runtime_joints.get(rji, -1))
                     def joint_world = node_world_mats.get(joint_idx, gltf_math.mat4_identity())
                     def inv_bind = runtime_inv_bind.get(rji, gltf_math.mat4_identity())
                     __gltf_skin_mat_store_raw(slab, rji, joint_world, inv_bind, runtime_mesh_inv)
                     rji += 1
                  }
               }
               def rec = [0, slab, runtime_count]
               if is_dict(skin_mats_cache) {
                  skin_mats_cache[cache_key] = rec
                  mut keys = skin_mats_cache.get("__keys", [])
                  keys = keys.append(cache_key)
                  skin_mats_cache["__keys"] = keys
               }
               return rec
            }
         }
      }
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

fn _gltf_apply_skinning_slab(any vptr, any bind_vptr, any joints_ptr, any weights_ptr, int vcnt, any skin_slab, int mat_count) bool {
   if !vptr || !bind_vptr || !joints_ptr || !weights_ptr || !skin_slab || vcnt <= 0 || mat_count <= 0 { return false }
   __gltf_skin_apply_raw(vptr, bind_vptr, joints_ptr, weights_ptr, vcnt, skin_slab, mat_count)
}

fn _gltf_apply_skinning_one_slab(any vptr, any bind_vptr, any joints_ptr, int vcnt, any skin_slab, int mat_count) bool {
   if !vptr || !bind_vptr || !joints_ptr || !skin_slab || vcnt <= 0 || mat_count <= 0 { return false }
   __gltf_skin_apply_one_raw(vptr, bind_vptr, joints_ptr, vcnt, skin_slab, mat_count)
}

fn _gltf_apply_skinning_one_fast_slab(any vptr, any bind_vptr, any joints_ptr, int vcnt, any skin_slab, int mat_count) bool {
   if !vptr || !bind_vptr || !joints_ptr || !skin_slab || vcnt <= 0 || mat_count <= 0 { return false }
   __gltf_skin_apply_one_fast_raw(vptr, bind_vptr, joints_ptr, vcnt, skin_slab, mat_count)
}

fn _gltf_apply_part_skin_slab(dict part, any vptr, any bind_vptr, any joints_ptr, any weights_ptr, int vcnt, any skin_slab, int mat_count) bool {
   if part.get("skin_single_influence", false) {
      if !shr._gltf_skin_validate_enabled() { return _gltf_apply_skinning_one_fast_slab(vptr, bind_vptr, joints_ptr, vcnt, skin_slab, mat_count) }
      return _gltf_apply_skinning_one_slab(vptr, bind_vptr, joints_ptr, vcnt, skin_slab, mat_count)
   }
   _gltf_apply_skinning_slab(vptr, bind_vptr, joints_ptr, weights_ptr, vcnt, skin_slab, mat_count)
}

fn _gltf_part_runtime_skin_slab(dict part, any gltf_data, dict node_world_mats, int skin_idx, any mesh_bind_world) any {
   def runtime_skin = gltf_skin_info(gltf_data, skin_idx)
   def runtime_joints = is_dict(runtime_skin) ? runtime_skin.get("joints", []) : []
   def runtime_count = is_list(runtime_joints) ? runtime_joints.len : 0
   if runtime_count <= 0 { return 0 }
   def runtime_inv_bind = _gltf_skin_inv_bind_mats(gltf_data, skin_idx)
   if !is_list(runtime_inv_bind) || runtime_inv_bind.len < runtime_count { return 0 }
   def runtime_mesh_inv = _gltf_mesh_inv_cached(mesh_bind_world)
   mut runtime_slab = part.get("skin_runtime_slab", 0)
   mut runtime_slab_count = int(part.get("skin_runtime_slab_count", 0))
   if !runtime_slab || runtime_slab_count < runtime_count {
      if runtime_slab { free(runtime_slab) }
      runtime_slab = malloc(runtime_count * 64)
      if !runtime_slab { return 0 }
      runtime_slab_count = runtime_count
      part["skin_runtime_slab"] = runtime_slab
      part["skin_runtime_slab_count"] = runtime_slab_count
   }
   def runtime_world_list = node_world_mats.get("__world_list", 0)
   if is_list(runtime_world_list) {
      __gltf_skin_mats_store_raw(
         runtime_slab,
         runtime_joints,
         runtime_world_list,
         runtime_inv_bind,
         runtime_mesh_inv,
         runtime_count
      )
   } else {
      mut rji = 0
      while rji < runtime_count {
         def joint_idx = int(runtime_joints.get(rji, -1))
         def joint_world = node_world_mats.get(joint_idx, gltf_math.mat4_identity())
         def inv_bind = runtime_inv_bind.get(rji, gltf_math.mat4_identity())
         __gltf_skin_mat_store_raw(runtime_slab, rji, joint_world, inv_bind, runtime_mesh_inv)
         rji += 1
      }
   }
   [runtime_slab, runtime_count]
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
   def skin_raw_off = shr._gltf_skin_raw_off_enabled()
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
   if !is_dict(skin_mats_cache) {
      def runtime_rec = _gltf_part_runtime_skin_slab(part, gltf_data, node_world_mats, skin_idx, mesh_bind_world)
      if !is_list(runtime_rec) { return part }
      _gltf_apply_part_skin_slab(part, vptr, bind_vptr, joints_ptr, weights_ptr, vcnt, runtime_rec.get(0, 0), int(runtime_rec.get(1, 0)))
      return part
   }
   def skin_rec = _gltf_skin_mats_cache_record(gltf_data, skin_idx, node_world_mats, mesh_bind_world, skin_mats_cache)
   mut skin_mats = is_list(skin_rec) ? skin_rec.get(0, 0) : 0
   def skin_slab = is_list(skin_rec) ? skin_rec.get(1, 0) : 0
   def skin_mat_count = is_list(skin_rec) ? int(skin_rec.get(2, 0)) : 0
   if skin_slab && skin_mat_count > 0 && !skin_raw_off {
      _gltf_apply_part_skin_slab(part, vptr, bind_vptr, joints_ptr, weights_ptr, vcnt, skin_slab, skin_mat_count)
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
   mut out = []
   mut i = 0
   while i < target_count { out = out.append(0.0) i += 1 }
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
      if is_dict(target) && (weight > 0.000001 || weight < -0.000001) {
         def t_pos_res = ld._gltf_resolve_accessor_data(g, int(target.get("POSITION", -1)), data)
         def t_norm_res = ld._gltf_resolve_accessor_data(g, int(target.get("NORMAL", -1)), data)
         if is_dict(t_pos_res) || is_dict(t_norm_res) {
            morph_targets = morph_targets.append({"weight": weight, "pos_res": t_pos_res, "norm_res": t_norm_res})
         } else {
            ld._gltf_release_accessor_data(t_pos_res)
            ld._gltf_release_accessor_data(t_norm_res)
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
      mti += 1
   }
   true
}

fn _gltf_read_acc_f32(any data, dict acc_res, int elem_idx, int comp_idx) f64 {
   def ptr = acc_res.get("ptr", 0)
   if !ptr { return 0.0 }
   def stride = acc_res.get("stride", 4)
   def comp = acc_res.get("comp", shr.GLTF_COMP_FLOAT)
   def norm = acc_res.get("normalized", false)
   def cs = shr._gltf_comp_size(comp)
   def cols = int(acc_res.get("cols", 1))
   def rows = int(acc_res.get("rows", acc_res.get("type_count", 1)))
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
   mut out = []
   mut i = 0
   while i < n_comp {
      out = out.append(_gltf_read_acc_f32(data, acc_res, elem_idx, i))
      i += 1
   }
   out
}

fn _gltf_read_acc_scalar_tuple(any data, any acc_res, int tuple_idx, int n_comp) list {
   "Read n_comp adjacent scalar accessor elements starting at tuple_idx*n_comp.
   Needed for animation outputs like morph weights, which are commonly
   stored as SCALAR accessors with count = keyframes * weight_count."
   mut out = []
   def base_idx = tuple_idx * n_comp
   mut i = 0
   while i < n_comp {
      out = out.append(_gltf_read_acc_f32(data, acc_res, base_idx + i, 0))
      i += 1
   }
   out
}

fn _gltf_read_anim_tuple(any data, any output_res, int idx, int n_comp, bool packed_scalar_tuple) list {
   if packed_scalar_tuple { return _gltf_read_acc_scalar_tuple(data, output_res, idx, n_comp) }
   _gltf_read_acc_components(data, output_res, idx, n_comp)
}

fn _gltf_lerp_vec(list a, list b, f64 t, int n) list {
   mut out = []
   mut i = 0
   while i < n {
      def av, bv = 0.0 + a.get(i, 0.0), 0.0 + b.get(i, 0.0)
      out = out.append(av + (bv - av) * t)
      i += 1
   }
   out
}

fn _gltf_nlerp_quat(list a, list b, f64 t) list {
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
      def rx, ry = ax + (bx - ax) * t, ay + (by - ay) * t
      def rz, rw = az + (bz - az) * t, aw + (bw - aw) * t
      def len2 = rx * rx + ry * ry + rz * rz + rw * rw
      def inv_len = len2 > 0.000001 ? 1.0 / sqrt(len2) : 1.0
      return [rx * inv_len, ry * inv_len, rz * inv_len, rw * inv_len]
   }
   if dot > 1.0 { dot = 1.0 }
   def theta_0 = acos(dot)
   def sin_theta_0 = sin(theta_0)
   if abs(sin_theta_0) <= 0.000001 { return [ax, ay, az, aw] }
   def theta = theta_0 * t
   def sin_theta = sin(theta)
   def s0 = cos(theta) - dot * sin_theta / sin_theta_0
   def s1 = sin_theta / sin_theta_0
   [
      ax * s0 + bx * s1,
      ay * s0 + by * s1,
      az * s0 + bz * s1,
      aw * s0 + bw * s1
   ]
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
   mut qx, qy = x * shr._GLTF_INV_32767, y * _GLTF_INV_32767
   mut qz, qw = z * shr._GLTF_INV_32767, w * _GLTF_INV_32767
   if qx < -1.0 { qx = -1.0 }
   if qy < -1.0 { qy = -1.0 }
   if qz < -1.0 { qz = -1.0 }
   if qw < -1.0 { qw = -1.0 }
   [qx, qy, qz, qw]
}

fn _gltf_find_time_bracket(any data, any input_res, f64 time_sec) list {
   def count = input_res.get("count", 0)
   if count <= 0 { return [0, 0, 0.0] }
   if count == 1 { return [0, 0, 0.0] }
   def t_first = _gltf_read_acc_f32(data, input_res, 0, 0)
   def t_last  = _gltf_read_acc_f32(data, input_res, count - 1, 0)
   if time_sec <= t_first { return [0, 0, 0.0] }
   if time_sec >= t_last { return [count-1, count-1, 0.0] }
   mut lo = 0
   mut hi = count - 1
   while hi - lo > 1 {
      def mid = (lo + hi) / 2
      def t_mid = _gltf_read_acc_f32(data, input_res, mid, 0)
      if t_mid <= time_sec { lo = mid }
      else { hi = mid }
   }
   def t_lo = _gltf_read_acc_f32(data, input_res, lo, 0)
   def t_hi = _gltf_read_acc_f32(data, input_res, hi, 0)
   def dt = t_hi - t_lo
   def alpha = dt > 0.00001 ? (time_sec - t_lo) / dt : 0.0
   [lo, hi, alpha]
}

fn _gltf_sample_channel(any data, dict sampler, dict input_res, dict output_res, f64 time_sec, int n_comp, bool is_rotation, bool packed_scalar_tuple=false) list {
   def interp = to_str(sampler.get("interpolation", "LINEAR"))
   def bracket = _gltf_find_time_bracket(data, input_res, time_sec)
   def lo = int(bracket.get(0, 0))
   def hi = int(bracket.get(1, 0))
   def t_lo = _gltf_read_acc_f32(data, input_res, lo, 0)
   def t_hi = _gltf_read_acc_f32(data, input_res, hi, 0)
   def dt = t_hi - t_lo
   mut t = 0.0
   if dt > 0.00001 { t = (time_sec - t_lo) / dt }
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
      mut out = []
      mut i = 0
      while i < n_comp {
         out = out.append(h00 * (0.0 + vk.get(i, 0.0)) +
            h10 * td * (0.0 + bk.get(i, 0.0)) +
            h01 * (0.0 + vk1.get(i, 0.0)) +
         h11 * td * (0.0 + ak1.get(i, 0.0)))
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

fn _gltf_anim_fast_records(any gltf_data, int anim_idx) any {
   shr._gltf_ensure_caches()
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return 0 }
   def data = ld._gltf_primary_data_ptr(gltf_data)
   if !data { return 0 }
   def key = shr._gltf_cache_key_from_data(gltf_data) + ":data:" + to_str(to_int(data)) + ":anim:" + to_str(int(anim_idx))
   if _gltf_anim_sample_cache.contains(key) { return _gltf_anim_sample_cache.get(key, 0) }
   def anims = g.get("animations", 0)
   if !is_list(anims) || anim_idx < 0 || anim_idx >= anims.len { return 0 }
   def anim = anims.get(anim_idx)
   if !is_dict(anim) { return 0 }
   def channels = anim.get("channels", [])
   def samplers = anim.get("samplers", [])
   if !is_list(channels) || !is_list(samplers) { return 0 }
   def channels_n = channels.len
   def samplers_n = samplers.len
   mut records = list(0)
   mut ci = 0
   while ci < channels_n {
      def ch = channels[ci]
      def tgt = is_dict(ch) ? ch.get("target", 0) : 0
      def samp_idx = is_dict(ch) ? int(ch.get("sampler", -1)) : -1
      if !is_dict(tgt) || samp_idx < 0 || samp_idx >= samplers_n { return 0 }
      if is_dict(tgt.get("extensions", 0)) { return 0 }
      def node_idx = int(tgt.get("node", -1))
      def path = to_str(tgt.get("path", ""))
      mut path_code = 0
      mut n_comp = 0
      if path == "translation" { path_code = 1 n_comp = 3 }
      elif path == "rotation" { path_code = 2 n_comp = 4 }
      elif path == "scale" { path_code = 3 n_comp = 3 }
      else { return 0 }
      if node_idx < 0 { return 0 }
      def samp = samplers[samp_idx]
      if !is_dict(samp) { return 0 }
      if to_str(samp.get("interpolation", "LINEAR")) != "LINEAR" { return 0 }
      def input_res = ld._gltf_resolve_accessor_data(g, int(samp.get("input", -1)), data)
      def output_res = ld._gltf_resolve_accessor_data(g, int(samp.get("output", -1)), data)
      if !is_dict(input_res) || !is_dict(output_res) { return 0 }
      if input_res.get("owned", false) || output_res.get("owned", false) { return 0 }
      if int(input_res.get("comp", 0)) != GLTF_COMP_FLOAT || int(output_res.get("comp", 0)) != GLTF_COMP_FLOAT { return 0 }
      if int(input_res.get("type_count", 1)) != 1 || int(output_res.get("type_count", 0)) != n_comp { return 0 }
      def in_ptr = input_res.get("ptr", 0)
      def out_ptr = output_res.get("ptr", 0)
      def count = int(input_res.get("count", 0))
      if !in_ptr || !out_ptr || count <= 0 || int(output_res.get("count", 0)) < count { return 0 }
      records = records.append([node_idx, path_code, in_ptr, out_ptr, count, int(input_res.get("stride", 4)), int(output_res.get("stride", n_comp * 4)), n_comp, 0])
      ci += 1
   }
   if records.len != channels_n { return 0 }
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
   def dt = t_hi - t_lo
   def alpha = dt > 0.00001 ? (time_sec - t_lo) / dt : 0.0
   [lo, lo + 1, alpha]
}

fn _gltf_anim_clean_tiny(f64 v) f64 {
   abs(v) < 0.000000000001 ? 0.0 : v
}

fn _gltf_anim_fast_value(any rec, f64 time_sec) list {
   def br = _gltf_anim_record_bracket(rec, time_sec)
   def lo = int(br.get(0, 0))
   def hi = int(br.get(1, 0))
   def t = float(br.get(2, 0.0))
   def out_ptr = rec.get(3, 0)
   def stride = int(rec.get(6, 0))
   def n_comp = int(rec.get(7, 0))
   def lo_off = lo * stride
   def hi_off = hi * stride
   if n_comp == 4 {
      def ax, ay = f32le(out_ptr, lo_off + 0), f32le(out_ptr, lo_off + 4)
      def az, aw = f32le(out_ptr, lo_off + 8), f32le(out_ptr, lo_off + 12)
      mut bx, by = f32le(out_ptr, hi_off + 0), f32le(out_ptr, hi_off + 4)
      mut bz, bw = f32le(out_ptr, hi_off + 8), f32le(out_ptr, hi_off + 12)
      mut dot = ax * bx + ay * by + az * bz + aw * bw
      if dot < 0.0 { bx = -bx by = -by bz = -bz bw = -bw dot = 0.0 - dot }
      if dot > 0.9995 {
         def rx, ry = ax + (bx - ax) * t, ay + (by - ay) * t
         def rz, rw = az + (bz - az) * t, aw + (bw - aw) * t
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
      return [
         _gltf_anim_clean_tiny(ax * s0 + bx * s1),
         _gltf_anim_clean_tiny(ay * s0 + by * s1),
         _gltf_anim_clean_tiny(az * s0 + bz * s1),
         _gltf_anim_clean_tiny(aw * s0 + bw * s1)
      ]
   }
   def ax, ay = f32le(out_ptr, lo_off + 0), f32le(out_ptr, lo_off + 4)
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
   if !common.env_toggle("NY_GLTF_ANIM_FAST", true) { return 0 }
   def g = gltf_data.get("gltf", 0)
   def skins = is_dict(g) ? g.get("skins", 0) : 0
   if is_list(skins) && skins.len > 0 && !common.env_toggle("NY_GLTF_ANIM_FAST_SKIN", true) { return 0 }
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
   mut fast_node_overrides = []
   mut ni = 0
   while ni < nodes_n {
      fast_node_overrides = fast_node_overrides.append(0)
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
      if kind == "uvRotation" { return 1 }
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
      ptrs = ptrs.append({"material": int(ptr_mat_target.get("material", -1)), "kind": to_str(ptr_mat_target.get("kind", "")), "value": val})
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

fn gltf_sample_animation(any gltf_data, int anim_idx, f64 time_sec) any {
   "Sample animation at time_sec. Returns {node_idx: {T:[x,y,z], R:[x,y,z,w], S:[x,y,z]}} overrides."
   def fast = _gltf_sample_animation_fast(gltf_data, anim_idx, time_sec)
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
               def key_mult = eq(to_str(samp.get("interpolation", "LINEAR")), "CUBICSPLINE") ? 3 : 1
               def n_comp = _gltf_anim_sample_component_count(path, is_visibility_pointer, ptr_mat_target, input_res, output_res, key_mult)
               def is_rot = eq(path, "rotation")
               def packed_scalar_tuple =
               eq(path, "weights") &&
               int(output_res.get("type_count", 1)) == 1 &&
               n_comp > 1
               def val = _gltf_sample_channel(data, samp, input_res, output_res, time_sec, n_comp, is_rot, packed_scalar_tuple)
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
      def clip = gltf_sample_animation(gltf_data, ai, time_sec)
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
   "Applies sampled node weights overrides onto referenced mesh.weights in gltf_data.
   Returns [updated_gltf_data, changed]."
   if !is_dict(gltf_data) || !is_dict(overrides) { return [gltf_data, false] }
   mut g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return [gltf_data, false] }
   def nodes = g.get("nodes", 0)
   mut meshes = g.get("meshes", 0)
   if !is_list(nodes) || !is_list(meshes) { return [gltf_data, false] }
   def nodes_n = nodes.len
   def meshes_n = meshes.len
   mut changed = false
   mut ni = 0
   while ni < nodes_n {
      def node = nodes[ni]
      if is_dict(node) {
         def node_ov = overrides.get(ni, 0)
         if is_dict(node_ov) {
            def w = node_ov.get("W", 0)
            if is_list(w) {
               def mesh_idx = int(node.get("mesh", -1))
               if mesh_idx >= 0 && mesh_idx < meshes_n {
                  mut mesh = meshes[mesh_idx]
                  if is_dict(mesh) {
                     def w_n = w.len
                     mut out_w = list(w_n)
                     mut wi = 0
                     while wi < w_n {
                        __store_item_fast(out_w, wi, float(w[wi]))
                        wi += 1
                     }
                     __list_set_len(out_w, w_n)
                     mesh["weights"] = out_w
                     meshes[mesh_idx] = mesh
                     changed = true
                  }
               }
            }
         }
      }
      ni += 1
   }
   if !changed { return [gltf_data, false] }
   g["meshes"] = meshes
   gltf_data["gltf"] = g
   [gltf_data, true]
}

fn _gltf_build_node_world_mats_animated(dict g, int node_idx, list parent_m, dict node_world_mats, dict overrides) dict {
   def nodes = g.get("nodes")
   if !is_list(nodes) || node_idx < 0 || node_idx >= nodes.len { return node_world_mats }
   def visit_key = shr._gltf_node_visit_key(node_idx)
   if node_world_mats.get(visit_key, false) { return node_world_mats }
   if node_world_mats.contains(node_idx) { return node_world_mats }
   def node = nodes[node_idx]
   if !is_dict(node) { return node_world_mats }
   node_world_mats[visit_key] = true
   def anim_ov = shr._gltf_anim_override_for_node(overrides, node_idx)
   mut local_m = 0
   if is_dict(anim_ov) {
      def nodes_g = g.get("nodes")
      def orig_node = is_list(nodes_g) ? nodes_g[node_idx] : 0
      def t = anim_ov.get("T", is_dict(orig_node) ? orig_node.get("translation", [0.0,0.0,0.0]) : [0.0,0.0,0.0])
      def r_raw = anim_ov.get("R", is_dict(orig_node) ? orig_node.get("rotation", [0.0,0.0,0.0,1.0]) : [0.0,0.0,0.0,1.0])
      def r = _gltf_normalize_quat(r_raw)
      def s = anim_ov.get("S", is_dict(orig_node) ? orig_node.get("scale",       [1.0,1.0,1.0]) : [1.0,1.0,1.0])
      local_m = gltf_math.mat4_from_trs(t, r, s)
   } else {
      local_m = gltf_math.node_local_matrix(node)
   }
   def world_m = gltf_math.mat4_mul(parent_m, local_m)
   node_world_mats[node_idx] = world_m
   def children = node.get("children")
   if is_list(children) {
      def children_n = children.len
      mut i = 0
      while i < children_n {
         def child_idx = int(children[i])
         if child_idx >= 0 && child_idx != node_idx { node_world_mats = _gltf_build_node_world_mats_animated(g, child_idx, world_m, node_world_mats, overrides) }
         i += 1
      }
   }
   node_world_mats = node_world_mats.delete(visit_key)
   node_world_mats
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
   def fast_numeric = overrides.get("__fast_numeric", false) ? true : false
   def base_local_mats = fast_numeric ? shr._gltf_node_local_mats(g) : 0
   mut world_list = 0
   mut fast_node_overrides = 0
   if fast_numeric {
      world_list = []
      mut wi = 0
      while wi < nodes_n {
         world_list = world_list.append(0)
         wi += 1
      }
      fast_node_overrides = overrides.get("__fast_node_overrides", 0)
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
            if fast_numeric {
               node_world_mats = _gltf_build_node_world_mats_animated_fast(nodes,
                  base_local_mats,
                  world_list,
                  fast_node_overrides,
                  root_idx,
                  id,
                  node_world_mats,
               overrides)
            } else {
               node_world_mats = _gltf_build_node_world_mats_animated(g, root_idx, id, node_world_mats, overrides)
            }
            ri += 1
         }
      }
   }
   if fast_numeric && is_list(world_list) { node_world_mats["__world_list"] = world_list }
   node_world_mats
}
