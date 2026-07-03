;; Keywords: 3d gltf glb parse
;; glTF 2.0 loader with proper indexed primitive expansion
;; References: std.math.parse.3d
module std.math.parse.3d.gltf(
   load_gltf,
   load_gltf_file,
   parse_gltf_str,
   gltf_mesh_count,
   gltf_get_mesh,
   gltf_material_infos,
   gltf_material_infos_limited,
   gltf_material_info,
   gltf_material_feature_mask,
   _gltf_pack_uv_xform_words,
   _pack_material_word,
   gltf_to_mesh_group_indexed,
   gltf_scene_punctual_lights,
   gltf_camera_count,
   gltf_camera_info,
   gltf_camera_instances,
   gltf_skin_info,
   gltf_warm_runtime,
   gltf_free_data,
   gltf_skin_count,
   gltf_morph_target_count,
   gltf_animation_count,
   gltf_animation_info,
   gltf_sample_animation,
   gltf_rebuild_animated_mats,
   gltf_skin_joint_mats,
   gltf_apply_skinning,
   gltf_free_skin_mats_cache,
   gltf_supported_extension_caps,
   gltf_extensions_report,
   gltf_required_extension_failures,
   gltf_has_node_visibility,
   gltf_resolve_node_visibility,
   gltf_apply_morph_weights,
   gltf_sample_animation_merged,
   _gltf_read_f32_fast,
   _gltf_comp_size,
   _gltf_type_count,
   GLTF_COMP_NONE,
   GLTF_COMP_BYTE,
   GLTF_COMP_UBYTE,
   GLTF_COMP_SHORT,
   GLTF_COMP_USHORT,
   GLTF_COMP_UINT,
   GLTF_COMP_FLOAT,
   GLTF_TYPE_SCALAR,
   GLTF_TYPE_VEC2,
   GLTF_TYPE_VEC3,
   GLTF_TYPE_VEC4,
   GLTF_TYPE_MAT2,
   GLTF_TYPE_MAT3,
   GLTF_TYPE_MAT4
)

use std.math.parse.3d.gltf.shared as sub_shared
use std.math.parse.3d.gltf.load as sub_load
use std.math.parse.3d.gltf.material as sub_material
use std.math.parse.3d.gltf.animation as sub_anim
use std.math.parse.3d.gltf.scene as sub_scene
use std.math.parse.3d.gltf.mesh as sub_mesh
use std.math.parse.3d.gltf.math as math

fn load_gltf(any a, any b) any = sub_load.load_gltf(a, b)

fn load_gltf_file(any a) any = sub_load.load_gltf_file(a)

fn parse_gltf_str(any a, any b="", any c=0) any = sub_load.parse_gltf_str(a, b, c)

fn gltf_mesh_count(any a) any = sub_mesh.gltf_mesh_count(a)

fn gltf_get_mesh(any a, any b) any = sub_mesh.gltf_get_mesh(a, b)

fn gltf_material_infos(any a) any = sub_material.gltf_material_infos(a)

fn gltf_material_infos_limited(any a, any b) any = sub_material.gltf_material_infos_limited(a, b)

fn gltf_material_info(any a, any b) any = sub_material.gltf_material_info(a, b)

fn gltf_material_feature_mask(any a) any = sub_material.gltf_material_feature_mask(a)

fn gltf_to_mesh_group_indexed(any a, any b, any c) any = sub_mesh.gltf_to_mesh_group_indexed(a, b, c)

fn gltf_scene_punctual_lights(any a, any b) any = sub_scene.gltf_scene_punctual_lights(a, b)

fn gltf_camera_count(any a) any = sub_scene.gltf_camera_count(a)

fn gltf_camera_info(any a, any b) any = sub_scene.gltf_camera_info(a, b)

fn gltf_camera_instances(any a) any = sub_scene.gltf_camera_instances(a)

fn gltf_skin_info(any a, any b) any = sub_anim.gltf_skin_info(a, b)

fn gltf_warm_runtime() any = sub_mesh.gltf_warm_runtime()

fn gltf_free_data(any a) any = sub_load.gltf_free_data(a)

fn gltf_skin_count(any a) any = sub_anim.gltf_skin_count(a)

fn gltf_morph_target_count(any a) any = sub_anim.gltf_morph_target_count(a)

fn gltf_animation_count(any a) any = sub_anim.gltf_animation_count(a)

fn gltf_animation_info(any a, any b) any = sub_anim.gltf_animation_info(a, b)

fn gltf_sample_animation(any a, any b, any c) any = sub_anim.gltf_sample_animation(a, b, c)

fn gltf_rebuild_animated_mats(any a, any b) any = sub_anim.gltf_rebuild_animated_mats(a, b)

fn gltf_skin_joint_mats(any a, any b, any c, any d) any = sub_anim.gltf_skin_joint_mats(a, b, c, d)

fn gltf_apply_skinning(any a, any b, any c, any d) any = sub_anim.gltf_apply_skinning(a, b, c, d)

fn gltf_free_skin_mats_cache(any a) any = sub_anim.gltf_free_skin_mats_cache(a)

fn gltf_supported_extension_caps() any = sub_shared.gltf_supported_extension_caps()

fn gltf_extensions_report(any a) any = sub_shared.gltf_extensions_report(a)

fn gltf_required_extension_failures(any a) any = sub_shared.gltf_required_extension_failures(a)

fn gltf_has_node_visibility(any a, any b) any = sub_scene.gltf_has_node_visibility(a, b)

fn gltf_resolve_node_visibility(any a, any b) any = sub_scene.gltf_resolve_node_visibility(a, b)

fn gltf_apply_morph_weights(any a, any b) any = sub_anim.gltf_apply_morph_weights(a, b)

fn gltf_sample_animation_merged(any a, any b) any = sub_anim.gltf_sample_animation_merged(a, b)

fn _gltf_read_f32_fast(any data, int offset, int comp_type) f64 = sub_shared._gltf_read_f32_fast(data, offset, comp_type)
def _gltf_comp_size = sub_shared._gltf_comp_size
def _gltf_type_count = sub_shared._gltf_type_count
def GLTF_COMP_NONE = sub_shared.GLTF_COMP_NONE
def GLTF_COMP_BYTE = sub_shared.GLTF_COMP_BYTE
def GLTF_COMP_UBYTE = sub_shared.GLTF_COMP_UBYTE
def GLTF_COMP_SHORT = sub_shared.GLTF_COMP_SHORT
def GLTF_COMP_USHORT = sub_shared.GLTF_COMP_USHORT
def GLTF_COMP_UINT = sub_shared.GLTF_COMP_UINT
def GLTF_COMP_FLOAT = sub_shared.GLTF_COMP_FLOAT
def GLTF_TYPE_SCALAR = sub_shared.GLTF_TYPE_SCALAR
def GLTF_TYPE_VEC2 = sub_shared.GLTF_TYPE_VEC2
def GLTF_TYPE_VEC3 = sub_shared.GLTF_TYPE_VEC3
def GLTF_TYPE_VEC4 = sub_shared.GLTF_TYPE_VEC4
def GLTF_TYPE_MAT2 = sub_shared.GLTF_TYPE_MAT2
def GLTF_TYPE_MAT3 = sub_shared.GLTF_TYPE_MAT3

fn _gltf_pack_uv_xform_words(any a, any b) any = sub_shared._gltf_pack_uv_xform_words(a, b)

fn _pack_material_word(any a, any b, any c) any = sub_material._pack_material_word(a, b, c)
def GLTF_TYPE_MAT4 = sub_shared.GLTF_TYPE_MAT4
