;; Keywords: 3d gltf glb parse
;; Submodule: scene
module std.math.parse.3d.gltf.scene(_gltf_build_node_world_mats, _gltf_root_relevant_for_mesh_limit, gltf_scene_punctual_lights, _gltf_compute_mesh_instance_mats_fast, _gltf_node_default_visibility, _gltf_node_local_visibility, _gltf_build_node_visibility_map, gltf_has_node_visibility, gltf_resolve_node_visibility, _gltf_resolve_part_skin, gltf_camera_count, gltf_camera_info, gltf_camera_instances, _gltf_model_has_negative_det, _gltf_model_mean_scale, _gltf_scale_bsdf2_thickness, _gltf_resolve_instancing_mats)
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
use std.math.parse.3d.gltf.animation as anim

fn _gltf_build_node_world_mats(any g, int node_idx, list parent_m, dict node_world_mats) dict {
   def nodes = g.get("nodes")
   if !is_list(nodes) || node_idx < 0 || node_idx >= nodes.len { return node_world_mats }
   def visit_key = shr._gltf_node_visit_key(node_idx)
   if node_world_mats.get(visit_key, false) { return node_world_mats }
   if node_world_mats.contains(node_idx) { return node_world_mats }
   def node = nodes[node_idx]
   if !is_dict(node) { return node_world_mats }
   node_world_mats[visit_key] = true
   def local_m = gltf_math.node_local_matrix(node)
   def world_m = gltf_math.mat4_mul(parent_m, local_m)
   node_world_mats[node_idx] = world_m
   def children = node.get("children")
   if is_list(children) {
      def children_n = children.len
      mut i = 0
      while i < children_n {
         def child_idx = int(children[i])
         if child_idx >= 0 && child_idx != node_idx { node_world_mats = _gltf_build_node_world_mats(g, child_idx, world_m, node_world_mats) }
         i += 1
      }
   }
   node_world_mats = node_world_mats.delete(visit_key)
   node_world_mats
}

fn _gltf_root_relevant_for_mesh_limit(any g, int node_idx, int mesh_limit) bool {
   if mesh_limit <= 0 { return true }
   def nodes = g.get("nodes", 0)
   if !is_list(nodes) || node_idx < 0 || node_idx >= nodes.len { return false }
   def node = nodes.get(node_idx, 0)
   if !is_dict(node) { return false }
   def mesh_ref = int(node.get("mesh", -1))
   if mesh_ref >= 0 && mesh_ref < mesh_limit { return true }
   def children = node.get("children", [])
   if is_list(children) && children.len > 0 { return true }
   if mesh_ref >= 0 { return false }
   true
}

fn gltf_scene_punctual_lights(any gltf_data, any overrides=0) list {
   "Returns a list of world-space KHR_lights_punctual lights for the active scene.
   When `overrides` are provided, light transforms and visibility follow animated TRS/visibility."
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return [] }
   def exts = g.get("extensions", 0)
   def khr_lights = is_dict(exts) ? exts.get("KHR_lights_punctual", 0) : 0
   def lights = is_dict(khr_lights) ? khr_lights.get("lights", 0) : 0
   def nodes = g.get("nodes", 0)
   if !is_list(lights) || !is_list(nodes) { return [] }
   def nodes_n = nodes.len
   def lights_n = lights.len
   mut node_world_mats = dict(max(16, nodes_n * 2))
   def vis_map = gltf_resolve_node_visibility(gltf_data, overrides)
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
            if is_dict(overrides) {
               node_world_mats = anim._gltf_build_node_world_mats_animated(
                  g,
                  int(roots[ri]),
                  id,
                  node_world_mats,
                  overrides
               )
            } else {
               node_world_mats = _gltf_build_node_world_mats(g, int(roots[ri]), id, node_world_mats)
            }
            ri += 1
         }
      }
   }
   mut out = []
   mut i = 0
   while i < nodes_n {
      def node = nodes[i]
      if !vis_map.get(i, true) { i += 1 continue }
      def node_ext = is_dict(node) ? node.get("extensions", 0) : 0
      def lref = is_dict(node_ext) ? node_ext.get("KHR_lights_punctual", 0) : 0
      def light_idx = is_dict(lref) ? int(lref.get("light", -1)) : -1
      if light_idx >= 0 && light_idx < lights_n {
         def light = lights.get(light_idx, 0)
         def world_m = node_world_mats.get(i, gltf_math.mat4_identity())
         def color = light.get("color", [1.0, 1.0, 1.0])
         def intensity = float(light.get("intensity", 1.0))
         def range = float(light.get("range", 0.0))
         def ltype = to_str(light.get("type", "point"))
         def spot = light.get("spot", 0)
         def outer_angle = is_dict(spot) ? float(spot.get("outerConeAngle", 0.785398)) : 0.785398
         def rec = {
            "type": ltype, "position": gltf_math.mat4_transform_point(world_m, [0.0, 0.0, 0.0]),
            "direction": gltf_math.mat4_transform_dir(world_m, [0.0, 0.0, -1.0]),
            "color": [float(color.get(0,1.0)), float(color.get(1,1.0)), float(color.get(2,1.0))],
            "intensity": intensity, "range": range, "outer_cone_cos": cos(outer_angle), "node_idx": i
         }
         out = out.append(rec)
      }
      i += 1
   }
   out
}

fn _gltf_compute_mesh_instance_mats_fast(any g, any node_world_mats, int mesh_limit=0) dict {
   def nodes = g.get("nodes")
   if !is_list(nodes) { return dict(0) }
   def nodes_n = nodes.len
   def scene_scoped = is_dict(node_world_mats) && node_world_mats.len > 0
   mut mesh_map = dict(max(16, nodes_n * 2))
   mut node_i = 0
   while node_i < nodes_n {
      def node = nodes.get(node_i, 0)
      if !is_dict(node) {
         node_i += 1
         continue
      }
      def mesh_ref = int(node.get("mesh", -1))
      if mesh_limit > 0 && mesh_ref >= mesh_limit {
         node_i += 1
         continue
      }
      if mesh_ref >= 0 {
         mut wm = node_world_mats.get(node_i, 0)
         if scene_scoped && (!is_list(wm) || wm.len < 16) {
            node_i += 1
            continue
         }
         if !is_list(wm) || wm.len < 16 { wm = gltf_math.mat4_identity() }
         def node_ext = node.get("extensions", 0)
         def gpu_ins = is_dict(node_ext) ? node_ext.get("EXT_mesh_gpu_instancing", 0) : 0
         if is_dict(gpu_ins) {
            def attrs = gpu_ins.get("attributes", 0)
            if is_dict(attrs) {
               def existing = mesh_map.get(mesh_ref, 0)
               def entry = [wm, node_i]
               if is_list(existing) { mesh_map[mesh_ref] = existing.append(entry) }
               else { mesh_map[mesh_ref] = [entry] }
            }
         } else {
            def existing = mesh_map.get(mesh_ref, 0)
            def entry = [wm, node_i]
            if is_list(existing) { mesh_map[mesh_ref] = existing.append(entry) }
            else { mesh_map[mesh_ref] = [entry] }
         }
      }
      node_i += 1
   }
   mesh_map
}

fn _gltf_node_default_visibility(any g, int node_idx) bool {
   def nodes = g.get("nodes", 0)
   if !is_list(nodes) || node_idx < 0 || node_idx >= nodes.len { return true }
   def node = nodes[node_idx]
   if !is_dict(node) { return true }
   def exts = node.get("extensions", 0)
   def nv = is_dict(exts) ? exts.get("KHR_node_visibility", 0) : 0
   if is_dict(nv) && nv.contains("visible") {
      def v = nv.get("visible", true)
      if is_int(v) { return v != 0 }
      if is_float(v) { return float(v) >= 0.5 }
      return !!v
   }
   true
}

fn _gltf_node_local_visibility(any g, int node_idx, any overrides=0) bool {
   mut vis = _gltf_node_default_visibility(g, node_idx)
   def ov = shr._gltf_anim_override_for_node(overrides, node_idx)
   if is_dict(ov) && ov.contains("VIS") {
      def vv = ov.get("VIS", true)
      if is_int(vv) { vis = vv != 0 }
      elif is_float(vv) { vis = float(vv) >= 0.5 }
      else { vis = !!vv }
   }
   vis
}

fn _gltf_build_node_visibility_map(any g, int node_idx, bool parent_visible, dict vis_map, any overrides=0) dict {
   def nodes = g.get("nodes", 0)
   if !is_list(nodes) || node_idx < 0 || node_idx >= nodes.len { return vis_map }
   def local_vis = _gltf_node_local_visibility(g, node_idx, overrides)
   def eff_vis = parent_visible && local_vis
   vis_map[node_idx] = eff_vis
   def node = nodes[node_idx]
   def children = is_dict(node) ? node.get("children", 0) : 0
   if is_list(children) {
      def children_n = children.len
      mut ci = 0
      while ci < children_n {
         vis_map = _gltf_build_node_visibility_map(g, int(children[ci]), eff_vis, vis_map, overrides)
         ci += 1
      }
   }
   vis_map
}

fn gltf_has_node_visibility(any gltf_data, any overrides=0) bool {
   "Returns true when visibility defaults or animation overrides can affect nodes."
   shr._gltf_ensure_caches()
   if is_dict(overrides) {
      def ov_nodes = overrides.get("__nodes", 0)
      if is_list(ov_nodes) {
         mut oi = 0
         def ov_n = ov_nodes.len
         while oi < ov_n {
            def rec = ov_nodes[oi]
            if is_dict(rec) && rec.contains("VIS") { return true }
            oi += 1
         }
      }
   }
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return false }
   def key = shr._gltf_cache_key_from_data(gltf_data) + ":visibility"
   if _gltf_visibility_flag_cache.contains(key) { return _gltf_visibility_flag_cache.get(key, false) ? true : false }
   def nodes = g.get("nodes", 0)
   def nodes_n = is_list(nodes) ? nodes.len : 0
   mut ni = 0
   while ni < nodes_n {
      def node = nodes[ni]
      def exts = is_dict(node) ? node.get("extensions", 0) : 0
      if is_dict(exts) && is_dict(exts.get("KHR_node_visibility", 0)) {
         _gltf_visibility_flag_cache = cache.cache_put_reset(shr._gltf_visibility_flag_cache,
            key,
            true,
            shr._GLTF_CACHE_LIMIT_MED,
         64)
         return true
      }
      ni += 1
   }
   _gltf_visibility_flag_cache = cache.cache_put_reset(shr._gltf_visibility_flag_cache,
      key,
      false,
      shr._GLTF_CACHE_LIMIT_MED,
   64)
   false
}

fn gltf_resolve_node_visibility(any gltf_data, any overrides=0) dict {
   "Returns {node_idx: bool} effective visibility map for active scene roots."
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return dict(0) }
   def nodes = g.get("nodes", 0)
   mut vis_map = dict(is_list(nodes) ? max(16, nodes.len * 2) : 16)
   def scenes = g.get("scenes", 0)
   def scene_idx = shr._gltf_active_scene_idx(g, gltf_data)
   if is_list(scenes) && scene_idx >= 0 && scene_idx < scenes.len {
      def scene = scenes[scene_idx]
      def roots = is_dict(scene) ? scene.get("nodes", 0) : 0
      if is_list(roots) {
         def roots_n = roots.len
         mut ri = 0
         while ri < roots_n {
            vis_map = _gltf_build_node_visibility_map(g, int(roots[ri]), true, vis_map, overrides)
            ri += 1
         }
      }
   }
   vis_map
}

fn _gltf_resolve_part_skin(dict gltf_data, dict g, any data, list nodes, int part_node_idx, dict meta, any vptr, int vcnt) dict {
   if part_node_idx < 0 || part_node_idx >= nodes.len {
      return {"skin_idx": -1,
         "skin_bind_vptr": 0,
         "skin_joints_ptr": 0,
         "skin_weights_ptr": 0,
         "skin_vcnt": vcnt,
         "skin_joints": [],
      "skin_inv_bind_accessor": -1}
   }
   def node_obj = nodes.get(part_node_idx, 0)
   def skin_idx = is_dict(node_obj) ? int(node_obj.get("skin", -1)) : -1
   def out = {"skin_idx": skin_idx, "skin_bind_vptr": 0, "skin_joints_ptr": 0, "skin_weights_ptr": 0, "skin_vcnt": vcnt, "skin_joints": [], "skin_inv_bind_accessor": -1}
   if !(skin_idx >= 0 && meta.get("is_skinned_candidate", false) && vptr && vcnt > 0) { return out }
   def skin_bind_vptr = malloc(vcnt * shr._GLTF_VTX_STRIDE)
   if !skin_bind_vptr { return out }
   memcpy(skin_bind_vptr, vptr, vcnt * shr._GLTF_VTX_STRIDE)
   def joints_acc_idx = int(meta.get("joints_acc_idx", -1))
   def weights_acc_idx = int(meta.get("weights_acc_idx", -1))
   def skin_side = anim._gltf_pack_skin_sidecars(g, data, joints_acc_idx, weights_acc_idx, vcnt)
   if !is_dict(skin_side) {
      free(skin_bind_vptr)
      return out
   }
   def skin_info = anim.gltf_skin_info(gltf_data, skin_idx)
   def skin_joints = is_dict(skin_info) ? skin_info.get("joints", []) : []
   return {
      "skin_idx": skin_idx,
      "skin_bind_vptr": skin_bind_vptr,
      "skin_joints_ptr": skin_side.get("joints_ptr", 0),
      "skin_weights_ptr": skin_side.get("weights_ptr", 0),
      "skin_vcnt": int(skin_side.get("count", vcnt)),
      "skin_joints": skin_joints,
      "skin_inv_bind_accessor": is_dict(skin_info) ? int(skin_info.get("inverse_bind_accessor", -1)) : -1,
      "skin_single_influence": bool(skin_side.get("single_influence", false))
   }
}

fn gltf_camera_count(any gltf_data) int {
   "Runs the camera count operation."
   def g = gltf_data.get("gltf", 0)
   def cams = is_dict(g) ? g.get("cameras", []) : []
   is_list(cams) ? cams.len : 0
}

fn gltf_camera_info(any gltf_data, int cam_idx) any {
   "Runs the camera info operation."
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return 0 }
   def cams = g.get("cameras", [])
   if !is_list(cams) || cam_idx < 0 || cam_idx >= cams.len { return 0 }
   def cam = cams[cam_idx]
   if !is_dict(cam) { return 0 }
   def typ = to_str(cam.get("type", ""))
   if eq(typ, "perspective") {
      def p = cam.get("perspective", 0)
      def aspect_ratio = is_dict(p) ? p.get("aspectRatio", 0) : 0
      def yfov = is_dict(p) ? float(p.get("yfov", 0.0)) : 0.0
      def znear = is_dict(p) ? float(p.get("znear", 0.0)) : 0.0
      def zfar = is_dict(p) ? p.get("zfar", 0) : 0
      return {"index": cam_idx,
         "name": to_str(cam.get("name",
         "")),
         "type": typ,
         "aspect_ratio": aspect_ratio,
         "yfov": yfov,
         "znear": znear,
      "zfar": zfar}
   }
   if eq(typ, "orthographic") {
      def o = cam.get("orthographic", 0)
      def xmag = is_dict(o) ? float(o.get("xmag", 0.0)) : 0.0
      def ymag = is_dict(o) ? float(o.get("ymag", 0.0)) : 0.0
      def znear = is_dict(o) ? float(o.get("znear", 0.0)) : 0.0
      def zfar = is_dict(o) ? float(o.get("zfar", 0.0)) : 0.0
      return {"index": cam_idx,
         "name": to_str(cam.get("name",
         "")),
         "type": typ,
         "xmag": xmag,
         "ymag": ymag,
         "znear": znear,
      "zfar": zfar}
   }
   {"index": cam_idx, "name": to_str(cam.get("name", "")), "type": typ}
}

fn gltf_camera_instances(any gltf_data) list {
   "Runs the camera instances operation."
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return [] }
   def nodes = g.get("nodes", [])
   if !is_list(nodes) { return [] }
   def nodes_n = nodes.len
   mut node_world_mats = dict(max(16, nodes_n * 2))
   def scenes = g.get("scenes", [])
   def scene_idx = shr._gltf_active_scene_idx(g, gltf_data)
   if is_list(scenes) && scene_idx >= 0 && scene_idx < scenes.len {
      def scene = scenes[scene_idx]
      def roots = is_dict(scene) ? scene.get("nodes", []) : []
      def roots_n = is_list(roots) ? roots.len : 0
      mut ri = 0
      while ri < roots_n {
         node_world_mats = _gltf_build_node_world_mats(g, int(roots[ri]), gltf_math.mat4_identity(), node_world_mats)
         ri += 1
      }
   }
   mut out = []
   mut i = 0
   while i < nodes_n {
      def node = nodes[i]
      def cam_idx = is_dict(node) ? int(node.get("camera", -1)) : -1
      if cam_idx >= 0 { out = out.append({"node_idx": i, "camera_idx": cam_idx, "camera": gltf_camera_info(gltf_data, cam_idx), "world_matrix": node_world_mats.get(i, gltf_math.mat4_identity())}) }
      i += 1
   }
   out
}

fn _gltf_model_has_negative_det(any m) bool {
   if !is_list(m) || m.len < 16 { return false }
   def mm = shr._gltf_mat3_num(m)
   def m00, m01 = mm.get(0), mm.get(1)
   def m02 = mm.get(2)
   def m10, m11 = mm.get(3), mm.get(4)
   def m12 = mm.get(5)
   def m20, m21 = mm.get(6), mm.get(7)
   def m22 = mm.get(8)
   def det = m00 * (m11 * m22 - m12 * m21) -
   m01 * (m10 * m22 - m12 * m20) +
   m02 * (m10 * m21 - m11 * m20)
   det < 0.0
}

fn _gltf_model_mean_scale(any m) f64 {
   if !is_list(m) || m.len < 16 { return 1.0 }
   def mm = shr._gltf_mat3_num(m)
   def m00, m01 = mm.get(0), mm.get(1)
   def m02 = mm.get(2)
   def m10, m11 = mm.get(3), mm.get(4)
   def m12 = mm.get(5)
   def m20, m21 = mm.get(6), mm.get(7)
   def m22 = mm.get(8)
   def sx, sy = sqrt(m00 * m00 + m10 * m10 + m20 * m20), sqrt(m01 * m01 + m11 * m11 + m21 * m21)
   def sz = sqrt(m02 * m02 + m12 * m12 + m22 * m22)
   mut s = (sx + sy + sz) / 3.0
   if shr._gltf_float_bad(s) || s <= 0.0 { return 1.0 }
   if s < 0.001 { s = 0.001 } elif s > 64.0 { s = 64.0 }
   s
}

fn _gltf_scale_bsdf2_thickness(any bsdf2_u32, any volume_scale) int {
   def word = int(bsdf2_u32)
   def thickness_u8 = band(bshr(word, 24), 255)
   if thickness_u8 <= 0 { return word }
   mut scale = float(volume_scale)
   if shr._gltf_float_bad(scale) || scale <= 0.0 { return word }
   if scale > 0.997 && scale < 1.003 { return word }
   if scale < 0.001 { scale = 0.001 } elif scale > 64.0 { scale = 64.0 }
   mut scaled_u8 = int(float(thickness_u8) * scale + 0.5)
   if scaled_u8 <= 0 { scaled_u8 = 1 } elif scaled_u8 > 255 { scaled_u8 = 255 }
   bor(band(word, 0x00ffffff), bshl(scaled_u8, 24))
}

fn _gltf_resolve_instancing_mats(any g, any node, any data, any local_to_world) any {
   if !is_dict(node) { return 0 }
   if !node.contains("extensions") { return 0 }
   def ext = node.get("extensions", 0)
   if !is_dict(ext) { return 0 }
   def gpu_ins = ext.get("EXT_mesh_gpu_instancing", 0)
   if !is_dict(gpu_ins) { return 0 }
   def attrs = gpu_ins.get("attributes", 0)
   if !is_dict(attrs) { return 0 }
   def t_acc_idx, r_acc_idx = attrs.get("TRANSLATION", -1), attrs.get("ROTATION", -1)
   def s_acc_idx = attrs.get("SCALE", -1)
   def t_res = ld._gltf_resolve_accessor_data(g, t_acc_idx, data)
   def r_res = ld._gltf_resolve_accessor_data(g, r_acc_idx, data)
   def s_res = ld._gltf_resolve_accessor_data(g, s_acc_idx, data)
   mut count = 1000000
   mut has_any = false
   if t_res { count = min(count, int(t_res.get("count", 0))) has_any = true }
   if r_res { count = min(count, int(r_res.get("count", 0))) has_any = true }
   if s_res { count = min(count, int(s_res.get("count", 0))) has_any = true }
   if !has_any || count > 1000000 {
      ld._gltf_release_accessor_data(t_res)
      ld._gltf_release_accessor_data(r_res)
      ld._gltf_release_accessor_data(s_res)
      return 0
   }
   if count <= 0 {
      ld._gltf_release_accessor_data(t_res)
      ld._gltf_release_accessor_data(r_res)
      ld._gltf_release_accessor_data(s_res)
      return 0
   }
   mut mats = []
   mut i = 0
   while i < count {
      mut trans = [0.0, 0.0, 0.0]
      if t_res {
         def ptr = t_res.get("ptr", 0)
         def stride = t_res.get("stride", 0)
         def comp = t_res.get("comp", shr.GLTF_COMP_FLOAT)
         def norm = t_res.get("normalized", false)
         def cs = shr._gltf_comp_size(comp)
         def off = i * stride
         trans = [
            float(shr._gltf_read_f32_acc(ptr, off + cs * 0, comp, norm)),
            float(shr._gltf_read_f32_acc(ptr, off + cs * 1, comp, norm)),
            float(shr._gltf_read_f32_acc(ptr, off + cs * 2, comp, norm))
         ]
      }
      mut rot = [0.0, 0.0, 0.0, 1.0]
      if r_res {
         def ptr = r_res.get("ptr", 0)
         def stride = r_res.get("stride", 0)
         def comp = r_res.get("comp", shr.GLTF_COMP_FLOAT)
         def norm = r_res.get("normalized", false)
         def cs = shr._gltf_comp_size(comp)
         def off = i * stride
         rot = [
            float(shr._gltf_read_f32_acc(ptr, off + cs * 0, comp, norm)),
            float(shr._gltf_read_f32_acc(ptr, off + cs * 1, comp, norm)),
            float(shr._gltf_read_f32_acc(ptr, off + cs * 2, comp, norm)),
            float(shr._gltf_read_f32_acc(ptr, off + cs * 3, comp, norm))
         ]
      }
      mut scale = [1.0, 1.0, 1.0]
      if s_res {
         def ptr = s_res.get("ptr", 0)
         def stride = s_res.get("stride", 0)
         def comp = s_res.get("comp", shr.GLTF_COMP_FLOAT)
         def norm = s_res.get("normalized", false)
         def cs = shr._gltf_comp_size(comp)
         def off = i * stride
         scale = [
            float(shr._gltf_read_f32_acc(ptr, off + cs * 0, comp, norm)),
            float(shr._gltf_read_f32_acc(ptr, off + cs * 1, comp, norm)),
            float(shr._gltf_read_f32_acc(ptr, off + cs * 2, comp, norm))
         ]
      }
      def inst_local = gltf_math.mat4_from_trs(trans, rot, scale)
      mats = mats.append(gltf_math.mat4_mul(local_to_world, inst_local))
      i += 1
   }
   ld._gltf_release_accessor_data(t_res)
   ld._gltf_release_accessor_data(r_res)
   ld._gltf_release_accessor_data(s_res)
   mats
}
