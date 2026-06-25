;; Keywords: 3d gltf glb parse
;; Submodule: mesh
module std.math.parse.3d.gltf.mesh(_gltf_make_indexed_part_from_state, gltf_mesh_count, gltf_get_mesh, _gltf_accessor_local_bounds, _gltf_prim_meta, _gltf_collect_indexed_semantics, _gltf_release_vertex_pack_accessors, _gltf_try_pack_vertices_pnc_raw, _gltf_apply_morph_vec3, _gltf_pack_unique_vertices, _gltf_store_index, _gltf_store_index2, _gltf_store_index3, _gltf_pack_index_value, _gltf_pack_point_indices, _gltf_pack_line_indices, _gltf_pack_triangle_strip_indices, _gltf_pack_triangle_fan_indices, _gltf_pack_triangle_indices, _gltf_pack_indices, _gltf_index_value, _gltf_expand_primitive_vertices, _gltf_copy_part_opts, _gltf_pack_cacheable_meta, _gltf_pack_vertex_cache_key, _gltf_pack_index_cache_key, _gltf_pack_primitive_buffers, _gltf_indexed_append_instance_parts, _gltf_indexed_scene_world_mats, _gltf_indexed_finish_result, gltf_to_mesh_group_indexed, gltf_warm_runtime)
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
use std.math.parse.3d.gltf.scene as scn

fn _gltf_make_indexed_part_from_state(
   any vptr, int vcnt, any iptr, int icnt, dict part_opts,
   dict material_state, int mat_idx, int uv_set, any model,
   int part_node_idx, dict skin_state, bool part_visible,
   bool has_gpu_instancing, int prim_mode, list world_bounds,
   int packed_color
) dict {
   def tex_id = int(material_state.get("tex_id", -1))
   def material_u32 = int(material_state.get("material_u32", 0x0000ff00))
   def normal_tex_id = int(material_state.get("normal_tex_id", -1))
   def normal_uv_set = int(material_state.get("normal_uv_set", 0))
   def emissive_tex_id = int(material_state.get("emissive_tex_id", -1))
   def emissive_uv_set = int(material_state.get("emissive_uv_set", 0))
   def occlusion_tex_id = int(material_state.get("occlusion_tex_id", -1))
   def occlusion_uv_set = int(material_state.get("occlusion_uv_set", 0))
   def met_rough_uv_set = int(material_state.get("met_rough_uv_set", 0))
   def skin_joints = skin_state.get("skin_joints", [])
   def bsdf2_u32 = scn._gltf_scale_bsdf2_thickness(int(material_state.get("bsdf2_u32", 0)), scn._gltf_model_mean_scale(model))
   {
      "vptr": vptr,
      "vcnt": vcnt,
      "iptr": iptr,
      "icnt": icnt,
      "opts": part_opts,
      "index_type_u32": part_opts.get("index_type_u32", false),
      "unlit": part_opts.get("unlit", false),
      "no_cull": part_opts.get("no_cull", false),
      "double_sided": part_opts.get("double_sided", false),
      "flip_winding": part_opts.get("flip_winding", false),
      "mat_idx": mat_idx,
      "tex_id": tex_id,
      "base_color_u32": int(material_state.get("prim_packed_color", packed_color)),
      "material_u32": material_u32,
      "base_uv_xf0": int(material_state.get("base_uv_xf0", 0)),
      "base_uv_xf1": int(material_state.get("base_uv_xf1", 0)),
      "normal_uv_xf0": int(material_state.get("normal_uv_xf0", 0)),
      "normal_uv_xf1": int(material_state.get("normal_uv_xf1", 0)),
      "mr_uv_xf0": int(material_state.get("mr_uv_xf0", 0)),
      "mr_uv_xf1": int(material_state.get("mr_uv_xf1", 0)),
      "occlusion_uv_xf0": int(material_state.get("occlusion_uv_xf0", 0)),
      "occlusion_uv_xf1": int(material_state.get("occlusion_uv_xf1", 0)),
      "emissive_uv_xf0": int(material_state.get("emissive_uv_xf0", 0)),
      "emissive_uv_xf1": int(material_state.get("emissive_uv_xf1", 0)),
      "is_points": part_opts.get("is_points", false),
      "is_lines": part_opts.get("is_lines", false),
      "vc_mode": int(material_state.get("prim_vc_mode", 0)),
      "normal_tex_id": normal_tex_id,
      "normal_uv_set": normal_uv_set,
      "normal_tex_word": int(material_state.get("normal_tex_word", 0x80000000)),
      "emissive_tex_id": emissive_tex_id,
      "emissive_u32": int(material_state.get("emissive_u32", 0)),
      "emissive_uv_set": emissive_uv_set,
      "alpha_u32": int(material_state.get("alpha_u32", 0)),
      "occlusion": occlusion_tex_id,
      "occlusion_uv_set": occlusion_uv_set,
      "bsdf0_u32": int(material_state.get("bsdf0_u32", 0)),
      "bsdf1_u32": int(material_state.get("bsdf1_u32", 0)),
      "bsdf2_u32": bsdf2_u32,
      "bsdf3_u32": int(material_state.get("bsdf3_u32", 0)),
      "bsdf4_u32": int(material_state.get("bsdf4_u32", 0)),
      "bsdf5_u32": int(material_state.get("bsdf5_u32", 0)),
      "ext2_tex_word": int(material_state.get("ext2_tex_word", 0x80000000)),
      "material_slots": mat._gltf_indexed_material_slots(material_state, tex_id, normal_tex_id, normal_uv_set, emissive_tex_id, emissive_uv_set, occlusion_tex_id, occlusion_uv_set, material_u32, uv_set, met_rough_uv_set),
      "model": model,
      "node_idx": part_node_idx,
      "skin_idx": int(skin_state.get("skin_idx", -1)),
      "skin_bind_vptr": skin_state.get("skin_bind_vptr", 0),
      "skin_joints_ptr": skin_state.get("skin_joints_ptr", 0),
      "skin_weights_ptr": skin_state.get("skin_weights_ptr", 0),
      "skin_vcnt": int(skin_state.get("skin_vcnt", vcnt)),
      "skin_mesh_bind_world": model,
      "skin_joint_nodes": skin_joints,
      "skin_joint_count": is_list(skin_joints) ? skin_joints.len : 0,
      "skin_inv_bind_accessor": int(skin_state.get("skin_inv_bind_accessor", -1)),
      "skin_single_influence": bool(skin_state.get("skin_single_influence", false)),
      "visible": part_visible,
      "instanced_part": has_gpu_instancing,
      "primitive_mode": prim_mode,
      "has_normals": part_opts.get("has_normals", false),
      "min": [float(world_bounds.get(0, 0.0)), float(world_bounds.get(1, 0.0)), float(world_bounds.get(2, 0.0))],
      "max": [float(world_bounds.get(3, 0.0)), float(world_bounds.get(4, 0.0)), float(world_bounds.get(5, 0.0))]
   }
}

fn gltf_mesh_count(any gltf_data) int {
   "Runs the mesh count operation."
   def meshes = shr._gltf_meshes(gltf_data)
   meshes ? meshes.len : 0
}

fn gltf_get_mesh(any gltf_data, int mesh_idx) any {
   "Runs the get mesh operation."
   def meshes = shr._gltf_meshes(gltf_data)
   if !meshes || mesh_idx < 0 || mesh_idx >= meshes.len { return 0 }
   meshes.get(mesh_idx)
}

fn _gltf_accessor_local_bounds(any acc) any {
   if !is_dict(acc) { return 0 }
   def mn, mx = acc.get("min", 0), acc.get("max", 0)
   if is_list(mn) && is_list(mx) && mn.len >= 3 && mx.len >= 3 {
      def x1 = shr._gltf_num_or(mn.get(0, 0.0), 0.0) def y1 = shr._gltf_num_or(mn.get(1, 0.0), 0.0) def z1 = shr._gltf_num_or(mn.get(2, 0.0), 0.0)
      def x2 = shr._gltf_num_or(mx.get(0, 0.0), 0.0) def y2 = shr._gltf_num_or(mx.get(1, 0.0), 0.0) def z2 = shr._gltf_num_or(mx.get(2, 0.0), 0.0)
      if shr._gltf_float6_bad(x1, y1, z1, x2, y2, z2) { return 0 }
      if x1 > x2 || y1 > y2 || z1 > z2 { return 0 }
      return [[x1, y1, z1], [x2, y2, z2]]
   }
   0
}

fn _gltf_prim_meta(dict prim, list accs) dict {
   def attrs = prim.get("attributes")
   def idx_acc = prim.get("indices", -1)
   def mat_idx = int(prim.get("material", -1))
   mut pos_acc_idx, pos_cnt = -1, 0
   mut uv_acc_idx, uv_cnt = -1, 0
   mut uv1_acc_idx, uv1_cnt = -1, 0
   mut c_acc_idx, c_cnt = -1, 0
   mut n_acc_idx, n_cnt = -1, 0
   mut i_acc_idx, i_cnt = -1, 0
   mut has_idx = false
   mut local_bounds = 0
   def prim_mode = int(prim.get("mode", shr.GLTF_MODE_TRIANGLES))
   if is_dict(attrs) {
      pos_acc_idx = attrs.get("POSITION", -1)
      if pos_acc_idx >= 0 && pos_acc_idx < accs.len {
         def acc = accs.get(pos_acc_idx)
         pos_cnt = acc.get("count", 0)
         local_bounds = _gltf_accessor_local_bounds(acc)
      }
      uv_acc_idx = attrs.get("TEXCOORD_0", -1)
      if uv_acc_idx >= 0 && uv_acc_idx < accs.len {
         def acc = accs.get(uv_acc_idx)
         uv_cnt = acc.get("count", 0)
      }
      uv1_acc_idx = attrs.get("TEXCOORD_1", -1)
      if uv1_acc_idx >= 0 && uv1_acc_idx < accs.len {
         def acc = accs.get(uv1_acc_idx)
         uv1_cnt = acc.get("count", 0)
      }
      c_acc_idx = attrs.get("COLOR_0", -1)
      if c_acc_idx >= 0 && c_acc_idx < accs.len {
         def acc = accs.get(c_acc_idx)
         c_cnt = acc.get("count", 0)
      }
      n_acc_idx = attrs.get("NORMAL", -1)
      if n_acc_idx >= 0 && n_acc_idx < accs.len {
         def acc = accs.get(n_acc_idx)
         n_cnt = acc.get("count", 0)
      }
   }
   def texcoord_sets = _gltf_collect_indexed_semantics(attrs, "TEXCOORD")
   def color_sets = _gltf_collect_indexed_semantics(attrs, "COLOR")
   def joints_sets = _gltf_collect_indexed_semantics(attrs, "JOINTS")
   def weights_sets = _gltf_collect_indexed_semantics(attrs, "WEIGHTS")
   def joints_acc_idx = is_dict(attrs) ? int(attrs.get("JOINTS_0", attrs.get("JOINT_0", -1))) : -1
   def weights_acc_idx = is_dict(attrs) ? int(attrs.get("WEIGHTS_0", attrs.get("WEIGHT_0", -1))) : -1
   def is_skinned_candidate = joints_acc_idx >= 0 && weights_acc_idx >= 0
   def t_acc_idx = is_dict(attrs) ? int(attrs.get("TANGENT", -1)) : -1
   mut t_cnt = 0
   if t_acc_idx >= 0 && t_acc_idx < accs.len {
      def acc = accs.get(t_acc_idx)
      t_cnt = acc.get("count", 0)
   }
   if idx_acc >= 0 && idx_acc < accs.len {
      def acc = accs.get(idx_acc)
      i_acc_idx = idx_acc
      i_cnt = acc.get("count", 0)
      has_idx = true
   }
   {
      "pos_acc_idx": pos_acc_idx,
      "pos_cnt": pos_cnt,
      "uv_acc_idx": uv_acc_idx,
      "uv_cnt": uv_cnt,
      "uv1_acc_idx": uv1_acc_idx,
      "uv1_cnt": uv1_cnt,
      "c_acc_idx": c_acc_idx,
      "c_cnt": c_cnt,
      "n_acc_idx": n_acc_idx,
      "n_cnt": n_cnt,
      "t_acc_idx": t_acc_idx,
      "t_cnt": t_cnt,
      "texcoord_sets": texcoord_sets,
      "color_sets": color_sets,
      "joints_sets": joints_sets,
      "weights_sets": weights_sets,
      "is_skinned_candidate": is_skinned_candidate,
      "joints_acc_idx": joints_acc_idx,
      "weights_acc_idx": weights_acc_idx,
      "i_acc_idx": i_acc_idx,
      "i_cnt": i_cnt,
      "has_idx": has_idx,
      "mat_idx": mat_idx,
      "local_bounds": local_bounds,
      "mode": prim_mode
   }
}

fn _gltf_collect_indexed_semantics(any attrs, str prefix) list {
   mut out = []
   if !is_dict(attrs) { return out }
   mut idx = 0
   while true {
      def key = prefix + "_" + to_str(idx)
      if !attrs.contains(key) { break }
      mut rec = dict(4)
      rec["set"] = idx
      rec["semantic"] = key
      rec["accessor"] = int(attrs.get(key, -1))
      out = out.append(rec)
      idx += 1
   }
   out
}

fn _gltf_release_vertex_pack_accessors(any pos_res, any uv0_res, any uv1_res, any c_res, any n_res, any t_res) int {
   ld._gltf_release_accessor_data(pos_res)
   ld._gltf_release_accessor_data(uv0_res)
   ld._gltf_release_accessor_data(uv1_res)
   ld._gltf_release_accessor_data(c_res)
   ld._gltf_release_accessor_data(n_res)
   ld._gltf_release_accessor_data(t_res)
   0
}

fn _gltf_try_pack_vertices_pnc_raw(
   any buf, int count, any pos_ptr, int pos_comp, bool pos_norm, int pos_stride,
   bool uv0_valid, int uv0_comp, bool uv0_norm, bool uv1_valid,
   bool n_valid, any n_ptr, int n_cnt, int n_comp, bool n_norm, int n_stride,
   bool t_valid, int t_comp, bool t_norm,
   bool c_valid, any c_ptr, int c_cnt, int c_stride, int c_comp, int c_type_count, bool c_norm,
   int tex_id, int morph_targets_n
) bool {
   false
}

fn _gltf_apply_morph_vec3(list morph_targets, int morph_targets_n, int vi, f64 x, f64 y, f64 z, str res_key) list {
   mut ox, oy = x, y
   mut oz, mi = z, 0
   while mi < morph_targets_n {
      def mt = morph_targets[mi]
      def mt_w = float(mt.get("weight", 0.0))
      if abs(mt_w) <= 0.0000001 {
         mi += 1
         continue
      }
      def mt_res = mt.get(res_key, 0)
      if is_dict(mt_res) && vi < int(mt_res.get("count", 0)) {
         def mt_ptr = mt_res.get("ptr", 0)
         def mt_comp = mt_res.get("comp", shr.GLTF_COMP_FLOAT)
         def mt_norm = mt_res.get("normalized", false)
         def mt_stride = mt_res.get("stride", 0)
         def mt_cs = shr._gltf_comp_size(mt_comp)
         def mt_off = vi * mt_stride
         ox += shr._gltf_read_f32_acc(mt_ptr, mt_off + mt_cs * 0, mt_comp, mt_norm) * mt_w
         oy += shr._gltf_read_f32_acc(mt_ptr, mt_off + mt_cs * 1, mt_comp, mt_norm) * mt_w
         oz += shr._gltf_read_f32_acc(mt_ptr, mt_off + mt_cs * 2, mt_comp, mt_norm) * mt_w
      }
      mi += 1
   }
   [ox, oy, oz]
}

fn _gltf_apply_morph_vec3_into(list morph_targets, int morph_targets_n, int vi, f64 x, f64 y, f64 z, str res_key, list out) list {
   mut ox, oy = x, y
   mut oz = z
   mut mi = 0
   while mi < morph_targets_n {
      def mt = morph_targets[mi]
      def mt_w = float(mt.get("weight", 0.0))
      if abs(mt_w) > 0.0000001 {
         def mt_res = mt.get(res_key, 0)
         if is_dict(mt_res) && vi < int(mt_res.get("count", 0)) {
            def mt_ptr = mt_res.get("ptr", 0)
            def mt_comp = mt_res.get("comp", shr.GLTF_COMP_FLOAT)
            def mt_norm = mt_res.get("normalized", false)
            def mt_stride = mt_res.get("stride", 0)
            def mt_cs = shr._gltf_comp_size(mt_comp)
            def mt_off = vi * mt_stride
            ox += shr._gltf_read_f32_acc(mt_ptr, mt_off + mt_cs * 0, mt_comp, mt_norm) * mt_w
            oy += shr._gltf_read_f32_acc(mt_ptr, mt_off + mt_cs * 1, mt_comp, mt_norm) * mt_w
            oz += shr._gltf_read_f32_acc(mt_ptr, mt_off + mt_cs * 2, mt_comp, mt_norm) * mt_w
         }
      }
      mi += 1
   }
   if out.len < 3 { __list_set_len(out, 3) }
   out[0] = ox
   out[1] = oy
   out[2] = oz
   out
}

fn _gltf_pack_unique_vertices(dict g, any data, dict meta, int packed_color, int tex_id, int uv_set=0, int uv_xform=0) any {
   def pos_res = ld._gltf_resolve_accessor_data(g, meta.get("pos_acc_idx", -1), data)
   def uv0_idx = meta.get("uv_acc_idx", -1)
   def uv1_idx = meta.get("uv1_acc_idx", -1)
   def uv0_res = ld._gltf_resolve_accessor_data(g, uv0_idx, data)
   def uv1_res = (uv1_idx >= 0 && uv1_idx != uv0_idx) ? ld._gltf_resolve_accessor_data(g, uv1_idx, data) : 0
   def c_res = ld._gltf_resolve_accessor_data(g, meta.get("c_acc_idx", -1), data)
   def n_res = ld._gltf_resolve_accessor_data(g, meta.get("n_acc_idx", -1), data)
   def t_res = ld._gltf_resolve_accessor_data(g, meta.get("t_acc_idx", -1), data)
   mut count = 0
   if is_dict(pos_res) { count = pos_res.get("count", 0) }
   if count <= 0 {
      _gltf_release_vertex_pack_accessors(pos_res, uv0_res, uv1_res, c_res, n_res, t_res)
      return 0
   }
   def buf = malloc(count * shr._GLTF_VTX_STRIDE)
   if !buf {
      _gltf_release_vertex_pack_accessors(pos_res, uv0_res, uv1_res, c_res, n_res, t_res)
      return 0
   }
   def pos_ptr = pos_res.get("ptr", 0)
   def pos_comp = pos_res.get("comp", 0)
   def pos_norm = pos_res.get("normalized", false)
   def pos_stride = pos_res.get("stride", 0)
   mut uv0_ptr, uv0_cnt, uv0_comp, uv0_norm, uv0_stride = 0, 0, 0, false, 0
   if is_dict(uv0_res) {
      uv0_ptr, uv0_cnt = uv0_res.get("ptr", 0), uv0_res.get("count", 0)
      uv0_comp, uv0_norm = uv0_res.get("comp", 0), uv0_res.get("normalized", false)
      uv0_stride = uv0_res.get("stride", 0)
   }
   mut uv1_ptr, uv1_cnt, uv1_comp, uv1_norm, uv1_stride = 0, 0, shr.GLTF_COMP_FLOAT, false, 0
   if is_dict(uv1_res) {
      uv1_ptr, uv1_cnt = uv1_res.get("ptr", 0), uv1_res.get("count", 0)
      uv1_comp, uv1_norm = uv1_res.get("comp", shr.GLTF_COMP_FLOAT), uv1_res.get("normalized", false)
      uv1_stride = uv1_res.get("stride", 0)
   }
   def uv1_cs = shr._gltf_comp_size(uv1_comp)
   mut n_ptr, n_cnt, n_comp, n_norm, n_stride = 0, 0, 0, false, 0
   if is_dict(n_res) {
      n_ptr, n_cnt = n_res.get("ptr", 0), n_res.get("count", 0)
      n_comp, n_norm = n_res.get("comp", 0), n_res.get("normalized", false)
      n_stride = n_res.get("stride", 0)
   }
   mut t_ptr, t_cnt, t_comp, t_norm, t_stride = 0, 0, 0, false, 0
   if is_dict(t_res) {
      t_ptr, t_cnt = t_res.get("ptr", 0), t_res.get("count", 0)
      t_comp, t_norm = t_res.get("comp", 0), t_res.get("normalized", false)
      t_stride = t_res.get("stride", 0)
   }
   if !pos_ptr {
      free(buf)
      _gltf_release_vertex_pack_accessors(pos_res, uv0_res, uv1_res, c_res, n_res, t_res)
      return 0
   }
   def morph_targets = anim._gltf_collect_morph_targets(g, data, meta.get("targets", 0), meta.get("morph_weights", 0))
   def morph_targets_n = morph_targets.len
   mut morph_has_norm = false
   mut morph_has_tan = false
   mut morph_i = 0
   while morph_i < morph_targets_n {
      def mt = morph_targets[morph_i]
      if is_dict(mt.get("norm_res", 0)) { morph_has_norm = true }
      if is_dict(mt.get("tan_res", 0)) { morph_has_tan = true }
      morph_i += 1
   }
   def pos_cs = shr._gltf_comp_size(pos_comp)
   def uv0_cs = shr._gltf_comp_size(uv0_comp)
   def n_cs = shr._gltf_comp_size(n_comp)
   def t_cs = shr._gltf_comp_size(t_comp)
   def uv0_valid = is_dict(uv0_res) && uv0_cnt > 0 && uv0_stride > 0 && uv0_cs > 0
   def uv1_valid = is_dict(uv1_res) && uv1_cnt > 0 && uv1_stride > 0 && uv1_cs > 0
   def n_valid = is_dict(n_res) && n_cnt > 0 && n_stride > 0 && n_cs > 0
   def t_valid = is_dict(t_res) && t_cnt > 0 && t_stride > 0 && t_cs > 0
   mut c_ptr, c_cnt, c_comp, c_stride, c_type_count, c_norm = 0, 0, 0, 0, 4, false
   if is_dict(c_res) {
      c_ptr = c_res.get("ptr", 0)
      c_cnt = int(c_res.get("count", 0))
      c_comp = int(c_res.get("comp", 0))
      c_stride = int(c_res.get("stride", 0))
      c_type_count = int(c_res.get("type_count", 4))
      c_norm = c_res.get("normalized", false) || c_comp != shr.GLTF_COMP_FLOAT
   }
   def c_valid = c_ptr && c_cnt > 0 && c_stride > 0 && (c_type_count == 3 || c_type_count == 4)
   mut morph_pos = [0.0, 0.0, 0.0]
   mut morph_norm = [0.0, 0.0, 0.0]
   mut morph_tan = [0.0, 0.0, 0.0]
   if _gltf_try_pack_vertices_pnc_raw(buf, count, pos_ptr, pos_comp, pos_norm, pos_stride,
      uv0_valid, uv0_comp, uv0_norm, uv1_valid, n_valid, n_ptr, n_cnt, n_comp, n_norm, n_stride,
      t_valid, t_comp, t_norm, c_valid, c_ptr, c_cnt, c_stride, c_comp, c_type_count, c_norm,
      tex_id, morph_targets_n){
      _gltf_release_vertex_pack_accessors(pos_res, uv0_res, uv1_res, c_res, n_res, t_res)
      mut out = dict(4)
      out["ptr"] = buf
      out["count"] = count
      return out
   }
   mut vi = 0
   if vi < count {
      while vi < count {
         def pbase = vi * pos_stride
         def px = shr._gltf_read_f32_acc(pos_ptr, pbase + pos_cs * 0, pos_comp, pos_norm)
         def py = shr._gltf_read_f32_acc(pos_ptr, pbase + pos_cs * 1, pos_comp, pos_norm)
         def pz = shr._gltf_read_f32_acc(pos_ptr, pbase + pos_cs * 2, pos_comp, pos_norm)
         morph_pos = _gltf_apply_morph_vec3_into(morph_targets, morph_targets_n, vi, px, py, pz, "pos_res", morph_pos)
         def mx, my = float(morph_pos.get(0, px)), float(morph_pos.get(1, py))
         def mz = float(morph_pos.get(2, pz))
         mut nx, ny = 0.0, 0.0
         mut nz = 0.0
         if n_valid && vi < n_cnt {
            def nbase = vi * n_stride
            nx = shr._gltf_read_f32_acc(n_ptr, nbase + n_cs * 0, n_comp, n_norm)
            ny = shr._gltf_read_f32_acc(n_ptr, nbase + n_cs * 1, n_comp, n_norm)
            nz = shr._gltf_read_f32_acc(n_ptr, nbase + n_cs * 2, n_comp, n_norm)
         }
         if morph_has_norm {
            morph_norm = _gltf_apply_morph_vec3_into(morph_targets, morph_targets_n, vi, nx, ny, nz, "norm_res", morph_norm)
            nx, ny = float(morph_norm.get(0, nx)), float(morph_norm.get(1, ny))
            nz = float(morph_norm.get(2, nz))
         }
         def nl = sqrt(nx * nx + ny * ny + nz * nz)
         if nl > 0.00001 { nx /= nl ny /= nl nz /= nl }
         mut tx, ty = 0.0, 0.0
         mut tz, tw = 0.0, 1.0
         if t_valid && vi < t_cnt {
            def tbase = vi * t_stride
            tx = shr._gltf_read_f32_acc(t_ptr, tbase + t_cs * 0, t_comp, t_norm)
            ty = shr._gltf_read_f32_acc(t_ptr, tbase + t_cs * 1, t_comp, t_norm)
            tz = shr._gltf_read_f32_acc(t_ptr, tbase + t_cs * 2, t_comp, t_norm)
            tw = shr._gltf_read_f32_acc(t_ptr, tbase + t_cs * 3, t_comp, t_norm)
         }
         if morph_has_tan {
            morph_tan = _gltf_apply_morph_vec3_into(morph_targets, morph_targets_n, vi, tx, ty, tz, "tan_res", morph_tan)
            tx, ty = float(morph_tan.get(0, tx)), float(morph_tan.get(1, ty))
            tz = float(morph_tan.get(2, tz))
         }
         def tl = sqrt(tx * tx + ty * ty + tz * tz)
         if tl > 0.00001 { tx /= tl ty /= tl tz /= tl }
         def off = ptr_add(buf, vi * shr._GLTF_VTX_STRIDE)
         def color_u32 = shr._gltf_vertex_color_u32(c_res, vi, 0xffffffff)
         store32_f32(off, mx, shr._GLTF_VTX_OFF_X)
         store32_f32(off, my, shr._GLTF_VTX_OFF_Y)
         store32_f32(off, mz, shr._GLTF_VTX_OFF_Z)
         store32_f32(off, 0.0, shr._GLTF_VTX_OFF_U)
         store32_f32(off, 0.0, shr._GLTF_VTX_OFF_V)
         if uv0_valid && vi < uv0_cnt {
            def ubase = vi * uv0_stride
            store32_f32(off, shr._gltf_read_f32_acc(uv0_ptr, ubase + uv0_cs * 0, uv0_comp, uv0_norm), shr._GLTF_VTX_OFF_U)
            store32_f32(off, shr._gltf_read_f32_acc(uv0_ptr, ubase + uv0_cs * 1, uv0_comp, uv0_norm), shr._GLTF_VTX_OFF_V)
         }
         store32(off, color_u32, shr._GLTF_VTX_OFF_C)
         store32_f32(off, nx, shr._GLTF_VTX_OFF_NX)
         store32_f32(off, ny, shr._GLTF_VTX_OFF_NY)
         store32_f32(off, nz, shr._GLTF_VTX_OFF_NZ)
         store32_f32(off, tx, shr._GLTF_VTX_OFF_TX)
         store32_f32(off, ty, shr._GLTF_VTX_OFF_TY)
         store32_f32(off, tz, shr._GLTF_VTX_OFF_TZ)
         store32_f32(off, tw, shr._GLTF_VTX_OFF_TW)
         store32_f32(off, 0.0, shr._GLTF_VTX_OFF_U2)
         store32_f32(off, 0.0, shr._GLTF_VTX_OFF_V2)
         if uv1_valid && vi < uv1_cnt {
            def sbase = vi * uv1_stride
            store32_f32(off, shr._gltf_read_f32_acc(uv1_ptr, sbase + uv1_cs * 0, uv1_comp, uv1_norm), shr._GLTF_VTX_OFF_U2)
            store32_f32(off, shr._gltf_read_f32_acc(uv1_ptr, sbase + uv1_cs * 1, uv1_comp, uv1_norm), shr._GLTF_VTX_OFF_V2)
         }
         store32(off, tex_id, shr._GLTF_VTX_OFF_TEX)
         vi += 1
      }
   }
   _gltf_release_vertex_pack_accessors(pos_res, uv0_res, uv1_res, c_res, n_res, t_res)
   anim._gltf_release_morph_targets(morph_targets)
   mut out = dict(4)
   out["ptr"] = buf
   out["count"] = count
   out
}

fn _gltf_store_index(any out, bool use_u32, int v, int out_cnt) int {
   if use_u32 { store32(out, v, out_cnt * 4) } else { store16(out, v, out_cnt * 2) }
   out_cnt + 1
}

fn _gltf_store_index2(any out, bool use_u32, int a, int b, int out_cnt) int {
   out_cnt = _gltf_store_index(out, use_u32, a, out_cnt)
   _gltf_store_index(out, use_u32, b, out_cnt)
}

fn _gltf_store_index3(any out, bool use_u32, int a, int b, int c, int out_cnt) int {
   out_cnt = _gltf_store_index(out, use_u32, a, out_cnt)
   out_cnt = _gltf_store_index(out, use_u32, b, out_cnt)
   _gltf_store_index(out, use_u32, c, out_cnt)
}

fn _gltf_pack_index_value(bool has_idx, any idx_ptr, int idx_stride, int idx_comp, int i) int {
   if has_idx { return shr._gltf_read_index_acc(idx_ptr, i * idx_stride, idx_comp) }
   i
}

fn _gltf_pack_point_indices(any out, bool use_u32, bool has_idx, any idx_ptr, int idx_stride, int idx_comp, int src_count) int {
   mut out_cnt, i = 0, 0
   while i < src_count {
      out_cnt = _gltf_store_index(out, use_u32, _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i), out_cnt)
      i += 1
   }
   out_cnt
}

fn _gltf_pack_line_indices(any out, bool use_u32, bool has_idx, any idx_ptr, int idx_stride, int idx_comp, int src_count, int step, bool close_loop) int {
   mut out_cnt, i = 0, 0
   while i + 1 < src_count {
      def a = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i)
      def b = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i + 1)
      out_cnt = _gltf_store_index2(out, use_u32, a, b, out_cnt)
      i += step
   }
   if close_loop && src_count > 1 {
      def a = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, src_count - 1)
      def b = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, 0)
      out_cnt = _gltf_store_index2(out, use_u32, a, b, out_cnt)
   }
   out_cnt
}

fn _gltf_pack_triangle_strip_indices(any out, bool use_u32, bool has_idx, any idx_ptr, int idx_stride, int idx_comp, int src_count) int {
   mut out_cnt, i = 0, 0
   while i + 2 < src_count {
      def a = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i)
      def b = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i + 1)
      def c = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i + 2)
      if (i & 1) == 0 { out_cnt = _gltf_store_index3(out, use_u32, a, b, c, out_cnt) }
      else { out_cnt = _gltf_store_index3(out, use_u32, b, a, c, out_cnt) }
      i += 1
   }
   out_cnt
}

fn _gltf_pack_triangle_fan_indices(any out, bool use_u32, bool has_idx, any idx_ptr, int idx_stride, int idx_comp, int src_count) int {
   mut out_cnt, i = 0, 1
   def base = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, 0)
   while i + 1 < src_count {
      def b = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i)
      def c = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i + 1)
      out_cnt = _gltf_store_index3(out, use_u32, base, b, c, out_cnt)
      i += 1
   }
   out_cnt
}

fn _gltf_pack_triangle_indices(any out, bool use_u32, bool has_idx, any idx_ptr, int idx_stride, int idx_comp, int src_count) int {
   mut out_cnt, i = 0, 0
   if has_idx {
      while i < src_count {
         out_cnt = _gltf_store_index(out, use_u32, _gltf_pack_index_value(true, idx_ptr, idx_stride, idx_comp, i), out_cnt)
         i += 1
      }
      return out_cnt
   }
   while i + 2 < src_count {
      out_cnt = _gltf_store_index3(out, use_u32, i, i + 1, i + 2, out_cnt)
      i += 3
   }
   out_cnt
}

fn _gltf_pack_indices(any g, any data, any meta) any {
   "Packs index buffer. Returns {ptr, count, u32}. Uses u32 if > 65535 verts.
   For non-indexed primitives(has_idx=false) generates a sequential [0..n-1] index buffer."
   def has_idx = meta.get("has_idx", false)
   def pos_cnt = meta.get("pos_cnt", 0)
   def prim_mode = int(meta.get("mode", shr.GLTF_MODE_TRIANGLES))
   mut idx_res = 0
   mut i_cnt, i_comp, idx_ptr, idx_stride = 0, 0, 0, 0
   if has_idx {
      idx_res = ld._gltf_resolve_accessor_data(g, meta.get("i_acc_idx", -1), data)
      if is_dict(idx_res) {
         i_cnt, i_comp = idx_res.get("count", 0), idx_res.get("comp", 0)
         idx_ptr, idx_stride = idx_res.get("ptr", 0), idx_res.get("stride", 0)
      }
      if i_cnt <= 0 {
         ld._gltf_release_accessor_data(idx_res)
         return 0
      }
   }
   def src_count = has_idx ? i_cnt : pos_cnt
   def use_u32 = pos_cnt > 65535
   def max_out = src_count * 3
   def esize = use_u32 ? 4 : 2
   def out = malloc(max_out * esize)
   if !out {
      if has_idx { ld._gltf_release_accessor_data(idx_res) }
      return 0
   }
   def out_cnt = case prim_mode {
      0 -> _gltf_pack_point_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count)
      1 -> _gltf_pack_line_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count, 2, false)
      2 -> _gltf_pack_line_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count, 1, true)
      3 -> _gltf_pack_line_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count, 1, false)
      5 -> _gltf_pack_triangle_strip_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count)
      6 -> _gltf_pack_triangle_fan_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count)
      _ -> _gltf_pack_triangle_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count)
   }
   if has_idx { ld._gltf_release_accessor_data(idx_res) }
   return {"ptr": out, "count": out_cnt, "u32": use_u32, "mode": prim_mode}
}

fn _gltf_index_value(any iptr, bool idx_u32, int idx) int { idx_u32 ? load32(iptr, idx * 4) : load16(iptr, idx * 2) }

fn _gltf_expand_primitive_vertices(?ptr vptr, int vcnt, ?ptr iptr, int icnt, bool idx_u32=false) any {
   if !vptr || vcnt <= 0 || !iptr || icnt <= 0 { return 0 }
   def out = malloc(icnt * shr._GLTF_VTX_STRIDE)
   if !out { return 0 }
   mut i = 0
   while i < icnt {
      def vi = _gltf_index_value(iptr, idx_u32, i)
      if vi < 0 || vi >= vcnt {
         free(out)
         return 0
      }
      memcpy(ptr_add(out, i * shr._GLTF_VTX_STRIDE), ptr_add(vptr, vi * shr._GLTF_VTX_STRIDE), shr._GLTF_VTX_STRIDE)
      i += 1
   }
   {"ptr": out, "count": icnt}
}

fn _gltf_copy_part_opts(any opts) dict {
   mut out = dict(8)
   if !is_dict(opts) { return out }
   def flags = ["index_type_u32", "is_points", "is_lines", "unlit", "no_cull", "double_sided", "flip_winding"]
   mut i = 0
   while i < flags.len {
      def key = to_str(flags.get(i))
      if opts.get(key, false) { out[key] = true }
      i += 1
   }
   def storage = opts.get("storage", "")
   if is_str(storage) && storage.len > 0 { out["storage"] = storage }
   out
}


fn _gltf_node_or_mesh_morph_weights(any g, int mesh_idx, int node_idx, int target_count) list {
   "Returns the morph weights that apply to one mesh instance. Node weights override mesh defaults."
   mut out = anim._gltf_mesh_morph_weights(g, mesh_idx, target_count)
   if target_count <= 0 { return out }
   def nodes = g.get("nodes", 0)
   if is_list(nodes) && node_idx >= 0 && node_idx < nodes.len {
      def node = nodes[node_idx]
      def node_w = is_dict(node) ? node.get("weights", 0) : 0
      if is_list(node_w) {
         mut wi = 0
         while wi < target_count && wi < node_w.len {
            out[wi] = float(node_w[wi])
            wi += 1
         }
      }
   }
   out
}

fn _gltf_instance_node_idx(any inst_pair) int {
   if is_list(inst_pair) && inst_pair.len == 2 && is_list(inst_pair.get(0)) { return int(inst_pair.get(1, -1)) }
   -1
}

fn _gltf_pack_cacheable_meta(any meta) bool {
   if !is_dict(meta) { return false }
   def targets = meta.get("targets", 0)
   !is_list(targets) || targets.len == 0
}

fn _gltf_pack_vertex_cache_key(any meta, int packed_color, int tex_id, int uv_set, any uv_xform) str {
   to_str(int(meta.get("pos_acc_idx", -1))) + "|" +
   to_str(int(meta.get("uv_acc_idx", -1))) + "|" +
   to_str(int(meta.get("uv1_acc_idx", -1))) + "|" +
   to_str(int(meta.get("c_acc_idx", -1))) + "|" +
   to_str(int(meta.get("n_acc_idx", -1))) + "|" +
   to_str(int(meta.get("t_acc_idx", -1))) + "|" +
   to_str(int(packed_color)) + "|" +
   to_str(int(tex_id)) + "|" +
   to_str(int(uv_set)) + "|" +
   to_str(to_int(uv_xform))
}

fn _gltf_pack_index_cache_key(any meta) str {
   to_str(int(meta.get("i_acc_idx", -1))) + "|" +
   to_str(int(meta.get("pos_cnt", 0))) + "|" +
   to_str(int(meta.get("mode", shr.GLTF_MODE_TRIANGLES)))
}

fn _gltf_pack_primitive_buffers(any g, any data, any meta, int packed_color, int tex_id, int uv_set, any uv_xform, bool pack_cache_enabled, any vertex_pack_cache, any index_pack_cache) list {
   mut verts = 0
   mut inds = 0
   mut vcache = vertex_pack_cache
   mut icache = index_pack_cache
   def use_pack_cache = pack_cache_enabled && _gltf_pack_cacheable_meta(meta)
   if use_pack_cache {
      def vk = _gltf_pack_vertex_cache_key(meta, packed_color, tex_id, uv_set, uv_xform)
      verts = vcache.get(vk, 0)
      if !is_dict(verts) {
         verts = _gltf_pack_unique_vertices(g, data, meta, packed_color, tex_id, uv_set, uv_xform)
         if is_dict(verts) { vcache[vk] = verts }
      }
      def ik = _gltf_pack_index_cache_key(meta)
      inds = icache.get(ik, 0)
      if !is_dict(inds) {
         inds = _gltf_pack_indices(g, data, meta)
         if is_dict(inds) { icache[ik] = inds }
      }
   } else {
      verts = _gltf_pack_unique_vertices(g, data, meta, packed_color, tex_id, uv_set, uv_xform)
      inds = _gltf_pack_indices(g, data, meta)
   }
   [verts, inds, vcache, icache]
}

fn _gltf_indexed_append_instance_parts(
   list parts, list scene_bounds, dict gltf_data, dict g, any data, any nodes,
   list instance_mats, dict node_vis_map, dict meta, any vptr, int vcnt,
   any iptr, int icnt, dict mesh_opts, dict material_state,
   int uv_set, int packed_color, int prim_mode, any lbs
) list {
   def mat_idx = int(meta.get("mat_idx", 0))
   def instance_mats_n = instance_mats.len
   mut mi = 0
   while mi < instance_mats_n {
      def inst_pair = instance_mats.get(mi)
      def is_mat_pair = is_list(inst_pair) && inst_pair.len == 2 && is_list(inst_pair.get(0))
      def base_model = is_mat_pair ? inst_pair.get(0) : inst_pair
      def part_node_idx = is_mat_pair ? int(inst_pair.get(1, -1)) : -1
      mut sub_mats = 0
      mut has_gpu_instancing = false
      if part_node_idx >= 0 && part_node_idx < nodes.len {
         def node_obj = nodes.get(part_node_idx, 0)
         if is_dict(node_obj) {
            if node_obj.contains("extensions") {
               sub_mats = scn._gltf_resolve_instancing_mats(g, node_obj, data, base_model)
               has_gpu_instancing = is_list(sub_mats) && sub_mats.len > 0
            }
         }
      }
      mut sub_mats_list = [base_model]
      if has_gpu_instancing { sub_mats_list = sub_mats }
      def sub_mats_n = sub_mats_list.len
      mut smi = 0
      while smi < sub_mats_n {
         def model = gltf_math.safe_model_mat4(sub_mats_list.get(smi))
         mut part_visible = true
         if part_node_idx >= 0 { part_visible = node_vis_map.get(part_node_idx, true) ? true : false }
         mut part_opts = _gltf_copy_part_opts(mesh_opts)
         def flip_winding = scn._gltf_model_has_negative_det(model)
         if flip_winding { part_opts["flip_winding"] = true }
         def world_bounds = mat._gltf_part_world_bounds(lbs, model)
         if part_visible { scene_bounds = mat._gltf_scene_bounds_accum_part(scene_bounds, world_bounds) }
         def skin_state = scn._gltf_resolve_part_skin(gltf_data, g, data, nodes, part_node_idx, meta, vptr, vcnt)
         parts = parts.append(_gltf_make_indexed_part_from_state(vptr,
               vcnt,
               iptr,
               icnt,
               part_opts,
               material_state,
               mat_idx,
               uv_set,
               model,
               part_node_idx,
               skin_state,
               part_visible,
               has_gpu_instancing,
               prim_mode,
               world_bounds,
         packed_color))
         smi += 1
      }
      mi += 1
   }
   [parts, scene_bounds]
}

fn _gltf_indexed_scene_world_mats(dict g, any gltf_data, int mesh_limit, int nodes_n) dict {
   def node_cap = (mesh_limit > 0 && mesh_limit < nodes_n) ? (mesh_limit + 64) : nodes_n
   mut node_world_mats = dict(max(16, node_cap * 2))
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
            if scn._gltf_root_relevant_for_mesh_limit(g, root_idx, mesh_limit) {
               node_world_mats = scn._gltf_build_node_world_mats(g, root_idx, id, node_world_mats)
            }
            ri += 1
         }
      }
   }
   node_world_mats
}

fn _gltf_indexed_finish_result(list parts, list scene_bounds) dict {
   def final_bounds = mat._gltf_scene_bounds_result(scene_bounds)
   {"parts": parts,
      "min": final_bounds.get(0),
      "max": final_bounds.get(1),
   "loader_bounds_ready": true}
}

fn gltf_to_mesh_group_indexed(any gltf_data, any color=0, any material_tex_ids=0) any {
   "Loads glTF as indexed raw vertex/index data.
   Returns {parts, min, max} where each part has {vptr, vcnt, iptr, icnt, opts, mat_idx, tex_id, model}.
   Caller must upload to GPU via mesh_create_indexed for each part x instance."
   if !is_dict(gltf_data) { return 0 }
   def g = gltf_data.get("gltf", 0)
   def data = ld._gltf_primary_data_ptr(gltf_data)
   if !is_dict(g) || !data { return 0 }
   def meshes = g.get("meshes")
   def nodes = g.get("nodes")
   if !is_list(meshes) { return 0 }
   mut meshes_n = meshes.len
   def mesh_limit = int(gltf_data.get("__mesh_limit", 0))
   if mesh_limit > 0 && mesh_limit < meshes_n {
      meshes_n = mesh_limit
   }
   def nodes_n = is_list(nodes) ? nodes.len : 0
   def accs = g.get("accessors")
   def bv_list = g.get("bufferViews")
   if !is_list(accs) || !is_list(bv_list) { return 0 }
   def packed_color = mat._gltf_indexed_default_color(gltf_data)
   def mat_setup = mat._gltf_indexed_material_record_setup(gltf_data, material_tex_ids, mesh_limit)
   def material_tex_ids_n = int(mat_setup.get(0, 0))
   def mat_records = mat_setup.get(1, [])
   mut parts = []
   mut scene_bounds = mat._gltf_scene_bounds_new()
   mut use_static = "static"
   if is_dict(gltf_data) { use_static = gltf_data.get("storage", "static") }
   def node_world_mats = _gltf_indexed_scene_world_mats(g, gltf_data, mesh_limit, nodes_n)
   def mesh_instance_map = scn._gltf_compute_mesh_instance_mats_fast(g, node_world_mats, mesh_limit)
   def scene_scoped_instances = is_dict(node_world_mats) && node_world_mats.len > 0
   def node_vis_map = scn.gltf_resolve_node_visibility(gltf_data, 0)
   def pack_cache_enabled = !common.env_truthy("NY_GLTF_PACK_CACHE_OFF")
   mut vertex_pack_cache = dict(64)
   mut index_pack_cache = dict(64)
   mut mesh_idx = 0
   while mesh_idx < meshes_n {
      def mesh = meshes.get(mesh_idx)
      def primitives = mesh.get("primitives", [])
      def primitives_n = is_list(primitives) ? primitives.len : 0
      mut instance_mats = mesh_instance_map.get(mesh_idx, 0)
      if !is_list(instance_mats) || instance_mats.len == 0 {
         if scene_scoped_instances {
            mesh_idx += 1
            continue
         }
         instance_mats = [[gltf_math.mat4_identity(), -1]]
      }
      mut pi = 0
      while pi < primitives_n {
         def prim = primitives.get(pi)
         mut meta = _gltf_prim_meta(prim, accs)
         if is_dict(meta) {
            def targets = prim.get("targets", 0)
            def targets_n = is_list(targets) ? targets.len : 0
            if targets_n > 0 {
               meta["targets"] = targets
               meta["morph_target_count"] = targets_n
               meta["morph_weights"] = anim._gltf_mesh_morph_weights(g, mesh_idx, targets_n)
            }
         }
         if is_dict(meta) {
            def material_state = mat._gltf_indexed_prim_material_state(material_tex_ids,
               material_tex_ids_n,
               mat_records,
               packed_color,
            meta)
            def uv_set = int(material_state.get("uv_set", 0))
            def uv_xform = material_state.get("uv_xform", 0)
            def tex_id = int(material_state.get("tex_id", -1))
            def prim_packed_color = int(material_state.get("prim_packed_color", packed_color))
            def prim_mode = int(meta.get("mode", shr.GLTF_MODE_TRIANGLES))
            def lbs = meta.get("local_bounds", 0)
            def inst_n = is_list(instance_mats) ? instance_mats.len : 0
            def has_morph_targets = int(meta.get("morph_target_count", 0)) > 0 || is_list(meta.get("targets", 0))
            mut inst_i = 0
            while inst_i < (has_morph_targets ? max(inst_n, 1) : 1) {
               def inst_pair = (has_morph_targets && inst_n > 0) ? instance_mats.get(inst_i) : 0
               def inst_node_idx = has_morph_targets ? _gltf_instance_node_idx(inst_pair) : -1
               mut meta_inst = meta
               if has_morph_targets {
                  meta_inst = dict_clone(meta)
                  meta_inst["morph_weights"] = _gltf_node_or_mesh_morph_weights(g, mesh_idx, inst_node_idx, int(meta.get("morph_target_count", 0)))
               }
               def packed_buffers = _gltf_pack_primitive_buffers(g, data, meta_inst, prim_packed_color, tex_id, uv_set, uv_xform, pack_cache_enabled, vertex_pack_cache, index_pack_cache)
               def verts = packed_buffers.get(0, 0)
               def inds = packed_buffers.get(1, 0)
               vertex_pack_cache = packed_buffers.get(2, vertex_pack_cache)
               index_pack_cache = packed_buffers.get(3, index_pack_cache)
               if is_dict(verts) && is_dict(inds) {
                  mut vptr = verts.get("ptr", 0)
                  mut vcnt = verts.get("count", 0)
                  mut iptr = inds.get("ptr", 0)
                  mut icnt = inds.get("count", 0)
                  mut idx_u32 = inds.get("u32", false)
                  if shr._gltf_prim_mode_expands_to_vertices(prim_mode) {
                     def expanded_prim = _gltf_expand_primitive_vertices(vptr, vcnt, iptr, icnt, idx_u32)
                     if is_dict(expanded_prim) {
                        vptr = expanded_prim.get("ptr", 0)
                        vcnt = int(expanded_prim.get("count", 0))
                        iptr = 0
                        icnt = 0
                        idx_u32 = false
                     }
                  }
                  def mesh_opts = mat._gltf_indexed_mesh_opts(material_state, idx_u32, meta_inst, prim_mode, use_static)
                  def emit_instances = has_morph_targets ? [inst_pair] : instance_mats
                  def emitted = _gltf_indexed_append_instance_parts(parts, scene_bounds, gltf_data, g, data, nodes, emit_instances,
                     node_vis_map, meta_inst, vptr, vcnt, iptr, icnt, mesh_opts, material_state,
                  uv_set, packed_color, prim_mode, lbs)
                  parts = emitted.get(0, parts)
                  scene_bounds = emitted.get(1, scene_bounds)
               }
               inst_i += 1
            }
         }
         pi += 1
      }
      mesh_idx += 1
   }
   _gltf_indexed_finish_result(parts, scene_bounds)
}

fn gltf_warm_runtime() bool {
   "Warms the indexed glTF packing helpers so first real scene load does not pay their JIT/setup cost."
   true
}
